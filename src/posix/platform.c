/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
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

#include "../common/private.h"
#include "platform.h"
#include "eventfd.h"

/*
 * Self-pipe used as the kqueue's "fd" handle.  pipe(2)+fcntl is
 * used unconditionally so the same code path runs on hosts that
 * lack pipe2(2) (Darwin).
 */
static int
posix_self_pipe(int fd[2])
{
    int i;

    if (pipe(fd) < 0) {
        dbg_perror("pipe(2)");
        return (-1);
    }
    for (i = 0; i < 2; i++) {
        int fl = fcntl(fd[i], F_GETFL, 0);
        if (fl < 0 ||
            fcntl(fd[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0) {
            dbg_perror("fcntl(2)");
            if (close(fd[0]) < 0)
                dbg_perror("close(fd[0]) on cleanup");
            if (close(fd[1]) < 0)
                dbg_perror("close(fd[1]) on cleanup");
            return (-1);
        }
    }
    return (0);
}

/*
 * Wake any parked pselect on this kqueue.  Writing a byte to
 * kq_wake_wfd makes its read-side (kq_id, registered in kq_fds)
 * readable, so the parked caller's next pselect iteration sees
 * the new fd_set state.  EAGAIN on a full pipe is benign - the
 * pipe is already primed.
 */
void
posix_wake_kqueue(struct kqueue *kq)
{
    ssize_t rv;

    if (kq->kq_wake_wfd < 0)
        return;
    rv = write(kq->kq_wake_wfd, "K", 1);
    (void) rv;                          /* silence -Wunused-result */
}

/*
 * In-flight tracking for KEVENT_WAIT_DROP_LOCK.  Common code in
 * kevent.c calls these around the kevent_wait/copyout pair so
 * kqueue_free can defer destruction until the last in-flight
 * caller drops out.
 */
void
posix_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);
    TAILQ_INSERT_TAIL(&kq->kq_inflight, state, entry);
}

void
posix_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);
    TAILQ_REMOVE(&kq->kq_inflight, state, entry);
}

/*
 * Set the per-kqueue file-knote poll interval (NOTE_FILE_POLL_INTERVAL
 * on EVFILT_LIBKQUEUE).  Negative is rejected; 0 disables auto-poll
 * (kqueue sleeps indefinitely with file knotes registered,
 * re-evaluation only on other wake sources).  Positive values clamp
 * pselect's timeout when the only readiness sources are KNFL_FILE
 * knotes.
 */
static int
posix_set_file_poll_interval(struct kqueue *kq, intptr_t interval_ns)
{
    if (interval_ns < 0) {
        errno = EINVAL;
        return (-1);
    }
    kq->kq_file_poll_interval_ns = (long) interval_ns;
    posix_wake_kqueue(kq);              /* parked pselects need to re-clamp */
    return (0);
}

int
posix_kqueue_init(struct kqueue *kq)
{
    int sd[2];

    if (posix_self_pipe(sd) < 0)
        return (-1);

    if (sd[0] >= FD_SETSIZE) {
        dbg_printf("self-pipe read end fd=%d >= FD_SETSIZE=%d; too many open files",
                   sd[0], FD_SETSIZE);
        close(sd[0]);
        close(sd[1]);
        errno = EMFILE;
        return (-1);
    }

    TAILQ_INIT(&kq->kq_inflight);
    RB_INIT(&kq->kq_timers);

    /*
     * Hand the read end back as the kqueue's identifying fd; the
     * write end is reserved for cross-thread wakeups (future work).
     * Adding the read end to kq_fds means a user-side close(kq) is
     * surfaced to pselect(2) as readiness, but we do not currently
     * react to that beyond making the wait return.
     */
    kq->kq_id = sd[0];
    FD_ZERO(&kq->kq_fds);
    FD_ZERO(&kq->kq_wfds);
    FD_SET(sd[0], &kq->kq_fds);
    kq->kq_nfds = sd[0] + 1;

    kq->kq_wake_wfd = sd[1];

    if (filter_register_all(kq) < 0) {
        if (close(sd[0]) < 0)
            dbg_perror("close(sd[0]) on cleanup");
        if (close(sd[1]) < 0)
            dbg_perror("close(sd[1]) on cleanup");
        kq->kq_id = -1;
        kq->kq_wake_wfd = -1;
        return (-1);
    }

    return (0);
}

