/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _KQUEUE_COMMON_EVFILT_SIGNAL_H
#define _KQUEUE_COMMON_EVFILT_SIGNAL_H

/*
 * Shared EVFILT_SIGNAL dispatch machinery.
 *
 * This header is included by exactly one .c file per platform:
 * src/posix/signal.c (sigaction + self-pipe platform layer) or
 * src/solaris/signal.c (signalfd + self-pipe platform layer).
 *
 * All function bodies here are `static` and end up file-local in
 * the includer's translation unit.  The includer must provide
 * definitions for the platform-layer hooks declared at the top of
 * this header before they are first used (forward declarations
 * suffice; the definitions come later in the same .c file or
 * before this header is included).
 *
 * The fan-out machinery is identical across backends:
 *
 *   - sigtbl[signum] -> list of sig_link surrogates, one per
 *     interested filter (= per kqueue with EVFILT_SIGNAL knotes).
 *   - Each sig_link holds enabled/disabled knote lists for that
 *     (filter, signum) pair.
 *   - On signal arrival the dispatcher walks sigtbl[sig].s_links,
 *     bumps every interested knote's kn_signal_count, links
 *     enabled ones onto the per-filter sfs_pending list, and
 *     rings each filter's eventfd.
 *   - Per-knote delivery happens in evfilt_signal_copyout, which
 *     walks sfs_pending and emits kev.data = kn_signal_count.
 *
 * RT signals queue per-fire so kev.data is an accurate count.
 * Non-RT signals coalesce in the kernel pending bitmask, so
 * back-to-back kills before a drain surface as a single bump
 * (cap of 1 per drain cycle for non-RT).  See BUGS.md for the
 * cross-thread mask requirement that signalfd-based delivery
 * imposes (and that the sigaction backend doesn't, but we adopt
 * uniformly anyway via the BUGS.md doc).
 */

/*
 * Highest signum (exclusive) for the surrogate table.  Falls back
 * to 64 on platforms that don't expose NSIG (e.g., minimal libc).
 */
#ifndef SIGNAL_MAX
#  ifdef NSIG
#    define SIGNAL_MAX NSIG
#  else
#    define SIGNAL_MAX 64
#  endif
#endif

/*
 * Per-(filter, signum) surrogate.  Holds enabled/disabled knote
 * lists for the pair.  Each knote carries kn_signal_count which
 * the dispatcher bumps per fire and copyout drains to 0.
 */
struct sig_link {
    LIST_ENTRY(sig_link)   sl_entry;        /* in sigtbl[sig].s_links */
    struct filter         *sl_filt;
    LIST_HEAD(, knote)     kn_enabled;
    LIST_HEAD(, knote)     kn_disabled;
};

/*
 * Per-filter state.  sfs_pending is the list of knotes ready for
 * copyout.  Disable unlinks from sfs_pending without resetting
 * kn_signal_count, so a later enable can re-link and deliver
 * any fires that accumulated during the disable window.
 */
struct sig_filter_state {
    struct sig_link        sfs_links[SIGNAL_MAX];
    LIST_HEAD(, knote)     sfs_pending;
};

struct sentry {
    LIST_HEAD(, sig_link)  s_links;
};

static pthread_mutex_t sigtbl_mtx = PTHREAD_MUTEX_INITIALIZER;
/*
 * UNUSED suppresses a spurious -Wunused-variable from musl-gcc;
 * sigtbl is referenced through sig_dispatch_handle which the
 * compiler doesn't always see through the static-function chain.
 */
static struct sentry   sigtbl[SIGNAL_MAX] UNUSED;

static pthread_mutex_t sig_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static int             sig_filter_count = 0;
static pthread_t       sig_dispatch_thread;
static int             sig_dispatch_started;

/*
 * Self-pipe used both for shutdown signalling (close write end ->
 * read end gets POLLHUP / read() returns 0) and, in the sigaction
 * backend, as the data channel from the AS-safe handler.
 */
static int             sig_pipe[2] = { -1, -1 };

static pthread_mutex_t sig_dispatch_thread_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sig_dispatch_thread_cond = PTHREAD_COND_INITIALIZER;

