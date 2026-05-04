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

#if HAVE_LINUX_SOCKIOS_H
# include <linux/sockios.h>
#endif

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>

#ifndef SIOCOUTQ
# define SIOCOUTQ TIOCOUTQ
#endif

/*
 * F_GETPIPE_SZ requires _GNU_SOURCE; private.h sets that already
 * but the macro can still be missing in older glibc fcntl.h.  The
 * kernel constant is stable, so define it locally as a fallback.
 */
#ifndef F_GETPIPE_SZ
# define F_GETPIPE_SZ 1032
#endif

int
evfilt_write_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr)
{
    int ret;
    int serr;
    socklen_t slen = sizeof(serr);
    struct epoll_event * const ev = (struct epoll_event *) ptr;

    epoll_event_dump(ev);
    memcpy(dst, &src->kev, sizeof(*dst));

    /*
     * Fast path for files...
     */
    if (src->kn_flags & KNFL_FILE) goto done;

    if (ev->events & EPOLLHUP)
        dst->flags |= EV_EOF;

    if (ev->events & EPOLLERR) {
        if (src->kn_flags & KNFL_SOCKET) {
            ret = getsockopt(src->kev.ident, SOL_SOCKET, SO_ERROR, &serr, &slen);
            dst->fflags = ((ret < 0) ? errno : serr);
        } else
            dst->fflags = EIO;

        /*
         * The only way we seem to be able to signal an error
         * is by setting EOF on the socket.
         */
        dst->flags |= EV_EOF;
    }

    /*
     * kev.data carries free space in the write buffer.  Sockets use
     * SIOCOUTQ; pipes/FIFOs use F_GETPIPE_SZ minus FIONREAD (the
     * read-end's pending byte count, which equals the writer's
     * occupied bytes for an anonymous pipe).
     */
    if (!(dst->flags & EV_EOF)) {
        if (src->kn_flags & KNFL_PIPE) {
            int pipe_sz = fcntl((int) dst->ident, F_GETPIPE_SZ);
            int pending = 0;
            if (pipe_sz < 0 || ioctl(dst->ident, FIONREAD, &pending) < 0) {
                dbg_puts("F_GETPIPE_SZ/FIONREAD failed on pipe");
                dst->data = 0;
            } else {
                dst->data = pipe_sz - pending;
                if (dst->data < 0) dst->data = 0;
            }
        } else if (ioctl(dst->ident, SIOCOUTQ, &dst->data) < 0) {
            /* race with socket close; tolerate. */
            dbg_puts("ioctl(SIOCOUTQ) failed");
            dst->data = 0;
        }
    }

done:
    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_write_knote_create(struct filter *filt, struct knote *kn)
{
    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    if (linux_get_descriptor_type(kn) < 0)
        return (-1);

    /*
     * Epoll won't allow us to add EPOLLOUT on a regular file
     * and just fails with EPERM.
     *
     * So we add a surrogate eventfd that is always polls.
     */
    if (kn->kn_flags & KNFL_FILE) {
        int evfd;

        /* Convert the kevent into an epoll_event */
#if defined(HAVE_EPOLLRDHUP)
        kn->epoll_events = EPOLLIN | EPOLLRDHUP;
#else
        kn->epoll_events = EPOLLIN;
#endif
        if (kn->kev.flags & EV_CLEAR)
            kn->epoll_events |= EPOLLET;

        /*
         * We only set oneshot for cases where we're not going to
         * be using EPOLL_CTL_MOD.
         */
        if (kn->kev.flags & EV_ONESHOT || kn->kev.flags & EV_DISPATCH)
            kn->epoll_events |= EPOLLONESHOT;

        kn->kn_epollfd = filter_epoll_fd(filt);
        evfd = eventfd(0, 0);
        if (evfd < 0) {
            dbg_perror("eventfd(2)");
            return (-1);
        }
        /* Now the FD will be permanently high */
        if (eventfd_write(evfd, 1) < 0) {
            dbg_perror("eventfd_write(3)");
            (void) close(evfd);
            return (-1);
        }

        kn->kn_eventfd = evfd;

        KN_UDATA_ALLOC(kn);   /* populate this knote's kn_udata field */
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kn_eventfd, EPOLL_EV_KN(kn->epoll_events, kn)) < 0) {
            dbg_printf("epoll_ctl(2): %s", strerror(errno));
            (void) close(evfd);
            /* Kernel never accepted the registration; free direct. */
            KN_UDATA_FREE(kn);
            return (-1);
        }

        kn->kn_registered = 1;

        return (0);
    }

    /*
     * Convert the kevent into an epoll_event
     */
    kn->epoll_events = EPOLLOUT;

    /*
     * For EV_ONESHOT, EV_DISPATCH we rely on common code
     * disabling/deleting the event after it's fired once.
     *
     * See this SO post for details:
     * https://stackoverflow.com/questions/59517961/how-should-i-use-epoll-to-read-and-write-from-the-same-fd
     */
    if (kn->kev.flags & EV_CLEAR)
        kn->epoll_events |= EPOLLET;

    /*
     * NOTE_LOWAT on the write side is not honoured: setsockopt(SO_SNDLOWAT)
     * returns ENOPROTOOPT on Linux (socket(7): "On Linux, the socket layer
     * does not support these options.  They are just present for
     * compatibility with BSD.") and on illumos.  Silently ignored;
     * userspace gating in copyout would busy-loop on level-triggered EPOLL.
     */

    return epoll_update(EPOLL_CTL_ADD, filt, kn, kn->epoll_events, false);
}

