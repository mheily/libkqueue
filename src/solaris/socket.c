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

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/filio.h>

#include <port.h>

#include "private.h"

int
evfilt_socket_knote_create(struct filter *filt, struct knote *kn)
{
    int rv, events;

    switch (kn->kev.filter) {
        case EVFILT_READ:
            events = POLLIN;
            break;
        case EVFILT_WRITE:
            events = POLLOUT;
            break;
        default:
            dbg_puts("invalid filter");
            return (-1);
    }

    dbg_printf("port_associate kq fd=%d with actual fd %ld", filter_epoll_fd(filt), kn->kev.ident);

    rv = port_associate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident,
            events, kn);
    if (rv < 0) {
            dbg_perror("port_associate(2)");
            return (-1);
        }

    return (0);
}

int
evfilt_socket_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    dbg_puts("XXX-FIXME");
    (void)filt;
    (void)kn;
    (void)kev;
    return (-1); /* STUB */
}

int
evfilt_socket_knote_delete(struct filter *filt, struct knote *kn)
{
    /* FIXME: should be handled at kevent_copyin()
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    */

    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident) < 0) {
        dbg_perror("port_dissociate(2)");
        return (-1);
    }

    return (0);
}

int
evfilt_socket_knote_enable(struct filter *filt, struct knote *kn)
{

    return evfilt_socket_knote_create(filt, kn);
}

int
evfilt_socket_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_socket_knote_delete(filt, kn);
}

int
evfilt_socket_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr)
{
    port_event_t *pe = (port_event_t *) ptr;
    unsigned int pending_data = 0;

    memcpy(dst, &src->kev, sizeof(*dst));
    if (pe->portev_events == 8) //XXX-FIXME Should be POLLHUP)
        dst->flags |= EV_EOF;
    else if (pe->portev_events & POLLERR)
        dst->fflags = 1; /* FIXME: Return the actual socket error */

    if (pe->portev_events & POLLIN)
    {
        /* On return, data contains the number of bytes of protocol
         data available to read / the length of the socket backlog. */

        if (ioctl(pe->portev_object, FIONREAD, &pending_data) < 0)
        {
            /* race condition with socket close, so ignore this error */
            dbg_puts("ioctl(2) of socket failed");
            dst->data = 0;
        }
        else
            dst->data = pending_data;
    }

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

const struct filter evfilt_read = {
    .kf_id      = EVFILT_READ,
    .kf_copyout = evfilt_socket_copyout,
    .kn_create  = evfilt_socket_knote_create,
    .kn_modify  = evfilt_socket_knote_modify,
    .kn_delete  = evfilt_socket_knote_delete,
    .kn_enable  = evfilt_socket_knote_enable,
    .kn_disable = evfilt_socket_knote_disable,
};

const struct filter evfilt_write = {
    .kf_id      = EVFILT_WRITE,
    .kf_copyout = evfilt_socket_copyout,
    .kn_create  = evfilt_socket_knote_create,
    .kn_modify  = evfilt_socket_knote_modify,
    .kn_delete  = evfilt_socket_knote_delete,
    .kn_enable  = evfilt_socket_knote_enable,
    .kn_disable = evfilt_socket_knote_disable,
};