/** @name Platform-layer hooks
 *
 * Provided by the includer (src/posix/signal.c for sigaction-based
 * delivery, src/solaris/signal.c for signalfd-based delivery).  All
 * are static; the per-platform .c file forward-declares them via
 * this header and supplies the bodies later in the same TU.
 *
 * @{
 */

/** Initialise the platform layer (e.g., create signalfd).
 *
 * Called once under sig_init_mtx before the dispatch thread is
 * started.
 *
 * @return 0 on success, -1 on failure (errno set).
 */
static int  sig_platform_init(void);

/** Tear down the platform layer.
 *
 * Called once under sig_init_mtx after the dispatch thread has
 * joined.
 */
static void sig_platform_destroy(void);

/** Begin observing a signal number.
 *
 * Called with sigtbl_mtx held when the first knote across all
 * kqueues registers for this signum.
 *
 * @param[in] sig the signal number to start observing.
 * @return 0 on success, -1 on failure (errno set).
 */
static int  sig_platform_add(int sig);

/** Stop observing a signal number.
 *
 * Called with sigtbl_mtx held when the last knote across all
 * kqueues for this signum goes away.
 *
 * @param[in] sig the signal number to stop observing.
 * @return 0 on success, -1 on failure (errno set).
 */
static int  sig_platform_remove(int sig);

/** Block until at least one signal arrives (or shutdown), then
 * dispatch the arrivals via sig_dispatch_handle.
 *
 * The implementation takes/releases sigtbl_mtx around the
 * sig_dispatch_handle calls itself.
 *
 * @return  1 to keep going,
 *          0 on shutdown (self-pipe closed),
 *         -1 on unrecoverable error.
 */
static int  sig_platform_wait_dispatch(void);

/** @} */

/** @name Common dispatch machinery
 *
 * Identical across the sigaction and signalfd platform layers.
 * Each platform's signal.c picks these up by including this
 * header.
 *
 * @{
 */

/* sigtbl_mtx must be held by the caller. */
static void
sig_dispatch_handle(int sig)
{
    struct sig_link *sl;

    if (sig <= 0 || sig >= SIGNAL_MAX)
        return;

    LIST_FOREACH(sl, &sigtbl[sig].s_links, sl_entry) {
        struct sig_filter_state *sfs = sl->sl_filt->kf_signal_state;
        struct knote *kn;
        int wake = 0;

        /*
         * Bump every interested knote's fire counter.  Enabled
         * knotes also link onto the per-filter pending list (if
         * not already linked) so copyout can iterate just that
         * list rather than walking sigtbl[].  Disabled knotes
         * keep the count so a later enable can re-link and
         * deliver the deferred fires.
         */
        LIST_FOREACH(kn, &sl->kn_enabled, kn_signal_entry) {
            kn->kn_signal_count++;
            if (!LIST_INSERTED(kn, kn_signal_pending)) {
                LIST_INSERT_HEAD(&sfs->sfs_pending, kn, kn_signal_pending);
                wake = 1;
            }
        }
        LIST_FOREACH(kn, &sl->kn_disabled, kn_signal_entry)
            kn->kn_signal_count++;

        if (wake)
            (void) kqops.eventfd_raise(&sl->sl_filt->kf_efd);
    }
}

static void *
sig_dispatch_loop(UNUSED void *arg)
{
    sigset_t all;

    /*
     * Block every signal on this thread.  The sigaction backend
     * needs this so we don't recurse our own handler here; the
     * signalfd backend needs it because the kernel only queues
     * blocked signals into the pending queue that signalfd reads.
     */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    dbg_printf("signal dispatch thread started");

    pthread_mutex_lock(&sig_dispatch_thread_mtx);
    sig_dispatch_started = 1;
    pthread_cond_signal(&sig_dispatch_thread_cond);
    pthread_mutex_unlock(&sig_dispatch_thread_mtx);

    for (;;) {
        int rv = sig_platform_wait_dispatch();
        if (rv <= 0)
            break;
    }

    dbg_printf("signal dispatch thread exiting");
    return (NULL);
}

