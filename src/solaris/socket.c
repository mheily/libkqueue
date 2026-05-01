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

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/filio.h>

#include <port.h>

#include "private.h"

/*
 * Cache the fd type in kn_flags so the read filter's copyout can
 * branch without per-event fstat/getsockopt (mirrors Linux's
 * linux_get_descriptor_type).  KNFL_SOCKET_PASSIVE flags listening
 * sockets so the read filter knows to deliver their wakes even
 * when FIONREAD shows 0 - on a listener "readable" means a pending
 * accept, not queued bytes.
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

    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
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
     * Allocate on first registration; re-arms reuse the existing ud
     * (see port_udata in platform.h).  If port_associate fails we
     * have to free the ud inline: kn_create failure goes through
     * knote_release, not kn_delete, so defer_free never runs.
     * fresh_udata distinguishes "we allocated this call" from "we
     * re-armed an existing ud" so we only free what we own.
     */
    if (kn->kn_udata == NULL) {
        if (KN_UDATA_ALLOC(kn) == NULL) {
            dbg_puts("port_udata_alloc");
            return (-1);
        }
        fresh_udata = true;
    }

    /*
     * NOTE_LOWAT is not implemented on Solaris.  illumos rejects
     * setsockopt(SO_RCVLOWAT|SO_SNDLOWAT) with ENOPROTOOPT, and
     * PORT_SOURCE_FD is level-triggered, so userspace gating would
     * busy-loop: kernel wakes on every byte, we suppress, re-arm,
     * kernel wakes again immediately because data is still queued.
     * The flag is silently ignored.
     */

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
     * port_associate doubles as update: a second call on an already-
     * associated source replaces events and user, so we just call
     * knote_create again to re-arm.
     */
    if (evfilt_socket_knote_create(filt, kn) < 0)
        return (-1);

    /*
     * Common code only copies udata (and conditionally EV_DISPATCH)
     * after kn_modify.  Sync fflags/data (NOTE_LOWAT and friends are
     * caller-mutable), but leave kev.flags alone: BSD treats EV_CLEAR
     * as register-only on sockets and EV_RECEIPT is sticky.  Touching
     * flags here would clobber both invariants.
     */
    kn->kev.fflags = kev->fflags;
    kn->kev.data   = kev->data;
    return (0);
}

int
evfilt_socket_knote_delete(struct filter *filt, struct knote *kn)
{
    /*
     * port_dissociate may fail if a prior delivery auto-removed the
     * association (PORT_SOURCE_FD is one-shot per delivery).  EV_DELETE
     * is teardown - the kernel-side state is gone either way, so log
     * and return success.
     */
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD, kn->kev.ident) < 0)
        dbg_perror("port_dissociate(2) on EV_DELETE (likely already auto-removed)");

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
     * port_associate uses poll(2) event types.  POLLHUP signals an
     * explicit hangup; a peer's FIN-close on a connected stream arrives
     * as POLLIN with no data instead, caught by the MSG_PEEK below.
     */
    if (pe->portev_events & (POLLHUP | POLLERR)) {
        /*
         * RST can arrive as POLLHUP on illumos without POLLERR set, so
         * check SO_ERROR on either path.  Normal FIN-close leaves
         * SO_ERROR == 0 and fflags stays 0.
         */
        int       serr = 0;
        socklen_t slen = sizeof(serr);
        dst->flags |= EV_EOF;
        if (getsockopt(pe->portev_object, SOL_SOCKET, SO_ERROR, &serr, &slen) < 0) {
            dbg_perror("getsockopt(SO_ERROR)");
            if (pe->portev_events & POLLERR)
                dst->fflags = errno;
        } else if (serr) {
            dst->fflags = (unsigned int) serr;
        } else if (pe->portev_events & POLLERR) {
            /* POLLERR but SO_ERROR clear - shouldn't happen, stamp EIO. */
            dst->fflags = EIO;
        }
    }

    if (pe->portev_events & POLLOUT) {
        /*
         * BSD reports "space remaining in the write buffer".  illumos
         * has no SIOCOUTQ-style queued-bytes ioctl, so for sockets we
         * report SO_SNDBUF (the high-water mark): an upper bound on a
         * single write the kernel will accept without blocking.
         * Approximate, but matches the value most consumers expect.
         * Non-sockets (e.g. pipes) don't accept SO_SNDBUF; leave data=0
         * rather than stamp errno into it.
         */
        if (src->kn_flags & KNFL_SOCKET) {
            int       sndbuf = 0;
            socklen_t slen = sizeof(sndbuf);
            if (getsockopt(pe->portev_object, SOL_SOCKET, SO_SNDBUF, &sndbuf, &slen) < 0) {
                dbg_perror("getsockopt(SO_SNDBUF)");
                dst->data = 0;
            } else {
                dst->data = sndbuf;
            }
        } else {
            dst->data = 0;
        }
    }

    if (pe->portev_events & POLLIN) {
        if (src->kn_flags & KNFL_SOCKET_PASSIVE) {
            /*
             * Listening socket: readiness signals a pending accept, not data
             * bytes.  solaris_kevent_copyout drops POLLIN events with data==0
             * to suppress zero-byte read races; we report data=1 so a real
             * accept readiness isn't suppressed.
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
         * POLLIN + data=0 on a connected stream is ambiguous: peer closed
         * (EOF) or another thread drained the queue first (race).  recv
         * with MSG_PEEK + MSG_DONTWAIT confirms - it returns 0 only on real
         * EOF.  Set EV_EOF only when confirmed; otherwise the copyout's
         * data==0 skip drops the event silently and the consumer never
         * sees the close.
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
     * NOTE: re-association and EV_DISPATCH/EV_ONESHOT handling happen
     * in solaris_kevent_copyout, not here.  Calling
     * knote_copyout_flag_actions inside this function would double-fire
     * the actions and UAF the knote.
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
