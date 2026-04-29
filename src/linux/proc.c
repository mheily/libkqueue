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

/* We depend on the SYS_pidfd_open call to determine when a process has exited
 *
 * The SYS_pidfd_open call is only available in Kernels >= 5.3.  If this call
 * isn't available there's currently no good fallback.
 *
 * If the build system detects SYS_pidfd_open is not available it will fall back
 * to using posix/proc.c and won't build this source file.
 */
#include <sys/syscall.h>
#include <sys/wait.h>

int
evfilt_proc_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, UNUSED_NDEBUG void *ptr)
{
    siginfo_t info;
    unsigned int status = 0;
#ifndef NDEBUG
    struct epoll_event * const ev = (struct epoll_event *) ptr;
#endif

    dbg_printf("epoll_ev=%s", epoll_event_dump(ev));
    memcpy(dst, &src->kev, sizeof(*dst)); /* Populate flags from the src kevent */

    /* Get the exit status _without_ reaping the process, waitpid() should still work in the caller */
    if (waitid(P_PID, (id_t)src->kev.ident, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
        dbg_printf("waitid(2): %s", strerror(errno));
        return (-1);
    }

    /*
     *  Try and reconstruct the status code that would have been
     *  returned by waitpid.  The OpenBSD man pages
     *  and observations of the macOS kqueue confirm this is what
     *  we should be returning in the data field of the kevent.
     */
    switch (info.si_code) {
    case CLD_EXITED:    /* WIFEXITED - High byte contains status, low byte zeroed */
        status = info.si_status << 8;
        dbg_printf("pid=%u exited, status %u", (unsigned int)src->kev.ident, status);
        break;

    case CLD_DUMPED:    /* WIFSIGNALED/WCOREDUMP - Core flag set - Low 7 bits contains fatal signal */
        status |= 0x80; /* core flag */
        status |= info.si_status & 0x7f;
        dbg_printf("pid=%u dumped, status %u", (unsigned int)src->kev.ident, status);
        break;

    case CLD_KILLED:    /* WIFSIGNALED - Low 7 bits contains fatal signal */
        status = info.si_status & 0x7f;
        dbg_printf("pid=%u signalled, status %u", (unsigned int)src->kev.ident, status);
        break;

    default: /* The rest aren't valid exit states */
        return (0);
    }

    dst->data = status;
    dst->flags |= EV_EOF; /* Set in macOS and FreeBSD kqueue implementations */

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_proc_knote_enable(struct filter *filt, struct knote *kn)
{
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_ADD, kn->kn_procfd, EPOLL_EV_KN(EPOLLIN, kn)) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        return -1;
    }
    return (0);
}

int
evfilt_proc_knote_disable(struct filter *filt, struct knote *kn)
{
    /*
     * kn_create returns 0 without arming a pidfd when no NOTE_*
     * fflags are set (kn_procfd stays -1).  A subsequent
     * EV_DISABLE / EV_DELETE on such a knote has nothing kernel-
     * side to remove; short-circuit instead of letting epoll_ctl
     * fail with EBADF and having to decide whether that's benign.
     */
    if (kn->kn_procfd < 0)
        return (0);

    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_DEL, kn->kn_procfd, NULL) < 0) {
        dbg_printf("epoll_ctl(EPOLL_CTL_DEL pidfd=%i): %s",
                   kn->kn_procfd, strerror(errno));
        return (-1);
    }
    return (0);
}