static int
sig_dispatch_start(void)
{
    /*
     * socketpair, not pipe(2): on illumos pipe(2) historically
     * yields STREAMS pipes whose half-close semantics aren't
     * reliably observable via select(2)/poll(2) read-readiness,
     * so a close(sig_pipe[1]) shutdown didn't always wake the
     * dispatch thread.  socketpair gives us a normal BSD-flavoured
     * pair where shutdown/close on one side lights up the other.
     */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sig_pipe) < 0) {
        dbg_perror("socketpair");
        return (-1);
    }
    (void) fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);
    (void) fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);

    if (sig_platform_init() < 0) {
        close(sig_pipe[0]);
        close(sig_pipe[1]);
        sig_pipe[0] = sig_pipe[1] = -1;
        return (-1);
    }

    pthread_mutex_lock(&sig_dispatch_thread_mtx);
    if (pthread_create(&sig_dispatch_thread, NULL, sig_dispatch_loop, NULL) != 0) {
        dbg_perror("pthread_create");
        pthread_mutex_unlock(&sig_dispatch_thread_mtx);
        sig_platform_destroy();
        close(sig_pipe[0]);
        close(sig_pipe[1]);
        sig_pipe[0] = sig_pipe[1] = -1;
        return (-1);
    }
    while (!sig_dispatch_started)
        pthread_cond_wait(&sig_dispatch_thread_cond, &sig_dispatch_thread_mtx);
    pthread_mutex_unlock(&sig_dispatch_thread_mtx);

    dbg_printf("signal dispatch thread up");
    return (0);
}

static void
sig_dispatch_stop(void)
{
    void *retval;
    int ret;

    /*
     * Close the write end of the self-pipe.  The dispatch thread's
     * blocked select/poll wakes (POLLHUP / read returns 0) and the
     * loop exits cleanly.  Avoids pthread_cancel.
     */
    if (sig_pipe[1] >= 0) {
        close(sig_pipe[1]);
        sig_pipe[1] = -1;
    }

    ret = pthread_join(sig_dispatch_thread, &retval);
    if (ret != 0)
        dbg_printf("pthread_join: %s", strerror(ret));
    else
        dbg_printf("dispatch thread joined");

    if (sig_pipe[0] >= 0) {
        close(sig_pipe[0]);
        sig_pipe[0] = -1;
    }
    sig_platform_destroy();
    sig_dispatch_started = 0;
}

/* sigtbl_mtx must be held. */
static int
sl_has_no_knotes(struct sig_link *sl)
{
    return LIST_EMPTY(&sl->kn_enabled) && LIST_EMPTY(&sl->kn_disabled);
}

static int
evfilt_signal_init(struct filter *filt)
{
    struct sig_filter_state *sfs;
    int sig;

    if (kqops.eventfd_init(&filt->kf_efd, filt) < 0)
        return (-1);

    /*
     * Register the eventfd with the kqueue so the consumer's wait
     * wakes when sig_dispatch_handle raises it.  No-op on backends
     * (Solaris) where the eventfd transport already routes into the
     * kqueue's wait by construction; required on Linux to attach
     * the eventfd to the kqueue's epoll set.
     */
    if (kqops.eventfd_register(filt->kf_kqueue, &filt->kf_efd) < 0) {
        kqops.eventfd_close(&filt->kf_efd);
        return (-1);
    }

    sfs = calloc(1, sizeof(*sfs));
    if (sfs == NULL) {
        kqops.eventfd_unregister(filt->kf_kqueue, &filt->kf_efd);
        kqops.eventfd_close(&filt->kf_efd);
        return (-1);
    }
    for (sig = 0; sig < SIGNAL_MAX; sig++)
        sfs->sfs_links[sig].sl_filt = filt;
    filt->kf_signal_state = sfs;

    pthread_mutex_lock(&sig_init_mtx);
    if (sig_filter_count == 0) {
        if (sig_dispatch_start() < 0) {
            pthread_mutex_unlock(&sig_init_mtx);
            free(sfs);
            filt->kf_signal_state = NULL;
            kqops.eventfd_unregister(filt->kf_kqueue, &filt->kf_efd);
            kqops.eventfd_close(&filt->kf_efd);
            return (-1);
        }
    }
    sig_filter_count++;
    pthread_mutex_unlock(&sig_init_mtx);

    return (0);
}

