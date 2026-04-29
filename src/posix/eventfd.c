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

/*
 * Open a self-pipe with both ends in CLOEXEC + non-blocking mode.
 * pipe2(2) does this atomically on Linux/FreeBSD/illumos; on
 * platforms that lack it (e.g. Darwin) we fall back to pipe(2)
 * plus per-fd fcntl, accepting the small race window between the
 * two calls.  Either way the descriptors land in efd on success.
 */
static int
posix_eventfd_pipe(int sd[2])
{
#if defined(HAVE_PIPE2)
    if (pipe2(sd, O_CLOEXEC | O_NONBLOCK) < 0) {
        dbg_perror("pipe2(2)");
        return (-1);
    }
    return (0);
#else
    int i;

    if (pipe(sd) < 0) {
        dbg_perror("pipe(2)");
        return (-1);
    }
    for (i = 0; i < 2; i++) {
        int fl = fcntl(sd[i], F_GETFL, 0);
        if (fl < 0 ||
            fcntl(sd[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            fcntl(sd[i], F_SETFD, FD_CLOEXEC) < 0) {
            dbg_perror("fcntl(2)");
            (void) close(sd[0]);
            (void) close(sd[1]);
            return (-1);
        }
    }
    return (0);
#endif
}

int
posix_eventfd_init(struct eventfd *efd, struct filter *filt)
{
    int sd[2];

    if (posix_eventfd_pipe(sd) < 0)
        return (-1);

    /*
     * pipe(2)/pipe2(2) returns sd[0] as the read end and sd[1] as
     * the write end.  posix_eventfd_lower() read()s from ef_id,
     * posix_eventfd_raise() write()s to ef_wfd, so map ef_id to
     * the read end and ef_wfd to the write end.
     */
    efd->ef_id  = sd[0];
    efd->ef_wfd = sd[1];
    efd->ef_filt = filt;

    dbg_printf("eventfd=%i - created", efd->ef_id);

    return (0);
}

void
posix_eventfd_close(struct eventfd *efd)
{
    dbg_printf("eventfd=%i - closed", efd->ef_id);

    close(efd->ef_id);
    close(efd->ef_wfd);
    efd->ef_id = -1;
}

int
posix_eventfd_raise(struct eventfd *efd)
{
    dbg_printf("eventfd=%i - raising event level", efd->ef_id);
    if (write(efd->ef_wfd, ".", 1) < 0) {
        /* FIXME: handle EAGAIN and EINTR */
        dbg_printf("write(2) on fd %d: %s", efd->ef_wfd, strerror(errno));
        return (-1);
    }
    return (0);
}

int
posix_eventfd_lower(struct eventfd *efd)
{
    char buf[1024];

    /* Reset the counter */
    dbg_printf("eventfd=%i - lowering event level", efd->ef_id);
    if (read(efd->ef_id, &buf, sizeof(buf)) < 0) {
        /* FIXME: handle EAGAIN and EINTR */
        /* FIXME: loop so as to consume all data.. may need mutex */
        dbg_printf("read(2): %s", strerror(errno));
        return (-1);
    }
    return (0);
}

int
posix_eventfd_descriptor(struct eventfd *efd)
{
    return (efd->ef_id);
}

