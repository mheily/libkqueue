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

/*
 * We depend on the SYS_pidfd_open call to determine when a process has exited
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

    /*
     * KEVENT_WAIT_DROP_LOCK lets another thread call EV_DISABLE
     * between epoll_wait fetching the ready event into our TLS
     * evbuf and us getting here for copyout.  BSD semantics: a
     * disabled knote MUST NOT deliver pending events.  Skip and
     * return zero events for this slot.
     */
    if (KNOTE_DISABLED(src)) {
        dbg_printf("kn=%p disabled, dropping pre-fetched event", src);
        return (0);
    }

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

/*
 * Open the pidfd, allocate udata, register in epoll.
 *
 * Mirrors posix/proc.c's proc_pid_arm in shape: a single "start
 * watching this pid" entry point used by kn_create and kn_modify's
 * late-arm branch.  Common code calls knote_release (not kn_delete)
 * on kn_create failure, so the only cleanup chance is here.
 *
 * @return 0 on success, -1 on failure (pidfd + udata released).
 */
static int
linux_proc_arm(struct filter *filt, struct knote *kn)
{
    int pfd;

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

    if (KN_UDATA_ALLOC(kn) == NULL) {
        dbg_perror("epoll_udata_alloc");
        goto error_1;
    }

    if (evfilt_proc_knote_enable(filt, kn) < 0) {
        /*
         * enable's epoll_ctl(EPOLL_CTL_ADD) failed.  Kernel never
         * saw the udata, free direct (no defer needed).
         */
        KN_UDATA_FREE(kn);
        goto error_1;
    }

    return (0);
}

/*
 * Detach pidfd from epoll, defer-free udata, close pidfd.
 *
 * Mirrors posix/proc.c's proc_pid_disarm.  Idempotent on a knote
 * that was never armed (kn_procfd < 0).  The udata is defer-freed
 * (not freed inline) because a concurrent epoll_wait may still
 * have it in its TLS buffer.
 */
static void
linux_proc_disarm(struct filter *filt, struct knote *kn)
{
    if (kn->kn_procfd < 0) return;

    if (KNOTE_ENABLED(kn))
        (void) evfilt_proc_knote_disable(filt, kn);
    KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);
    (void) close(kn->kn_procfd);
    kn->kn_procfd = -1;
}

int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    /* No NOTE_* in fflags = registered but won't deliver. */
    if (!(kn->kev.fflags & NOTE_EXIT)) {
        dbg_printf("not monitoring pid=%u as no NOTE_* fflags set", (unsigned int)kn->kev.ident);
        kn->kn_procfd = -1;
        return (0);
    }

    if (linux_proc_arm(filt, kn) < 0) return (-1);

    /*
     * EV_ONESHOT|EV_CLEAR are forced because pidfd readiness for
     * NOTE_EXIT is intrinsically edge-triggered and fires once.
     * See evfilt_proc_knote_modify for the matching preserve-on-
     * modify policy.
     */
    kn->kev.flags |= EV_ONESHOT;
    kn->kev.flags |= EV_CLEAR;
    return (0);
}

int
evfilt_proc_knote_modify(struct filter *filt, struct knote *kn, const struct kevent *kev)
{
    /*
     * Merge the new caller-visible flags onto kn->kev, preserving
     * bits that kn_create owns the policy for (EV_ONESHOT|EV_CLEAR
     * for NOTE_EXIT, EV_RECEIPT sticky on BSD).  See posix/proc.c
     * kn_modify - the two backends use the identical merge policy.
     */
    static const unsigned int preserve = EV_ONESHOT | EV_CLEAR | EV_RECEIPT;
    bool want_armed, was_armed;

    /* Detect arm-state structurally rather than from a flag bit. */
    was_armed  = (kn->kn_procfd >= 0);
    want_armed = (kev->fflags & NOTE_EXIT) != 0;

    kn->kev.flags  = (kev->flags & ~preserve)
                   | (kn->kev.flags & preserve);
    kn->kev.fflags = kev->fflags;

    if (want_armed && !was_armed) {
        if (linux_proc_arm(filt, kn) < 0) return (-1);
        kn->kev.flags |= EV_ONESHOT | EV_CLEAR;
    } else if (!want_armed && was_armed) {
        linux_proc_disarm(filt, kn);
    }

    return (0);
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
{
    linux_proc_disarm(filt, kn);
    return (0);
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