static void
evfilt_signal_destroy(struct filter *filt)
{
    struct sig_filter_state *sfs = filt->kf_signal_state;
    int sig;

    if (sfs != NULL) {
        pthread_mutex_lock(&sigtbl_mtx);
        for (sig = 0; sig < SIGNAL_MAX; sig++) {
            struct sig_link *sl = &sfs->sfs_links[sig];
            if (LIST_INSERTED(sl, sl_entry))
                LIST_REMOVE_ZERO(sl, sl_entry);
        }
        pthread_mutex_unlock(&sigtbl_mtx);
        free(sfs);
        filt->kf_signal_state = NULL;
    }

    kqops.eventfd_unregister(filt->kf_kqueue, &filt->kf_efd);
    kqops.eventfd_close(&filt->kf_efd);

    pthread_mutex_lock(&sig_init_mtx);
    if (--sig_filter_count == 0)
        sig_dispatch_stop();
    pthread_mutex_unlock(&sig_init_mtx);
}

static int
evfilt_signal_knote_create(struct filter *filt, struct knote *kn)
{
    int sig = (int) kn->kev.ident;
    struct sig_filter_state *sfs;
    struct sig_link *sl;
    int first;
    int rv = 0;

    if (sig <= 0 || sig >= SIGNAL_MAX) {
        dbg_printf("unsupported signal number %u",
                   (unsigned int) kn->kev.ident);
        return (-1);
    }

    kn->kev.flags |= EV_CLEAR;
    sfs = filt->kf_signal_state;
    sl = &sfs->sfs_links[sig];

    pthread_mutex_lock(&sigtbl_mtx);
    first = LIST_EMPTY(&sigtbl[sig].s_links);
    if (!LIST_INSERTED(sl, sl_entry))
        LIST_INSERT_HEAD(&sigtbl[sig].s_links, sl, sl_entry);

    /*
     * New knote starts enabled unless the caller passed EV_DISABLE
     * alongside EV_ADD.  Common code in kevent_copyin_one runs the
     * subsequent kn_disable for us if needed; insert into enabled
     * here and let that path move it.
     */
    LIST_INSERT_HEAD(&sl->kn_enabled, kn, kn_signal_entry);

    if (first) {
        if (sig_platform_add(sig) != 0) {
            int saved = errno;
            LIST_REMOVE_ZERO(kn, kn_signal_entry);
            if (sl_has_no_knotes(sl))
                LIST_REMOVE_ZERO(sl, sl_entry);
            errno = saved;
            rv = -1;
        }
    }
    pthread_mutex_unlock(&sigtbl_mtx);

    if (rv == 0)
        dbg_printf("registered knote for signal %d (first=%d)", sig, first);
    return (rv);
}

static int
evfilt_signal_knote_modify(struct filter *filt, struct knote *kn,
                const struct kevent *kev)
{
    (void) filt;
    kn->kev.flags = kev->flags | EV_CLEAR;
    return (0);
}

static int
evfilt_signal_knote_delete(struct filter *filt, struct knote *kn)
{
    int sig = (int) kn->kev.ident;
    struct sig_filter_state *sfs = filt->kf_signal_state;
    struct sig_link *sl = &sfs->sfs_links[sig];

    (void) filt;

    pthread_mutex_lock(&sigtbl_mtx);
    if (LIST_INSERTED(kn, kn_signal_entry))
        LIST_REMOVE_ZERO(kn, kn_signal_entry);
    if (LIST_INSERTED(kn, kn_signal_pending))
        LIST_REMOVE_ZERO(kn, kn_signal_pending);
    kn->kn_signal_count = 0;

    if (sl_has_no_knotes(sl) && LIST_INSERTED(sl, sl_entry))
        LIST_REMOVE_ZERO(sl, sl_entry);

    /*
     * Last knote for this signum across all filters: drop it from
     * the platform layer.  In both backends the signal stays blocked
     * on whichever thread(s) catch_signal blocked it on - we don't
     * undo that, see BUGS.md.
     */
    if (LIST_EMPTY(&sigtbl[sig].s_links))
        (void) sig_platform_remove(sig);
    pthread_mutex_unlock(&sigtbl_mtx);

    return (0);
}

