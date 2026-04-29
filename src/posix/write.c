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
 * Mark fd as "watched for write" in the kqueue's write fd_set so
 * the next pselect picks up writability.  Regular files are
 * always-writable; we don't bother with a wake-pipe equivalent
 * because EVFILT_WRITE on a regular file is rarely interesting
 * (file is unconditionally writable up to disk-full).  If a
 * caller does add such a knote we treat it like a socket and let
 * pselect immediately report writable.
 */
static int
posix_write_arm(struct filter *filt, struct knote *kn)
{
    int fd = (int)kn->kev.ident;
    struct kqueue *kq = filt->kf_kqueue;

    if (fd < 0 || fd >= FD_SETSIZE) {
        dbg_printf("fd=%d out of FD_SETSIZE=%d", fd, FD_SETSIZE);
        errno = EBADF;
        return (-1);
    }
    FD_SET(fd, &kq->kq_wfds);
    if (fd >= kq->kq_nfds)
        kq->kq_nfds = fd + 1;
    return (0);
}

static void
posix_write_disarm(struct filter *filt, struct knote *kn)
{
    int fd = (int)kn->kev.ident;
    struct kqueue *kq = filt->kf_kqueue;

    if (fd < 0 || fd >= FD_SETSIZE)
        return;
    FD_CLR(fd, &kq->kq_wfds);
}

/*
 * Lightweight type detection just enough to set kn_flags so the
 * copyout path knows whether to hunt for buffer space (sockets)
 * or report a fixed fixed available size (files/pipes).
 */
static int
posix_write_descriptor_type(struct knote *kn)
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
    case S_IFDIR:  kn->kn_flags |= KNFL_FILE; break;
    case S_IFCHR:  kn->kn_flags |= KNFL_CHARDEV; break;
    case S_IFBLK:  kn->kn_flags |= KNFL_BLOCKDEV; break;
    case S_IFIFO:  kn->kn_flags |= KNFL_PIPE; break;
    case S_IFSOCK:
        if (getsockopt((int)kn->kev.ident, SOL_SOCKET, SO_TYPE,
                       &type, &slen) < 0) {
            dbg_perror("getsockopt(SO_TYPE)");
            return (-1);
        }
        if (type == SOCK_STREAM)
            kn->kn_flags |= KNFL_SOCKET_STREAM;
        else if (type == SOCK_DGRAM)
            kn->kn_flags |= KNFL_SOCKET_DGRAM;
        else
            kn->kn_flags |= KNFL_SOCKET_STREAM;
        break;
    default:
        return (-1);
    }
    return (0);
}

int
evfilt_write_knote_create(struct filter *filt, struct knote *kn)
{
    if (posix_write_descriptor_type(kn) < 0)
        return (-1);
    return posix_write_arm(filt, kn);
}

int
evfilt_write_knote_delete(struct filter *filt, struct knote *kn)
{
    posix_write_disarm(filt, kn);
    return (0);
}

int
evfilt_write_knote_enable(struct filter *filt, struct knote *kn)
{
    return posix_write_arm(filt, kn);
}

int
evfilt_write_knote_disable(struct filter *filt, struct knote *kn)
{
    posix_write_disarm(filt, kn);
    return (0);
}

int
evfilt_write_knote_modify(UNUSED struct filter *filt, UNUSED struct knote *kn,
        UNUSED const struct kevent *kev)
{
    return (0);
}

int
evfilt_write_copyout(struct kevent *dst, UNUSED int nevents,
        struct filter *filt, struct knote *src, UNUSED void *ptr)
{
    int fd = (int)src->kev.ident;

    memcpy(dst, &src->kev, sizeof(*dst));

    /*
     * BSD kqueue reports `data` as the amount of free buffer space
     * available for non-blocking write.  We don't have a portable
     * way to query send-buffer free space (Linux has SIOCOUTQ, BSD
     * has none cleanly); report 0 to mean "writable" and let the
     * application size its own writes.  This loses the byte count
     * but matches "ready to write" semantics.
     */
    dst->data = 0;

    if (src->kev.flags & EV_CLEAR)
        posix_write_disarm(filt, src);

    if (knote_copyout_flag_actions(filt, src) < 0)
        return (-1);
    return (1);
    (void) fd;
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