void
posix_kqueue_free(struct kqueue *kq)
{
    /*
     * kq_id is the user-facing fd we handed back from kqueue();
     * the user is expected to close(2) it.  We must NOT close it
     * ourselves: posix_kqueue_free runs (a) at process exit via
     * libkqueue_free, where the user has likely already closed it
     * and the fd number may have been recycled by the OS for an
     * unrelated open, and (b) inline from kqueue_free_by_id when
     * a brand-new kqueue() reuses that fd number, in which case
     * kq_id is the *new* kqueue's pipe and our close(2) would
     * pull the rug from under it.  Without a monitoring thread
     * (linux/platform.c) we have no other way to detect ownership;
     * skip the close and accept the at-most one-fd-per-kqueue
     * leak when the caller forgets close().  kq_wake_wfd is the
     * write end of the same self-pipe, internal to libkqueue, so
     * it's always safe (and correct) to close.
     */
    kq->kq_id = -1;
    if (kq->kq_wake_wfd >= 0) {
        if (close(kq->kq_wake_wfd) < 0)
            dbg_perror("close(kq_wake_wfd)");
        kq->kq_wake_wfd = -1;
    }
    FD_ZERO(&kq->kq_fds);
    kq->kq_nfds = 0;
}

/*
 * Add the eventfd's read end to the kqueue's master fd_set so the
 * filter's eventfd_raise() wakes the next pselect.  The unregister
 * path is symmetric.  These are no-ops if ef_id is not a valid fd.
 */
int
posix_eventfd_register(struct kqueue *kq, struct eventfd *efd)
{
    int fd = efd->ef_id;

    if (fd < 0)
        return (0);
    if (fd >= FD_SETSIZE) {
        dbg_printf("eventfd fd=%d >= FD_SETSIZE=%d", fd, FD_SETSIZE);
        errno = EMFILE;
        return (-1);
    }
    FD_SET(fd, &kq->kq_fds);
    if (fd >= kq->kq_nfds)
        kq->kq_nfds = fd + 1;
    /*
     * Stash the eventfd's read descriptor on the owning filter so
     * posix_kevent_copyout can identify "this filter fired" without
     * needing to know each filter's storage layout.  Filters that
     * don't go through eventfd_register (READ/WRITE) are dispatched
     * by their own kf_id check.
     */
    if (efd->ef_filt != NULL)
        efd->ef_filt->kf_pfd = fd;
    dbg_printf("registered eventfd fd=%d (nfds=%d)", fd, kq->kq_nfds);
    return (0);
}

void
posix_eventfd_unregister(struct kqueue *kq, struct eventfd *efd)
{
    int fd = efd->ef_id;

    if (fd < 0 || fd >= FD_SETSIZE)
        return;
    FD_CLR(fd, &kq->kq_fds);
    if (efd->ef_filt != NULL && efd->ef_filt->kf_pfd == fd)
        efd->ef_filt->kf_pfd = -1;
    dbg_printf("unregistered eventfd fd=%d", fd);
}