int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    int pfd;

    /* This mirrors the behaviour of kqueue if fflags doesn't specify any events */
    if (!(kn->kev.fflags & NOTE_EXIT)) {
        dbg_printf("not monitoring pid=%u as no NOTE_* fflags set", (unsigned int)kn->kev.ident);
        kn->kn_procfd = -1;
        return 0;
    }

    /* Returns an FD, which, when readable, indicates the process has exited */
    pfd = syscall(SYS_pidfd_open, (pid_t)kn->kev.ident, 0);
    if (pfd < 0) {
        dbg_perror("pidfd_open(2)");
    error_0:
        return (-1);
    }
    if (fcntl(pfd, F_SETFD, FD_CLOEXEC) < 0) {
        dbg_perror("fcntl(F_SETFD)");
    error_1:
        (void) close(pfd);
        kn->kn_procfd = -1;
        goto error_0;
    }
    dbg_printf("created pidfd=%i monitoring pid=%u", pfd, (unsigned int)kn->kev.ident);

    kn->kn_procfd = pfd;

    /*
     * These get added by default on macOS (and likely FreeBSD)
     * which make sense considering a process exiting is an
     * edge triggered event, not a level triggered one.
     */
    kn->kev.flags |= EV_ONESHOT;
    kn->kev.flags |= EV_CLEAR;

    if (KN_UDATA_ALLOC(kn) == NULL) {
        dbg_perror("epoll_udata_alloc");
        goto error_1;
    }

    if (evfilt_proc_knote_enable(filt, kn) < 0) {
        /*
         * enable's epoll_ctl(EPOLL_CTL_ADD) failed.  Kernel never
         * saw the udata, free direct (no defer needed).  Common
         * code calls knote_release on kn_create failure, not
         * kn_delete, so this is the only chance to release the
         * pidfd and udata.
         */
        KN_UDATA_FREE(kn);
        goto error_1;
    }

    return (0);
}

int
evfilt_proc_knote_modify(struct filter *filt, struct knote *kn, const struct kevent *kev)
{
    /*
     * Merge the new caller-visible flags onto kn->kev, preserving
     * bits that kn_create owns the policy for:
     *
     *   - EV_ONESHOT | EV_CLEAR: kn_create forces these for NOTE_EXIT
     *     because pidfd readiness is intrinsically edge-triggered and
     *     fires once.  If support for NOTE_FORK / NOTE_EXEC (which
     *     fire repeatedly) is added, kn_create won't set these for
     *     those fflags, and this preserve-from-kn->kev.flags pattern
     *     naturally reflects whatever kn_create chose.
     *   - EV_RECEIPT: sticky on BSD, libkqueue matches.
     *
     * Mask the user's incoming flags to drop the same bits so the
     * preserved bits aren't double-OR'd from a stale caller value.
     */
    static const unsigned int preserve = EV_ONESHOT | EV_CLEAR | EV_RECEIPT;
    bool want_arm, is_armed;

    kn->kev.flags  = (kev->flags & ~preserve)
                   | (kn->kev.flags & preserve);
    kn->kev.fflags = kev->fflags;

    want_arm = (kev->fflags & NOTE_EXIT) != 0;
    is_armed = (kn->kn_procfd >= 0);

    /*
     * Late-arm: kn_create returned without arming (no NOTE_* fflags
     * set then) and the caller has now added NOTE_EXIT.  Run
     * kn_create now to open the pidfd and EPOLL_CTL_ADD it.
     */
    if (want_arm && !is_armed) return evfilt_proc_knote_create(filt, kn);

    /*
     * Disarm: the caller dropped NOTE_EXIT (or replaced it with an
     * fflag this backend doesn't yet implement).  Mirror the
     * kn_create-with-no-NOTE_* end state: detach from epoll, close
     * the pidfd, defer-free the udata so any in-flight epoll_wait
     * holding it in its TLS buffer remains safe to dereference.
     */
    if (!want_arm && is_armed) {
        if (KNOTE_ENABLED(kn))
            (void) evfilt_proc_knote_disable(filt, kn);
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);
        (void) close(kn->kn_procfd);
        kn->kn_procfd = -1;
    }

    return (0);
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
{
    int rv = 0;

    /* If it's enabled, we need to remove the pidfd from epoll */
    if (KNOTE_ENABLED(kn) && (evfilt_proc_knote_disable(filt, kn) < 0)) rv = -1;

    KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

    dbg_printf("closed pidfd=%i", kn->kn_procfd);
    if (close(kn->kn_procfd) < 0) rv = -1;
    kn->kn_procfd = -1;

    return (rv);
}

const struct filter evfilt_proc = {
    .kf_id      = EVFILT_PROC,
    .kf_copyout = evfilt_proc_copyout,
    .kn_create  = evfilt_proc_knote_create,
    .kn_modify  = evfilt_proc_knote_modify,
    .kn_delete  = evfilt_proc_knote_delete,
    .kn_enable  = evfilt_proc_knote_enable,
    .kn_disable = evfilt_proc_knote_disable,
};
