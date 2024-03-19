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

#ifndef SIOCOUTQ
# define SIOCOUTQ TIOCOUTQ
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
    if (src->kn_flags & KNFL_FILE) {
        dst->data = 1; /* To match macOS and FreeBSD */
        goto done;
    }

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

    /* On return, data contains the the amount of space remaining in the write buffer */
    if (!(dst->flags & EV_EOF) && (ioctl(dst->ident, SIOCOUTQ, &dst->data) < 0)) {
            /* race condition with socket close, so ignore this error */
            dbg_puts("ioctl(2) of socket failed");
            dst->data = 0;
    }

done:
    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_write_knote_create(struct filter *filt, struct knote *kn)
{
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

        KN_UDATA(kn);   /* populate this knote's kn_udata field */
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kn_eventfd, EPOLL_EV_KN(kn->epoll_events, kn)) < 0) {
            dbg_printf("epoll_ctl(2): %s", strerror(errno));
            (void) close(evfd);
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
        if ((kev->flags & EV_CLEAR))
            return (0);
        return (-1);
    }

    return (-1);
}

int
evfilt_write_knote_delete(struct filter *filt, struct knote *kn)
{
    if ((kn->kn_flags & KNFL_FILE) && (kn->kn_eventfd != -1)) {
        if (kn->kn_registered && epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kn_eventfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        kn->kn_registered = 0;
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