int
posix_kevent_wait(struct kqueue *kq, UNUSED int numevents, const struct timespec *timeout)
{
    int n;
    fd_set rfds, wfds;
    struct timespec file_poll_to;

    /*
     * Always-ready knotes (EVFILT_VNODE, EVFILT_READ on regular
     * files) have no kernel-level wake source - their state is
     * derived from periodic fstat.  Whenever ANY always-ready
     * knote is registered we must drive the pselect loop, even
     * if non-always-ready knotes are also registered: the
     * non-always-ready ones might never fire while the
     * always-ready one is the thing that actually changed.
     *
     *  - kq_file_poll_interval_ns > 0: clamp pselect's timeout to
     *    that many ns (the kqueue actually sleeps between polls).
     *  - kq_file_poll_interval_ns == 0 (default): sched_yield()
     *    each loop iteration with timeout=0 (cooperative spin).
     *    Only safe when the entire knote set is always-ready;
     *    otherwise the spin would burn CPU forever waiting on
     *    a co-resident kernel-wake knote.
     */
    bool any_always_ready  = (kq->kq_always_ready > 0);
    bool only_always_ready = any_always_ready &&
                             (kq->kq_knote_count == kq->kq_always_ready);
    if (any_always_ready) {
        /*
         * If the user set NOTE_FILE_POLL_INTERVAL, honour it.
         * Otherwise pick a 100ms default so a caller blocking on
         * kevent(timeout=NULL) wakes up periodically to drive
         * the always-ready knote's stat-poll.  The cooperative
         * sched_yield path below kicks in additionally when the
         * entire knote set is always-ready.
         */
        long ns = kq->kq_file_poll_interval_ns;
        if (ns <= 0) ns = 100L * 1000L * 1000L;  /* 100ms default */
        file_poll_to.tv_sec  = ns / 1000000000L;
        file_poll_to.tv_nsec = ns % 1000000000L;
        if (timeout == NULL ||
            timeout->tv_sec > file_poll_to.tv_sec ||
            (timeout->tv_sec == file_poll_to.tv_sec &&
             timeout->tv_nsec > file_poll_to.tv_nsec))
            timeout = &file_poll_to;
    }

    for (;;) {
        static const struct timespec zero = { 0, 0 };
        char buf[64];
        bool real_event;
        struct timespec timer_to;
        const struct timespec *use_to = timeout;
        long timer_ns;

        if (only_always_ready && kq->kq_file_poll_interval_ns == 0) {
            sched_yield();
            use_to = &zero;
        }

        /*
         * Clamp the pselect timeout against the next EVFILT_TIMER
         * deadline so timer fires drive wakeups without a sleeper
         * thread.  -1 means "no timer constrains us".
         */
        timer_ns = posix_timer_min_deadline_ns(kq);
        if (timer_ns >= 0) {
            timer_to.tv_sec  = timer_ns / 1000000000L;
            timer_to.tv_nsec = timer_ns % 1000000000L;
            if (use_to == NULL ||
                use_to->tv_sec > timer_to.tv_sec ||
                (use_to->tv_sec == timer_to.tv_sec &&
                 use_to->tv_nsec > timer_to.tv_nsec))
                use_to = &timer_to;
        }

        /*
         * Drain wake bytes left in kq_id from cross-thread arms
         * or same-thread copyin paths.  Without this, an EV_ADD
         * that primed the wake-pipe inside the same kevent() call
         * would make pselect return immediately on a synthetic
         * "kqueue changed" signal; copyout then finds nothing
         * really ready and returns 0, surfacing to the caller of
         * kevent(...,timeout=NULL) as a spurious 0 return.  The
         * pselect snapshot below already incorporates the
         * up-to-date kq_fds, so the wake byte itself is redundant
         * once we've reloaded.
         */
        if (kq->kq_id >= 0)
            while (read(kq->kq_id, buf, sizeof(buf)) > 0)
                /* repeat */;

        rfds = kq->kq_fds;
        wfds = kq->kq_wfds;

        dbg_printf("waiting on nfds=%d (always_ready=%d)",
                   kq->kq_nfds, kq->kq_always_ready);
        n = pselect(kq->kq_nfds, &rfds, &wfds, NULL, use_to, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                dbg_puts("pselect: EINTR");
                return (0);
            }
            dbg_perror("pselect(2)");
            return (-1);
        }

        kq->kq_rfds = rfds;
        kq->kq_wrfds = wfds;

        /*
         * A timer expiry presents to pselect as a plain timeout
         * (n == 0).  Run the timer sweep so copyout sees fresh
         * fire counts; if anything fired, force a copyout pass.
         */
        if (n == 0) {
            posix_timer_check(kq);
            if (kq->kq_always_ready > 0)
                return (1);
            if (timer_ns >= 0 && (use_to == &timer_to))
                return (1);              /* let copyout drain timers */
            return (0);                  /* genuine user timeout */
        }

        /*
         * Determine whether pselect saw a real fd transition or
         * only the wake-pipe.  Clear kq_id from the read snapshot
         * and see if anything remains; if both rfds and wfds are
         * empty, this was a spurious wake.  When the caller asked
         * to block (timeout NULL) we loop; otherwise return n and
         * let copyout do its thing (it will just produce zero
         * events, matching kevent's "timed out" semantics).
         */
        if (kq->kq_id >= 0 && FD_ISSET(kq->kq_id, &rfds))
            FD_CLR(kq->kq_id, &rfds);
        real_event = false;
        for (int fd = 0; fd < kq->kq_nfds; fd++) {
            if (FD_ISSET(fd, &rfds) || FD_ISSET(fd, &wfds)) {
                real_event = true;
                break;
            }
        }
        if (real_event || timeout != NULL)
            return (n);
        /* Spurious wake on a NULL-timeout caller - reload and re-arm. */
    }
}

/*
 * Drain pending knotes from a single eventfd-driven filter.  The
 * filter's kf_ready list holds the knotes that the trigger path
 * appended; we walk it and let each one's kf_copyout populate one
 * or more output kevents.  EV_DISPATCH/EV_ONESHOT bookkeeping is
 * handled inside the filter copyout via knote_copyout_flag_actions.
 */
