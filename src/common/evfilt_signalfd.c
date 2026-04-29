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

/*
 * Shared EVFILT_SIGNAL implementation for platforms with signalfd
 * (Linux + illumos).
 *
 * The dispatcher's signalfd holds the union of all signums any
 * knote in the process is interested in.  sig_active_set is the
 * authoritative copy (protected by sigtbl_mtx); we update the
 * kernel-side mask in place via signalfd(existing_fd, &new, 0).
 *
 * No reserved signal needed: select() picks up signalfd readiness
 * directly when a signal lands in the kernel pending queue, and
 * shutdown wakes us via the self-pipe (read() returns 0 once the
 * write end is closed).
 *
 * Cross-thread mask requirement: signalfd only sees signals that
 * are blocked process-wide.  sig_platform_add blocks the signum
 * on the calling thread; threads spawned afterwards inherit the
 * mask; pre-existing threads remain the application's
 * responsibility (see BUGS.md).  Both Linux and illumos signalfd
 * impose this constraint.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "private.h"
#include "evfilt_signal.h"

static int       sig_signalfd = -1;
static sigset_t  sig_active_set;

static int
sig_platform_init(void)
{
    sigset_t empty;

    sigemptyset(&empty);
    sigemptyset(&sig_active_set);
    sig_signalfd = signalfd(-1, &empty, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_signalfd < 0) {
        dbg_perror("signalfd(2)");
        return (-1);
    }
    return (0);
}

static void
sig_platform_destroy(void)
{
    if (sig_signalfd >= 0) {
        (void) close(sig_signalfd);
        sig_signalfd = -1;
    }
}

/** Close the inherited signalfd in a forked child.
 *
 * fork() dups the fd into the child's table; close() in the child
 * just drops the child's reference - the parent's signalfd object
 * is unaffected (refcount > 1 across fork).  The dispatcher thread
 * that was using this fd is gone in the child anyway, so closing
 * is both correct and frees the descriptor for reuse.
 *
 * sig_active_set is plain memory the child has its own copy of;
 * no action needed.  The child's next sig_platform_init will open
 * a fresh signalfd from scratch.
 */
static void
sig_platform_reset_after_fork(void)
{
    if (sig_signalfd >= 0) {
        (void) close(sig_signalfd);
        sig_signalfd = -1;
    }
}

/*
 * sigtbl_mtx held by caller.  Both update sig_active_set and push
 * the new mask into the kernel via signalfd().  We also block the
 * signum on the calling thread here in sig_platform_add - the
 * "first knote registered for this signum" path - so the very
 * thread that registered won't take delivery itself.
 */
static int
sig_platform_add(int sig)
{
    sigset_t s;
    siginfo_t info;
    struct timespec zero = { 0, 0 };

    sigemptyset(&s);
    sigaddset(&s, sig);
    if (pthread_sigmask(SIG_BLOCK, &s, NULL) != 0) {
        dbg_printf("pthread_sigmask(BLOCK %d): %s", sig, strerror(errno));
        return (-1);
    }

    /*
     * Drain any siginfos for this signum that accumulated in the
     * per-process pending queue while we weren't watching.  Without
     * this, an EV_DELETE -> kill() -> re-EV_ADD sequence would let
     * the queued signal land in the new knote's count once the
     * signum is added back to the signalfd mask below, falsely
     * reporting an extra fire to the new caller.
     */
    while (sigtimedwait(&s, &info, &zero) > 0) {
        dbg_printf("drained pre-existing siginfo for signal %d", sig);
    }

    sigaddset(&sig_active_set, sig);
    if (signalfd(sig_signalfd, &sig_active_set, 0) < 0) {
        dbg_perror("signalfd(update)");
        sigdelset(&sig_active_set, sig);
        return (-1);
    }
    return (0);
}

static int
sig_platform_remove(int sig)
{
    sigdelset(&sig_active_set, sig);
    if (signalfd(sig_signalfd, &sig_active_set, 0) < 0) {
        dbg_perror("signalfd(update)");
        return (-1);
    }
    /*
     * Leave the signal blocked.  pthread_sigmask only affects the
     * calling thread, which may not be the one that registered;
     * unblocking here would create an inconsistent mask across the
     * application's threads.  A blocked signal with no signalfd
     * watching just queues until the application explicitly
     * accepts or unblocks.
     */
    return (0);
}

static int
sig_platform_wait_dispatch(void)
{
    struct signalfd_siginfo si[64];
    fd_set rfds;
    ssize_t n;
    size_t count, i;
    int maxfd;

    FD_ZERO(&rfds);
    FD_SET(sig_signalfd, &rfds);
    FD_SET(sig_pipe[0], &rfds);
    maxfd = (sig_signalfd > sig_pipe[0]) ? sig_signalfd : sig_pipe[0];

    if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
        if (errno == EINTR) return (1);
        dbg_printf("select(2): %s", strerror(errno));
        return (-1);
    }

    if (FD_ISSET(sig_pipe[0], &rfds)) {
        char buf[64];
        ssize_t r = read(sig_pipe[0], buf, sizeof(buf));
        if (r <= 0) {
            dbg_printf("shutdown pipe closed, exiting");
            return (0);
        }
    }

    if (FD_ISSET(sig_signalfd, &rfds)) {
        /*
         * Lock-around-read serialises against sig_platform_add's
         * signalfd(sig_signalfd, &mask, 0) mask-update on the same
         * fd.  Functionally the kernel already does this (both
         * paths take current->sighand->siglock in fs/signalfd.c)
         * but TSAN tracks per-fd state in user space and flags the
         * unsynchronised read without an explicit happens-before
         * edge.  no_sanitize("thread") doesn't help - the report
         * comes from libc's read() interceptor, outside our
         * function.  signalfd is opened SFD_NONBLOCK, so the read
         * can't block the lock holder.
         *
         * As a side benefit it closes the would-be window where
         * catch_signal could add a new knote between our read()
         * and dispatch and cause pre-existing siginfos to be
         * mis-attributed.
         */
        pthread_mutex_lock(&sigtbl_mtx);
        n = read(sig_signalfd, si, sizeof(si));
        if (n < 0) {
            int saved = errno;
            pthread_mutex_unlock(&sigtbl_mtx);
            if (saved == EINTR || saved == EAGAIN) return (1);
            dbg_printf("read(signalfd): %s", strerror(saved));
            return (-1);
        }
        if ((size_t) n < sizeof(si[0])) {
            pthread_mutex_unlock(&sigtbl_mtx);
            dbg_printf("read(signalfd): short read (%zd bytes)", n);
            return (1);
        }
        count = (size_t) n / sizeof(si[0]);
        for (i = 0; i < count; i++)
            sig_dispatch_handle((int) si[i].ssi_signo);
        pthread_mutex_unlock(&sigtbl_mtx);
    }

    return (1);
}

const struct filter evfilt_signal = {
    .kf_id            = EVFILT_SIGNAL,
    .libkqueue_fork   = evfilt_signal_reset_after_fork,
    .kf_init          = evfilt_signal_init,
    .kf_destroy       = evfilt_signal_destroy,
    .kf_copyout       = evfilt_signal_copyout,
    .kn_create        = evfilt_signal_knote_create,
    .kn_modify        = evfilt_signal_knote_modify,
    .kn_delete        = evfilt_signal_knote_delete,
    .kn_enable        = evfilt_signal_knote_enable,
    .kn_disable       = evfilt_signal_knote_disable,
};
