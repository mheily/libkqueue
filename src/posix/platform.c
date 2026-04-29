/*
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
            (void) close(fd[0]);
            (void) close(fd[1]);
            return (-1);
        }
    }
    return (0);
}

int
posix_kqueue_init(struct kqueue *kq)
{
    int sd[2];

    if (posix_self_pipe(sd) < 0)
        return (-1);

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
        (void) close(sd[0]);
        (void) close(sd[1]);
        kq->kq_id = -1;
        kq->kq_wake_wfd = -1;
        return (-1);
    }

    return (0);
}

void
posix_kqueue_free(struct kqueue *kq)
{
    if (kq->kq_id >= 0) {
        (void) close(kq->kq_id);
        kq->kq_id = -1;
    }
    if (kq->kq_wake_wfd >= 0) {
        (void) close(kq->kq_wake_wfd);
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
    struct timespec zero = { 0, 0 };

    rfds = kq->kq_fds;
    wfds = kq->kq_wfds;

    /*
     * If any always-ready knote (e.g. EVFILT_READ on a regular
     * file) is registered, force pselect to return immediately.
     * The dispatch path emits those knotes unconditionally; we
     * must not block waiting for an unrelated wake source.
     */
    if (kq->kq_always_ready > 0)
        timeout = &zero;

    dbg_printf("waiting on nfds=%d (always_ready=%d)",
               kq->kq_nfds, kq->kq_always_ready);
    n = pselect(kq->kq_nfds, &rfds, &wfds, NULL, timeout, NULL);
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
     * Always-ready knotes still need dispatch even when pselect
     * timed out with no real fd activity.  Returning > 0 makes
     * the common kevent_copyout path call our copyout.
     */
    if (n == 0 && kq->kq_always_ready > 0)
        return (1);
    return (n);
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
     *    is ready in a single call regardless of `src`.  We detect
     *    this by an empty kf_ready and invoke copyout once with
     *    src=NULL; the filter walks its own list.
     */
    if (LIST_EMPTY(&filt->kf_ready)) {
        rv = filt->kf_copyout(eventlist, nevents, filt, NULL, NULL);
        if (rv < 0)
            return (-1);
        return rv;
    }

    LIST_FOREACH_SAFE(kn, &filt->kf_ready, kn_ready, kn_tmp) {
        if (n >= nevents)
            break;

        rv = filt->kf_copyout(eventlist + n, nevents - n, filt, kn, NULL);
        if (rv < 0) {
            dbg_puts("kf_copyout failed");
            return (-1);
        }
        LIST_REMOVE(kn, kn_ready);
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
    int rv;

    if (ctx->nout >= ctx->nevents)
        return (1);                    /* stop iteration, output buffer full */
    if (KNOTE_DISABLED(kn))
        return (0);
    /*
     * Regular files are "always readable" up to EOF; we don't
     * gate them on FD_ISSET because they can't be select()-armed.
     * Everything else has to show up in the post-pselect set.
     */
    if (!always_ready) {
        if (fd < 0 || fd >= FD_SETSIZE)
            return (0);
        if (!FD_ISSET(fd, ctx->active))
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
        if (filt->kf_id == EVFILT_READ) {
            rv = posix_dispatch_fd_filter(filt, &kq->kq_rfds,
                    eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }
        if (filt->kf_id == EVFILT_WRITE) {
            rv = posix_dispatch_fd_filter(filt, &kq->kq_wrfds,
                    eventlist + nout, nevents - nout);
            if (rv < 0)
                return (-1);
            nout += rv;
            continue;
        }

        /*
         * Eventfd-keyed filters (USER, PROC, SIGNAL, TIMER):
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
};
