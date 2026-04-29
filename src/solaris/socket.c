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
#include <sys/stat.h>
#include <unistd.h>
#include <sys/filio.h>

#include <port.h>

#include "private.h"

/*
 * Classify the fd associated with a knote and stamp kn->kn_flags
 * with the corresponding KNFL_* bits.  Mirrors the Linux backend's
 * linux_get_descriptor_type so the read filter's copyout can branch
 * on cached state instead of doing fstat/getsockopt on every event.
 *
 * - KNFL_FILE / KNFL_PIPE / KNFL_BLOCKDEV / KNFL_CHARDEV: from stat
 * - KNFL_SOCKET_STREAM / DGRAM / RDM / SEQPACKET / RAW: from SO_TYPE
 * - KNFL_SOCKET_PASSIVE: also set if SO_ACCEPTCONN reports a
 *   listening socket (FIONREAD on a passive socket returns 0; the
 *   readiness signals a pending accept(), not data).
 */
static int
solaris_get_descriptor_type(struct knote *kn)
{
    socklen_t slen;
    struct stat sb;
    int lsock, stype;
    const int fd = (int) kn->kev.ident;

    if (fstat(fd, &sb) < 0) {
        dbg_perror("fstat(2)");
        return (-1);
    }

    switch (sb.st_mode & S_IFMT) {
    default:
        errno = EBADF;
        dbg_perror("fd=%i unknown fd type, st_mode=0x%x", fd, sb.st_mode & S_IFMT);
        return (-1);

    case S_IFREG:
        kn->kn_flags |= KNFL_FILE;
        return (0);

    case S_IFIFO:
        kn->kn_flags |= KNFL_PIPE;
        return (0);

    case S_IFBLK:
        kn->kn_flags |= KNFL_BLOCKDEV;
        return (0);

    case S_IFCHR:
        kn->kn_flags |= KNFL_CHARDEV;
        return (0);

    case S_IFSOCK:
        break; /* fall through to SO_TYPE check */
    }

    slen = sizeof(stype);
    stype = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &stype, &slen) < 0) {
        dbg_perror("getsockopt(SO_TYPE)");
        return (-1);
    }
    switch (stype) {
    case SOCK_STREAM:
        kn->kn_flags |= KNFL_SOCKET_STREAM;
        break;

    case SOCK_DGRAM:
        kn->kn_flags |= KNFL_SOCKET_DGRAM;
        break;

    case SOCK_RDM:
        kn->kn_flags |= KNFL_SOCKET_RDM;
        break;

    case SOCK_SEQPACKET:
        kn->kn_flags |= KNFL_SOCKET_SEQPACKET;
        break;

    case SOCK_RAW:
        kn->kn_flags |= KNFL_SOCKET_RAW;
        break;

    default:
        errno = EBADF;
        dbg_perror("unknown socket type %d", stype);
        return (-1);
    }

    slen = sizeof(lsock);
    lsock = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &lsock, &slen) == 0 && lsock)
        kn->kn_flags |= KNFL_SOCKET_PASSIVE;

    return (0);
}

int
evfilt_socket_knote_create(struct filter *filt, struct knote *kn)
{
    int rv, events;
    bool fresh_udata = false;

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

    /*
     * Classify on first creation; subsequent re-arms (after delivery
     * auto-dissociates) bypass classification because kn_flags is
     * already stamped.
     */
    if (kn->kn_flags == 0 && solaris_get_descriptor_type(kn) < 0)
        return (-1);

    /*
     * Allocate the udata on first registration; subsequent re-arms
     * reuse it.  port_associate's user pointer must outlive the
     * knote across the kevent_wait window so EV_DELETE can defer-
     * free it instead of freeing inline.
     *
     * Track whether we just allocated so we can release on
     * port_associate failure.  Common code calls knote_release
     * (not kn_delete) on kn_create failure, so we need to free
     * inline; the udata never reached the kernel so deferring is
     * unnecessary.
     */
    if (kn->kn_udata == NULL) {
        if (KN_UDATA_ALLOC(kn) == NULL) {
            dbg_puts("port_udata_alloc");
            return (-1);
        }
        fresh_udata = true;
    }

    dbg_printf("port_associate kq fd=%d with actual fd %ld kn_flags=0x%x",
               filter_epoll_fd(filt), kn->kev.ident, kn->kn_flags);

    rv = port_associate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident,
            events, kn->kn_udata);
    if (rv < 0) {
        dbg_perror("port_associate(2)");
        if (fresh_udata)
            KN_UDATA_FREE(kn);
        return (-1);
    }

    return (0);
}