static int
posix_dispatch_filter(struct filter *filt, struct kevent *eventlist,
        int nevents)
{
    struct knote *kn, *kn_tmp;
    int n = 0;
    int rv;

    /*
     * Two dispatch styles share this path:
     *
     *  - Per-knote filters (USER): the knotes that need delivery
     *    are linked on filt->kf_ready and copyout emits one event
     *    per call.  Walk the list, calling copyout per entry.
     *
     *  - Drain filters (SIGNAL): the filter keeps its own internal
     *    pending structure (e.g. sfs_pending) and emits whatever
     *    is ready in a single call regardless of `src`.  Only the
     *    SIGNAL filter opts in; calling kf_copyout with src=NULL
     *    on a per-knote filter would dereference garbage.  An
     *    empty kf_ready for any other filter just means the
     *    eventfd was raised spuriously - drain it (the caller
     *    already lowered the level) and emit nothing.
     */
    if (LIST_EMPTY(&filt->kf_ready)) {
        /*
         * Drain-style filters: copyout walks the filter's own
         * knote tree (under kq_mtx) when the filter eventfd
         * fires.  The src argument is unused.
         */
        if (
#ifdef EVFILT_SIGNAL
            filt->kf_id == EVFILT_SIGNAL ||
#endif
#ifdef EVFILT_TIMER
            filt->kf_id == EVFILT_TIMER ||
#endif
#ifdef EVFILT_VNODE
            filt->kf_id == EVFILT_VNODE ||
#endif
            0) {
            rv = filt->kf_copyout(eventlist, nevents, filt, NULL, NULL);
            if (rv < 0)
                return (-1);
            return rv;
        }
        return (0);
    }

    LIST_FOREACH_SAFE(kn, &filt->kf_ready, kn_ready, kn_tmp) {
        if (n >= nevents)
            break;

        /*
         * Detach from kf_ready BEFORE invoking copyout: an
         * EV_ONESHOT knote deletes itself inside copyout via
         * knote_copyout_flag_actions -> knote_delete -> the
         * final knote_release frees the underlying storage,
         * and a post-copyout LIST_REMOVE_ZERO would walk freed
         * memory.  Detaching up front also closes the window
         * where a coalesced re-trigger from another thread sees
         * the entry still on kf_ready and skips its
         * LIST_INSERT_HEAD (correct: we're about to deliver).
         *
         * LIST_REMOVE_ZERO clears next/prev so a later
         * LIST_INSERTED check (e.g. inside knote_delete's own
         * cleanup) sees the entry as detached.
         */
        LIST_REMOVE_ZERO(kn, kn_ready);

        rv = filt->kf_copyout(eventlist + n, nevents - n, filt, kn, NULL);
        if (rv < 0) {
            dbg_puts("kf_copyout failed");
            return (-1);
        }
        if (rv > 0)
            n += rv;
    }

    return (n);
}

/*
 * Walk an fd-keyed filter's knotes and emit one kevent per knote
 * whose descriptor is readable in the kqueue's last pselect result.
 */
struct posix_dispatch_fd_ctx {
    struct filter   *filt;
    fd_set          *active;
    struct kevent   *eventlist;
    int             nevents;
    int             nout;
    int             err;
};

static int
posix_dispatch_fd_cb(struct knote *kn, void *uctx)
{
    struct posix_dispatch_fd_ctx *ctx = uctx;
    int fd = (int)kn->kev.ident;
    bool always_ready = (kn->kn_flags & KNFL_FILE) != 0;
    fd_set *master;
    int rv;

    if (ctx->nout >= ctx->nevents)
        return (1);                    /* stop iteration, output buffer full */
    if (KNOTE_DISABLED(kn))
        return (0);
    /*
     * Regular files are "always readable" up to EOF; we don't
     * gate them on FD_ISSET because they can't be select()-armed.
     * Everything else has to show up in the post-pselect set AND
     * still be present in the master watch set.  The master check
     * is the single-delivery gate under KEVENT_WAIT_DROP_LOCK:
     * once one waiter's EV_CLEAR copyout disarms the fd, later
     * waiters with stale snapshots see the CLR and quietly drop
     * the duplicate emit.
     */
    master = (ctx->active == &ctx->filt->kf_kqueue->kq_rfds)
        ? &ctx->filt->kf_kqueue->kq_fds
        : &ctx->filt->kf_kqueue->kq_wfds;
    if (!always_ready) {
        if (fd < 0 || fd >= FD_SETSIZE)
            return (0);
        if (!FD_ISSET(fd, ctx->active))
            return (0);
        if (!FD_ISSET(fd, master))
            return (0);
    }

    rv = ctx->filt->kf_copyout(ctx->eventlist + ctx->nout,
                               ctx->nevents - ctx->nout,
                               ctx->filt, kn, NULL);
    if (rv < 0) {
        ctx->err = -1;
        return (1);                    /* abort iteration */
    }
    ctx->nout += rv;
    return (0);
}

