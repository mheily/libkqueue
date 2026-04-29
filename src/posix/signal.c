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
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "private.h"

#include "../common/evfilt_signal.h"

/*
 * EVFILT_SIGNAL on platforms without signalfd: sigaction +
 * self-pipe platform layer.  AS-safe handler writes the signum byte
 * to sig_pipe[1]; the dispatch loop in signal.h reads
 * bytes via select(2) on sig_pipe[0].
 *
 * The fan-out machinery (sigtbl, per-filter pending list, copyout,
 * the public evfilt_signal struct) lives in signal.h.
 * This file only provides the platform-layer hooks declared there.
 */

static int
sig_platform_init(void)
{
    return (0);
}

static void
sig_platform_destroy(void)
{
}

/*
 * AS-safe handler.  One byte to the self-pipe == one fire.  EAGAIN
 * on a full pipe drops the fire (POSIX coalesces signals anyway).
 */
static void
_signal_handler(int sig)
{
    unsigned char b = (unsigned char) sig;
    (void) write(sig_pipe[1], &b, 1);
}

static int
sig_platform_add(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = _signal_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static int
sig_platform_remove(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static int
sig_platform_wait_dispatch(void)
{
    unsigned char buf[64];
    fd_set rfds;
    ssize_t n;
    size_t i;

    FD_ZERO(&rfds);
    FD_SET(sig_pipe[0], &rfds);

    if (select(sig_pipe[0] + 1, &rfds, NULL, NULL, NULL) < 0) {
        if (errno == EINTR) return (1);
        dbg_printf("select(2): %s", strerror(errno));
        return (-1);
    }

    n = read(sig_pipe[0], buf, sizeof(buf));
    if (n == 0) {
        dbg_printf("self-pipe closed, exiting");
        return (0);
    }
    if (n < 0) {
        if (errno == EINTR) return (1);
        dbg_printf("read(2): %s", strerror(errno));
        return (1); /* keep going on transient errors */
    }

    pthread_mutex_lock(&sigtbl_mtx);
    for (i = 0; i < (size_t) n; i++)
        sig_dispatch_handle(buf[i]);
    pthread_mutex_unlock(&sigtbl_mtx);
    return (1);
}

/*
 * Filter table entry.  Every member function except the platform-layer
 * hooks above is defined in the shared dispatch header included at
 * the top of this file.
 */
const struct filter evfilt_signal = {
    .kf_id      = EVFILT_SIGNAL,
    .kf_init    = evfilt_signal_init,
    .kf_destroy = evfilt_signal_destroy,
    .kf_copyout = evfilt_signal_copyout,
    .kn_create  = evfilt_signal_knote_create,
    .kn_modify  = evfilt_signal_knote_modify,
    .kn_delete  = evfilt_signal_knote_delete,
    .kn_enable  = evfilt_signal_knote_enable,
    .kn_disable = evfilt_signal_knote_disable,
};