int
evfilt_socket_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * BSD's kevent semantics: EV_ADD on an existing knote updates
     * the registration in place.  On Solaris/illumos event ports
     * a second port_associate on an already-associated source
     * serves as an update (port_associate(3C): "If the specified
     * object is already associated with the specified port, the
     * port_associate() function serves to update the events and
     * user arguments of the association").
     *
     * The common kevent_copyin path copies kev into kn->kev after
     * we return success, so just re-associate using the filter's
     * implied events.  No need to mutate kn here.
     */
    (void) kev;
    return evfilt_socket_knote_create(filt, kn);
}

int
evfilt_socket_knote_delete(struct filter *filt, struct knote *kn)
{
    /* FIXME: should be handled at kevent_copyin()
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    */

    /*
     * Solaris/illumos event ports are one-shot per association
     * (port_associate(3C): "At most one event notification will be
     * generated per associated file descriptor").  If a prior event
     * already auto-dissociated the fd, port_dissociate() here will
     * fail (the man page lists EBADF/EBADFD/EINVAL but not
     * specifically the auto-removed case for PORT_SOURCE_FD; in
     * practice it returns one of those).
     *
     * Either way, EV_DELETE is a teardown call and the kernel-side
     * state is already gone after a failed dissociate, so log the
     * error and return success.  The caller doesn't have anything
     * useful it can do with the failure.
     */
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident) < 0)
        dbg_perror("port_dissociate(2) on EV_DELETE (likely already auto-removed)");

    /*
     * After port_dissociate the kernel won't queue new events for
     * the udata, but a concurrent kevent() may already hold a
     * stale portev_user in its TLS buffer.  Defer-free so the
     * udata stays alive until every in-flight caller has exited.
     */
    if (kn->kn_udata != NULL)
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

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
    /*
     * Disable just dissociates; keep the udata so a later enable
     * can re-arm without reallocating.  Defer-free happens at
     * EV_DELETE.
     */
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident) < 0)
        dbg_perror("port_dissociate(2) on EV_DISABLE (likely already auto-removed)");
    return (0);
}

int
evfilt_socket_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr)
{
    port_event_t *pe = (port_event_t *) ptr;
    unsigned int pending_data = 0;

    (void) filt;

    memcpy(dst, &src->kev, sizeof(*dst));

    /*
     * port_associate(3C) on PORT_SOURCE_FD says event types follow
     * poll(2) semantics.  POLLERR -> error; POLLHUP -> peer hangup;
     * POLLIN combined with no readable data on a connected stream
     * socket also indicates EOF on illumos (POLLHUP isn't always
     * delivered).
     */
    if (pe->portev_events & POLLHUP)
        dst->flags |= EV_EOF;
    if (pe->portev_events & POLLERR)
        dst->fflags = 1; /* FIXME: surface the real SO_ERROR value */

    if (pe->portev_events & POLLIN) {
        if (src->kn_flags & KNFL_SOCKET_PASSIVE) {
            /*
             * Listening socket: readiness signals a pending accept,
             * not protocol data.  FIONREAD would return 0 here and
             * solaris_kevent_copyout's `skip if data == 0` defence
             * would drop the event; report data=1 so it survives.
             * Matches Linux's KNFL_SOCKET_PASSIVE path.
             */
            dst->data = 1;
        } else if (ioctl(pe->portev_object, FIONREAD, &pending_data) < 0) {
            /* race with close(); pretend zero pending */
            dbg_puts("ioctl(FIONREAD) of socket failed");
            dst->data = 0;
        } else {
            dst->data = pending_data;
        }

        /*
         * Connected stream sockets: zero-byte readiness from POLLIN
         * means the peer shut down or closed.  Translate to EV_EOF
         * so the consumer sees terminal state instead of having the
         * event silently dropped by the data==0 filter.  Use
         * MSG_PEEK to confirm rather than guessing - a transient
         * race with a reader is also possible.
         */
        if (dst->data == 0 && (src->kn_flags & KNFL_SOCKET_STREAM) &&
            !(src->kn_flags & KNFL_SOCKET_PASSIVE)) {
            char peek_buf;
            ssize_t n = recv(pe->portev_object, &peek_buf, 1,
                             MSG_PEEK | MSG_DONTWAIT);
            if (n == 0)
                dst->flags |= EV_EOF;
        }
    }

    /*
     * NOTE: re-association after delivery and EV_DISPATCH/EV_ONESHOT
     * handling are both done in solaris_kevent_copyout, NOT here,
     * unlike the Linux backend which calls knote_copyout_flag_actions
     * inside the per-filter copyout.  Doing it in both places leads
     * to a double knote_delete and use-after-free.
     */
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