static int
posix_dispatch_fd_filter(struct filter *filt, fd_set *active,
        struct kevent *eventlist, int nevents)
{
    struct posix_dispatch_fd_ctx ctx = {
        .filt = filt, .active = active,
        .eventlist = eventlist, .nevents = nevents,
        .nout = 0, .err = 0,
    };

    (void) knote_foreach(filt, posix_dispatch_fd_cb, &ctx);
    return ctx.err < 0 ? -1 : ctx.nout;
}

/*
 * Drain the kqueue's wake-pipe.  Filters write a byte to
 * kq_wake_wfd when they need pselect to return promptly even
 * though no kernel-level fd has changed state (the canonical
 * trigger is a regular-file READ knote being added: such knotes
 * are always-ready and dispatched on every wait).
 */
static void
posix_kevent_drain_wake(struct kqueue *kq)
{
    char buf[64];

    if (kq->kq_id < 0)
        return;
    if (!FD_ISSET(kq->kq_id, &kq->kq_rfds))
        return;
    while (read(kq->kq_id, buf, sizeof(buf)) > 0)
        /* repeat */;
}

int
posix_kevent_copyout(struct kqueue *kq, UNUSED int nready,
        struct kevent *eventlist, int nevents)
{
    struct filter *filt;
    int i, rv, nout = 0;

    posix_kevent_drain_wake(kq);

    for (i = 0; i < NUM_ELEMENTS(kq->kq_filt); i++) {
        if (nout >= nevents)
            break;

        filt = &kq->kq_filt[i];
        if (filt->kf_id == 0)
            continue;

        /*
         * READ/WRITE are fd-keyed: dispatch one event per knote
         * whose descriptor showed up in the post-pselect set.
         */
#ifdef EVFILT_READ
        if (filt->kf_id == EVFILT_READ) {
            rv = posix_dispatch_fd_filter(filt, &kq->kq_rfds,
                    eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }
#endif
#ifdef EVFILT_WRITE
        if (filt->kf_id == EVFILT_WRITE) {
            rv = posix_dispatch_fd_filter(filt, &kq->kq_wrfds,
                    eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }
#endif

#ifdef EVFILT_TIMER
        /*
         * EVFILT_TIMER has no eventfd: it's driven entirely from
         * the pselect timeout clamp.  Run its copyout every pass
         * and let it walk kq_timers for fired entries.
         */
        if (filt->kf_id == EVFILT_TIMER) {
            rv = posix_dispatch_filter(filt, eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }
#endif
#ifdef EVFILT_VNODE
        /*
         * EVFILT_VNODE on POSIX is fstat-snapshot polling: no
         * eventfd, knotes don't link to kf_ready, copyout walks
         * the knote index itself.  Run it every pass; the wait
         * loop's poll-interval mechanism gates how often "every
         * pass" actually happens.
         */
        if (filt->kf_id == EVFILT_VNODE) {
            rv = posix_dispatch_filter(filt, eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }
#endif

        /*
         * Eventfd-keyed filters (USER, PROC, SIGNAL):
         * a non-empty kf_pfd that turned readable means the
         * filter has knotes queued on its kf_ready list.  Drain
         * the eventfd and let the filter's copyout emit them.
         */
        if (filt->kf_pfd <= 0)
            continue;
        if (!FD_ISSET(filt->kf_pfd, &kq->kq_rfds))
            continue;

        dbg_printf("draining filter %s (pfd=%d)",
                   filter_name(filt->kf_id), filt->kf_pfd);

        kqops.eventfd_lower(&filt->kf_efd);

        rv = posix_dispatch_filter(filt, eventlist + nout, nevents - nout);
        if (rv < 0)
            return (-1);
        nout += rv;
    }

    return (nout);
}

const struct kqueue_vtable kqops = {
    .kqueue_init        = posix_kqueue_init,
    .kqueue_free        = posix_kqueue_free,
    .kevent_wait        = posix_kevent_wait,
    .kevent_copyout     = posix_kevent_copyout,
    .eventfd_register   = posix_eventfd_register,
    .eventfd_unregister = posix_eventfd_unregister,
    .eventfd_init       = posix_eventfd_init,
    .eventfd_close      = posix_eventfd_close,
    .eventfd_raise      = posix_eventfd_raise,
    .eventfd_lower      = posix_eventfd_lower,
    .eventfd_descriptor = posix_eventfd_descriptor,
    .set_file_poll_interval = posix_set_file_poll_interval,
};