static int
evfilt_signal_knote_enable(struct filter *filt, struct knote *kn)
{
    struct sig_filter_state *sfs = filt->kf_signal_state;
    struct sig_link *sl = &sfs->sfs_links[(int) kn->kev.ident];
    int wake = 0;

    pthread_mutex_lock(&sigtbl_mtx);
    if (LIST_INSERTED(kn, kn_signal_entry))
        LIST_REMOVE_ZERO(kn, kn_signal_entry);
    LIST_INSERT_HEAD(&sl->kn_enabled, kn, kn_signal_entry);

    /*
     * Signals that arrived during the disabled window stayed
     * counted on the knote; relink onto the pending list and
     * wake the consumer so the deferred fires deliver.
     */
    if (kn->kn_signal_count > 0 && !LIST_INSERTED(kn, kn_signal_pending)) {
        LIST_INSERT_HEAD(&sfs->sfs_pending, kn, kn_signal_pending);
        wake = 1;
    }
    pthread_mutex_unlock(&sigtbl_mtx);

    if (wake)
        (void) kqops.eventfd_raise(&filt->kf_efd);
    return (0);
}

static int
evfilt_signal_knote_disable(struct filter *filt, struct knote *kn)
{
    struct sig_filter_state *sfs = filt->kf_signal_state;
    struct sig_link *sl = &sfs->sfs_links[(int) kn->kev.ident];

    (void) filt;

    pthread_mutex_lock(&sigtbl_mtx);
    if (LIST_INSERTED(kn, kn_signal_entry))
        LIST_REMOVE_ZERO(kn, kn_signal_entry);
    /*
     * Unlink from the pending list so we don't deliver while
     * disabled.  Keep kn_signal_count intact so a later enable
     * sees the deferred fires and re-links.
     */
    if (LIST_INSERTED(kn, kn_signal_pending))
        LIST_REMOVE_ZERO(kn, kn_signal_pending);
    LIST_INSERT_HEAD(&sl->kn_disabled, kn, kn_signal_entry);
    pthread_mutex_unlock(&sigtbl_mtx);
    return (0);
}

static int
evfilt_signal_copyout(struct kevent *dst, int nevents, struct filter *filt,
    struct knote *src UNUSED, void *ptr UNUSED)
{
    struct sig_filter_state *sfs = filt->kf_signal_state;
    struct knote *kn, *kn_tmp;
    /*
     * Snapshot the knotes we're emitting under sigtbl_mtx so we
     * can call knote_copyout_flag_actions() AFTER unlocking.
     * EV_ONESHOT/EV_DISPATCH bounce through knote_delete/disable,
     * which themselves take sigtbl_mtx; calling them inline would
     * recursively lock and deadlock.
     */
    struct knote *emitted[nevents];
    int n_emitted = 0;
    int rv = 0;

    pthread_mutex_lock(&sigtbl_mtx);
    LIST_FOREACH_SAFE(kn, &sfs->sfs_pending, kn_signal_pending, kn_tmp) {
        if (n_emitted >= nevents)
            break;

        dst[n_emitted].ident  = kn->kev.ident;
        dst[n_emitted].filter = EVFILT_SIGNAL;
        dst[n_emitted].udata  = kn->kev.udata;
        dst[n_emitted].flags  = kn->kev.flags;
        dst[n_emitted].fflags = 0;
        dst[n_emitted].data   = kn->kn_signal_count;

        LIST_REMOVE_ZERO(kn, kn_signal_pending);
        kn->kn_signal_count = 0;
        emitted[n_emitted++] = kn;
    }
    pthread_mutex_unlock(&sigtbl_mtx);

    for (int i = 0; i < n_emitted; i++) {
        if (knote_copyout_flag_actions(filt, emitted[i]) < 0) {
            rv = -1;
            break;
        }
    }
    if (rv < 0)
        return (-1);

    kqops.eventfd_lower(&filt->kf_efd);
    return (n_emitted);
}

/** @} */

#endif  /* ! _KQUEUE_COMMON_EVFILT_SIGNAL_H */
