/*
 * Copyright (c) 2022 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "platform.h"

/*
 * Determine whether the descriptor is a regular file, a socket, or
 * something else (pipe/fifo/chardev) so the copyout path can do
 * the right thing for FIONREAD / EV_EOF / EOF-offset semantics.
 */
static int
posix_read_descriptor_type(struct knote *kn)
{
    struct stat sb;
    int type;
    socklen_t slen = sizeof(type);

    if (fstat((int)kn->kev.ident, &sb) < 0) {
        dbg_perror("fstat(2)");
        return (-1);
    }

    switch (sb.st_mode & S_IFMT) {
    case S_IFREG:
    case S_IFDIR:
        kn->kn_flags |= KNFL_FILE;
        return (0);
    case S_IFCHR:
        kn->kn_flags |= KNFL_CHARDEV;
        return (0);
    case S_IFBLK:
        kn->kn_flags |= KNFL_BLOCKDEV;
        return (0);
    case S_IFIFO:
        kn->kn_flags |= KNFL_PIPE;
        return (0);
    case S_IFSOCK:
        if (getsockopt((int)kn->kev.ident, SOL_SOCKET, SO_TYPE,
                       &type, &slen) < 0) {
            dbg_perror("getsockopt(SO_TYPE)");
            return (-1);
        }
        switch (type) {
        case SOCK_STREAM:    kn->kn_flags |= KNFL_SOCKET_STREAM; break;
        case SOCK_DGRAM:     kn->kn_flags |= KNFL_SOCKET_DGRAM; break;
#ifdef SOCK_SEQPACKET
        case SOCK_SEQPACKET: kn->kn_flags |= KNFL_SOCKET_SEQPACKET; break;
#endif
#ifdef SOCK_RAW
        case SOCK_RAW:       kn->kn_flags |= KNFL_SOCKET_RAW; break;
#endif
        default:
            dbg_printf("unknown socket type %d", type);
            kn->kn_flags |= KNFL_SOCKET_STREAM;
            break;
        }
        /*
         * Detect listening (passive) sockets so copyout can return
         * a non-zero data when an incoming connection is queued.
         */
        if (kn->kn_flags & KNFL_SOCKET_STREAM) {
            int listening = 0;
            slen = sizeof(listening);
            if (getsockopt((int)kn->kev.ident, SOL_SOCKET,
                           SO_ACCEPTCONN, &listening, &slen) == 0) {
                if (listening)
                    kn->kn_flags |= KNFL_SOCKET_PASSIVE;
            } else {
                /*
                 * SO_ACCEPTCONN is missing on macOS (ENOPROTOOPT).
                 * Fall back to recv(MSG_PEEK|MSG_DONTWAIT): on a
                 * listening socket it errors ENOTCONN, on a
                 * connected one it returns data, EOF, or EAGAIN.
                 */
                char probe;
                ssize_t r = recv((int)kn->kev.ident, &probe, 1,
                                 MSG_PEEK | MSG_DONTWAIT);
                if (r < 0 && errno == ENOTCONN)
                    kn->kn_flags |= KNFL_SOCKET_PASSIVE;
            }
        }
        return (0);
    default:
        dbg_printf("unsupported st_mode 0x%x", (unsigned int)sb.st_mode);
        return (-1);
    }
}

/*
 * Wake the kqueue's pselect so the next wait observes a state
 * change.  Writing a byte to kq_wake_wfd makes its read-side
 * (kq_id, registered in kq_fds) readable.  errno is preserved on
 * EAGAIN since the wake-pipe filling up is fine: any byte in it
 * already does the job.
 */
static void
posix_wake_kqueue(struct kqueue *kq)
{
    if (kq->kq_wake_wfd < 0)
        return;
    (void) write(kq->kq_wake_wfd, "K", 1);
}

/*
 * Add fd to the kqueue's read fd_set so the next pselect picks up
 * its readability.  Regular-file knotes are "always readable", so
 * instead of an FD_SET we park them on the filter's kf_ready list
 * and wake the kqueue; the dispatch path walks kf_ready unconditionally.
 */
static int
posix_read_arm(struct filter *filt, struct knote *kn)
{
    int fd = (int)kn->kev.ident;
    struct kqueue *kq = filt->kf_kqueue;

    if (kn->kn_flags & KNFL_FILE) {
        if (!LIST_INSERTED(kn, kn_ready)) {
            LIST_INSERT_HEAD(&filt->kf_ready, kn, kn_ready);
            kq->kq_always_ready++;
        }
        posix_wake_kqueue(kq);
        return (0);
    }
    if (fd < 0 || fd >= FD_SETSIZE) {
        dbg_printf("fd=%d out of FD_SETSIZE=%d", fd, FD_SETSIZE);
        errno = EBADF;
        return (-1);
    }
    FD_SET(fd, &kq->kq_fds);
    if (fd >= kq->kq_nfds)
        kq->kq_nfds = fd + 1;
    return (0);
}

