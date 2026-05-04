/*
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
#include "private.h"

#include <signal.h>
#include <sys/socket.h>


int
linux_evfilt_user_knote_enable(struct filter *filt, struct knote *kn);

int
linux_evfilt_user_knote_disable(struct filter *filt, struct knote *kn);

/* NOTE: copy+pasted from linux_eventfd_raise() */
static int
eventfd_raise(int evfd)
{
    uint64_t counter;
    int rv = 0;

    dbg_printf("event_fd=%i - raising event level", evfd);
    counter = 1;
    if (write(evfd, &counter, sizeof(counter)) < 0) {
        switch (errno) {
            case EAGAIN:
                /* Not considered an error */
                break;

            case EINTR:
                rv = -EINTR;
                break;

            default:
                dbg_printf("write(2): %s", strerror(errno));
                rv = -1;
        }
    }
    return (rv);
}

/* NOTE: copy+pasted from linux_eventfd_lower()
 *
 * Drains the eventfd counter.  The eventfd is created O_NONBLOCK so a
 * concurrent reader (e.g. another thread that woke from epoll_wait on
 * the same kq) can race us to the read; the loser sees EAGAIN.
 *
 * @return
 *      - 1  drained the counter (this caller "owns" the trigger).
 *      - 0  EAGAIN: another reader already drained it; caller must
 *           not dispatch a kevent for this wakeup.
 *      - -1 read failed for some other reason.
 *      - -EINTR  interrupted by a signal.
 */
static int
eventfd_lower(int evfd)
{
    uint64_t cur;
    ssize_t n;

    /* Reset the counter */
    dbg_printf("event_fd=%i - lowering event level", evfd);
    n = read(evfd, &cur, sizeof(cur));
    if (n < 0) {
        switch (errno) {
            case EAGAIN:
                return (0);

            case EINTR:
                return (-EINTR);

            default:
                dbg_printf("read(2): %s", strerror(errno));
                return (-1);
        }
    }
    if (n != sizeof(cur)) {
        dbg_puts("short read");
        return (-1);
    }

    return (1);
}

int
linux_evfilt_user_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr UNUSED)
{
    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags &= ~NOTE_FFCTRLMASK;     //FIXME: Not sure if needed
    dst->fflags &= ~NOTE_TRIGGER;
    if (src->kev.flags & EV_CLEAR)
        src->kev.fflags &= ~NOTE_TRIGGER;
    if (src->kev.flags & (EV_DISPATCH | EV_CLEAR | EV_ONESHOT)) {
        int rv = eventfd_lower(src->kn_user.eventfd);
        if (rv < 0) return (-1);
        /*
         * Another waiter on the same kq drained the eventfd before
         * we did - skip dispatch.  Without this, a blocking read on
         * the eventfd would deadlock multi-waiter setups; with it,
         * exactly one waiter dispatches per NOTE_TRIGGER and the
         * losers return zero events from this slot.
         */
        if (rv == 0) return (0);
    }

    if (src->kev.flags & EV_DISPATCH)
        src->kev.fflags &= ~NOTE_TRIGGER;

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
linux_evfilt_user_knote_create(struct filter *filt, struct knote *kn)
{
    int evfd;

    /*
     * EFD_NONBLOCK so concurrent readers (multiple kevent() callers
     * on the same kq racing for the same NOTE_TRIGGER) can detect
     * "another thread already drained me" via EAGAIN rather than
     * blocking forever in read().  See eventfd_lower() and
     * linux_evfilt_user_copyout() for the consumer side.
     */
    evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (evfd < 0) {
        if ((errno == EMFILE) || (errno == ENFILE)) {
            dbg_perror("eventfd(2) fd_used=%u fd_max=%u", get_fd_used(), get_fd_limit());
        } else {
            dbg_perror("eventfd(2)");
        }
        close(evfd);
        kn->kn_user.eventfd = -1;
        return (-1);
    }

    dbg_printf("event_fd=%i - created", evfd);
    kn->kn_user.eventfd = evfd;
    KN_UDATA_ALLOC(kn);   /* populate this knote's kn_udata field */

    if (KNOTE_ENABLED(kn)) {
        int rv = linux_evfilt_user_knote_enable(filt, kn);
        if (rv < 0) {
            /* enable's epoll_ctl(EPOLL_CTL_ADD) failed.  Kernel
             * never saw the udata, free direct. */
            KN_UDATA_FREE(kn);
        }
        return rv;
    }

    return (0);
}

int
linux_evfilt_user_knote_modify(struct filter *filt UNUSED, struct knote *kn, const struct kevent *kev)
{
    unsigned int ffctrl;
    unsigned int fflags;

    /* Excerpted from sys/kern/kern_event.c in FreeBSD HEAD */
    ffctrl = kev->fflags & NOTE_FFCTRLMASK;
    fflags = kev->fflags & NOTE_FFLAGSMASK;
    switch (ffctrl) {
        case NOTE_FFNOP:
            break;

        case NOTE_FFAND:
            kn->kev.fflags &= fflags;
            break;

        case NOTE_FFOR:
            kn->kev.fflags |= fflags;
            break;

        case NOTE_FFCOPY:
            kn->kev.fflags = fflags;
            break;

        default:
            /* XXX Return error? */
            break;
    }

    if ((!(kn->kev.flags & EV_DISABLE)) && kev->fflags & NOTE_TRIGGER) {
        kn->kev.fflags |= NOTE_TRIGGER;
        if (eventfd_raise(kn->kn_user.eventfd) < 0)
            return (-1);
    }

    return (0);
}

int
linux_evfilt_user_knote_delete(struct filter *filt, struct knote *kn)
{
    int rv = 0;

    if (KNOTE_ENABLED(kn))
        linux_evfilt_user_knote_disable(filt, kn);

    KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

    dbg_printf("event_fd=%i - closed", kn->kn_user.eventfd);
    if (close(kn->kn_user.eventfd) < 0) {
        dbg_perror("close(2)");
        return (-1);
    }
    kn->kn_user.eventfd = -1;

    return rv;
}

int
linux_evfilt_user_knote_enable(struct filter *filt, struct knote *kn)
{
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_ADD, kn->kn_user.eventfd, EPOLL_EV_KN(EPOLLIN, kn)) < 0) {
        dbg_perror("epoll_ctl(2)");
        return (-1);
    }
    dbg_printf("event_fd=%i - added to epoll_fd=%i", kn->kn_user.eventfd, filter_epoll_fd(filt));

    return (0);
}

int
linux_evfilt_user_knote_disable(struct filter *filt, struct knote *kn)
{
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_DEL, kn->kn_user.eventfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
    }
    dbg_printf("event_fd=%i - removed from epoll_fd=%i", kn->kn_user.eventfd, filter_epoll_fd(filt));

    return (0);
}

const struct filter evfilt_user = {
    .kf_id      = EVFILT_USER,
    .kf_copyout = linux_evfilt_user_copyout,
    .kn_create  = linux_evfilt_user_knote_create,
    .kn_modify  = linux_evfilt_user_knote_modify,
    .kn_delete  = linux_evfilt_user_knote_delete,
    .kn_enable  = linux_evfilt_user_knote_enable,
    .kn_disable = linux_evfilt_user_knote_disable,
};