int
evfilt_write_knote_modify(UNUSED struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    if (!(kn->kn_flags & KNFL_FILE)) {
        /*
         * This should reset the EOF sate of the socket.
         * but it's not even clear what that really means.
         *
         * With the native kqueue implementations it
         * basically does nothing.
         */
        if ((kev->flags & EV_CLEAR)) {
            /* SO_SNDLOWAT not supported on Linux, see knote_create. */

            /*
             * Common code only copies udata (and conditionally
             * EV_DISPATCH) after kn_modify.  Sync fflags/data
             * (NOTE_LOWAT and friends are caller-mutable), but
             * leave kev.flags alone: BSD treats EV_CLEAR as
             * register-only on sockets and EV_RECEIPT is sticky.
             */
            kn->kev.fflags = kev->fflags;
            kn->kev.data   = kev->data;
            return (0);
        }
        return (-1);
    }

    return (-1);
}

int
evfilt_write_knote_delete(struct filter *filt, struct knote *kn)
{
    /*
     * Restore the default SO_SNDLOWAT on socket fds the knote bumped.
     * Skipped for KNFL_FILE because the kn->kev.ident there is the
     * watched file, not a socket.
     */
    if ((kn->kev.fflags & NOTE_LOWAT) && (kn->kn_flags & KNFL_SOCKET)) {
        const int one = 1;
        if (setsockopt(kn->kev.ident, SOL_SOCKET, SO_SNDLOWAT, &one, sizeof(one)) < 0)
            dbg_perror("setsockopt(restore SO_SNDLOWAT) on EV_DELETE");
    }

    if ((kn->kn_flags & KNFL_FILE) && (kn->kn_eventfd != -1)) {
        if (kn->kn_registered && epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kn_eventfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        kn->kn_registered = 0;

        /* See evfilt_read_knote_delete for the udata-vs-fds_udata split. */
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

        (void) close(kn->kn_eventfd);
        kn->kn_eventfd = -1;
        return (0);
    }

    return epoll_update(EPOLL_CTL_DEL, filt, kn, EPOLLOUT, true);
}

int
evfilt_write_knote_enable(struct filter *filt, struct knote *kn)
{
    if (kn->kn_flags & KNFL_FILE) {
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kn_eventfd, EPOLL_EV_KN(kn->epoll_events, kn)) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        kn->kn_registered = 1;
        return (0);
    }

    return epoll_update(EPOLL_CTL_ADD, filt, kn, kn->epoll_events, false);
}

int
evfilt_write_knote_disable(struct filter *filt, struct knote *kn)
{
    if (kn->kn_flags & KNFL_FILE) {
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kn_eventfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        kn->kn_registered = 0;
        return (0);
    }

    return epoll_update(EPOLL_CTL_DEL, filt, kn, EPOLLOUT, false);
}

const struct filter evfilt_write = {
    .kf_id      = EVFILT_WRITE,
    .kf_copyout = evfilt_write_copyout,
    .kn_create  = evfilt_write_knote_create,
    .kn_modify  = evfilt_write_knote_modify,
    .kn_delete  = evfilt_write_knote_delete,
    .kn_enable  = evfilt_write_knote_enable,
    .kn_disable = evfilt_write_knote_disable,
};