static void
posix_read_disarm(struct filter *filt, struct knote *kn)
{
    int fd = (int)kn->kev.ident;
    struct kqueue *kq = filt->kf_kqueue;

    if (kn->kn_flags & KNFL_FILE) {
        if (LIST_INSERTED(kn, kn_ready)) {
            LIST_REMOVE_ZERO(kn, kn_ready);
            if (kq->kq_always_ready > 0)
                kq->kq_always_ready--;
        }
        return;
    }
    if (fd < 0 || fd >= FD_SETSIZE)
        return;
    FD_CLR(fd, &kq->kq_fds);
}

int
evfilt_read_knote_create(struct filter *filt, struct knote *kn)
{
    if (posix_read_descriptor_type(kn) < 0)
        return (-1);
    return posix_read_arm(filt, kn);
}

int
evfilt_read_knote_delete(struct filter *filt, struct knote *kn)
{
    posix_read_disarm(filt, kn);
    return (0);
}

int
evfilt_read_knote_enable(struct filter *filt, struct knote *kn)
{
    return posix_read_arm(filt, kn);
}

int
evfilt_read_knote_disable(struct filter *filt, struct knote *kn)
{
    posix_read_disarm(filt, kn);
    return (0);
}

int
evfilt_read_knote_modify(UNUSED struct filter *filt, UNUSED struct knote *kn,
        UNUSED const struct kevent *kev)
{
    return (0);
}

/*
 * Synthesise a kevent describing the readable state of a knote's
 * descriptor.  data carries the protocol-data byte count where the
 * platform exposes it (FIONREAD for sockets/pipes; size-curpos for
 * regular files); EV_EOF is set when the peer has shut down.
 */
int
evfilt_read_copyout(struct kevent *dst, UNUSED int nevents,
        struct filter *filt, struct knote *src, UNUSED void *ptr)
{
    int fd = (int)src->kev.ident;

    memcpy(dst, &src->kev, sizeof(*dst));

    if (src->kn_flags & KNFL_FILE) {
        struct stat sb;
        off_t curpos;

        if (fstat(fd, &sb) < 0) {
            dbg_perror("fstat(2)");
            sb.st_size = 0;
        }
        curpos = lseek(fd, 0, SEEK_CUR);
        if (curpos == (off_t) -1)
            curpos = 0;

        /*
         * At EOF the knote is "not ready": don't emit, and stop
         * waking the kqueue on its behalf.  When the consumer
         * lseeks back, the next arm path will rewake.
         */
        if (curpos >= sb.st_size) {
            if (LIST_INSERTED(src, kn_ready)) {
                LIST_REMOVE_ZERO(src, kn_ready);
                if (filt->kf_kqueue->kq_always_ready > 0)
                    filt->kf_kqueue->kq_always_ready--;
            }
            return (0);
        }
        dst->data = (intptr_t)(sb.st_size - curpos);
    } else if (src->kn_flags & KNFL_SOCKET_PASSIVE) {
        /*
         * BSD semantics: data = backlog length on the listen
         * socket.  POSIX has no portable way to read this; report
         * 1 to indicate "at least one connection is waiting".
         */
        dst->data = 1;
    } else {
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) < 0) {
            dbg_perror("ioctl(FIONREAD)");
            dst->data = 0;
        } else {
            dst->data = n;
        }
        /*
         * For stream sockets / pipes, an FD that pselect reported
         * readable but has zero queued bytes typically means the
         * peer closed.  Mirror BSD kqueue's EV_EOF.
         */
        if (dst->data == 0 &&
            (src->kn_flags & (KNFL_SOCKET_STREAM | KNFL_PIPE)))
            dst->flags |= EV_EOF;
    }

    /*
     * EV_CLEAR is "edge-triggered": each level transition
     * (not-readable -> readable) fires once, and we only refire
     * when the consumer has drained back below readable.  select(2)
     * has no edge detector so we approximate by removing the fd
     * from the kq's master set on emit; the level-tracker (TODO)
     * will rearm when FIONREAD returns 0 then >0.
     */
    if (src->kev.flags & EV_CLEAR)
        posix_read_disarm(filt, src);

    if (knote_copyout_flag_actions(filt, src) < 0)
        return (-1);
    return (1);
}

const struct filter evfilt_read = {
    .kf_id      = EVFILT_READ,
    .kf_copyout = evfilt_read_copyout,
    .kn_create  = evfilt_read_knote_create,
    .kn_modify  = evfilt_read_knote_modify,
    .kn_delete  = evfilt_read_knote_delete,
    .kn_enable  = evfilt_read_knote_enable,
    .kn_disable = evfilt_read_knote_disable,
};
