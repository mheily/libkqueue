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

#include "common.h"

static int sigusr1_caught = 0;
static pid_t pid;

static void
sig_handler(int signum)
{
    sigusr1_caught = 1;
}

static void
test_kevent_proc_add(struct test_context *ctx)
{
    struct kevent kev;

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_proc_delete(struct test_context *ctx)
{
    struct kevent kev;

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_DELETE, NOTE_EXIT, 0, NULL);
    if (kill(pid, SIGKILL) < 0)
        die("kill");
    sleep(1);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_proc_get(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int fflags;

    /*
     *  macOS requires NOTE_EXITSTATUS to get the
     *  exit code of the process, FreeBSD always
     *  provides it.
     */
#ifdef NOTE_EXITSTATUS
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    /* Create a child that waits to be killed and then exits */
    pid = fork();
    if (pid == 0) {
        pause();
        printf(" -- child caught signal, exiting\n");
        testing_end_quiet();
        exit(0);
    }
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /* Cause the child to exit, then retrieve the event */
    printf(" -- killing process %d\n", (int) pid);
    if (kill(pid, SIGKILL) < 0)
        die("kill");
    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);

    kev.data = SIGKILL; /* What we expected the process exit code to be */
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;

    kevent_cmp(&kev, buf);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_proc_exit_status_ok(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int fflags;
    int sync_pipe[2];
    char go = 'g';

    /*
     *  macOS requires NOTE_EXITSTATUS to get the
     *  exit code of the process, FreeBSD always
     *  provides it.
     */
#ifdef NOTE_EXITSTATUS
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    /* Pipe-gate the child's exit on the parent's kevent_add. */
    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char buf2;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &buf2, 1) != 1)
            _exit(127);
        printf(" -- child got go-ahead, exiting (0)\n");
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);

    kev.data = 0; /* What we expected the process exit code to be */
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;

    kevent_cmp(&kev, buf);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_proc_exit_status_error(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int fflags;
    int sync_pipe[2];
    char go = 'g';

    /*
     *  macOS requires NOTE_EXITSTATUS to get the
     *  exit code of the process, FreeBSD always
     *  provides it.
     */
#ifdef NOTE_EXITSTATUS
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char buf2;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &buf2, 1) != 1)
            _exit(127);
        printf(" -- child got go-ahead, exiting (64)\n");
        testing_end_quiet();
        exit(64);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);

    kev.data = 64 << 8; /* What we expected the process exit code to be */
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;

    kevent_cmp(&kev, buf);
    test_no_kevents(ctx->kqfd);
}

/*
 * Regression test for EV_DISABLE suppressing pending deliveries.
 *
 * BSD semantics: EV_DISABLE means "don't deliver" - including
 * events that have already been queued internally but haven't
 * yet been copyout'd to the caller.  Pre-fix:
 *
 *   - POSIX backend: kn_disable removed the knote from the waiter
 *     list but left it on kf_ready if the wait thread had already
 *     linked it there.  copyout would still emit it.
 *
 *   - Linux pidfd backend: KEVENT_WAIT_DROP_LOCK lets another
 *     thread call EV_DISABLE between epoll_wait fetching the
 *     ready event into our TLS evbuf and us getting to copyout;
 *     the disabled knote would be dispatched anyway.
 *
 * This test does the disable-after-trigger-before-drain dance and
 * asserts no event is delivered.
 */
static void
test_kevent_proc_disable_drains(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int sync_pipe[2];
    char go = 'g';
    int fflags;
    /* Short timeout - we expect zero events. */
    struct timespec timeout = { 0, 100L * 1000L * 1000L }; /* 100ms */
    int got;

#ifdef __APPLE__
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char b;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &b, 1) != 1)
            _exit(127);
        printf(" -- child got go-ahead, exiting (0)\n");
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);

    /* Arm. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /* Release child - it exits, queueing the event internally. */
    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    /* Reap so the next test isn't tripped by the zombie. */
    if (waitpid(pid, NULL, 0) != pid)
        die("waitpid");

    /*
     * Now disable BEFORE draining.  On the POSIX backend the wait
     * thread may already have linked the knote into kf_ready; on
     * Linux a previous epoll_wait may already have the pidfd's
     * ready event in its TLS buffer.  Either way, EV_DISABLE must
     * suppress the pending delivery.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_DISABLE, fflags, 0, NULL);

    /* Expect no event within the timeout. */
    got = kevent_get_timeout(ret, NUM_ELEMENTS(ret), ctx->kqfd, &timeout);
    if (got != 0)
        die("expected 0 events after EV_DISABLE, got %d", got);

    /* Clean up. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_DELETE, fflags, 0, NULL);
}

/*
 * Regression test for the "EV_ADD with no NOTE_* fflags then later
 * re-EV_ADD with NOTE_EXIT" path in the Linux pidfd backend.
 *
 * libkqueue's Linux EVFILT_PROC silently no-ops the EV_ADD when no
 * NOTE_* fflags are set (kn_procfd stays -1), matching native kqueue
 * which delivers nothing for an empty fflags.  A subsequent EV_ADD
 * (which routes through kn_modify) that adds NOTE_EXIT must arm the
 * pidfd at that point - otherwise the knote silently never delivers
 * even though the second EV_ADD looked successful to the caller.
 *
 * The same call also exercises the modify-preserves-EV_ONESHOT|EV_CLEAR
 * fix: copyout's auto-delete and edge-triggered behaviour both depend
 * on those bits being live on kn->kev.flags.
 */
static void
test_kevent_proc_modify_arms_late(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int sync_pipe[2];
    char go = 'g';
    int fflags;

#ifdef __APPLE__
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char b;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &b, 1) != 1)
            _exit(127);
        printf(" -- child got go-ahead, exiting (0)\n");
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);

    /*
     * Step 1: EV_ADD with no NOTE_* fflags.  Native kqueue accepts
     * this as a register-but-deliver-nothing; libkqueue's Linux
     * backend short-circuits in kn_create and leaves kn_procfd unset.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, 0, 0, NULL);

    /*
     * Step 2: re-EV_ADD with NOTE_EXIT.  This routes through kn_modify
     * which must (a) preserve EV_ONESHOT|EV_CLEAR that kn_create would
     * normally force, and (b) call kn_create now that kn_procfd is
     * still -1 from step 1 but the caller now wants delivery.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /* Release the child. */
    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    /*
     * Pre-fix this would hang: kn_modify only memcpy'd the new flags
     * onto kn->kev without calling kn_create, so the pidfd never got
     * armed, and the kqueue never saw the child exit.  kevent_get
     * with the test harness's default short timeout fails the test.
     */
    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);

    kev.data = 0;
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;
    kevent_cmp(&kev, buf);
    test_no_kevents(ctx->kqfd);
}

/*
 * Regression test for the "register an EVFILT_PROC knote for a child
 * that has already exited" path.  Native kqueue and Linux pidfd both
 * deliver NOTE_EXIT immediately; libkqueue's POSIX backend used to
 * register silently and only deliver if an unrelated sibling later
 * exited and triggered a SIGCHLD scan.  kn_create now probes via
 * waitid(WNOWAIT) so the event fires synchronously.
 */
static void
test_kevent_proc_already_exited(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int sync_pipe[2];
    pid_t early;
    int fflags;
    char b;

#ifdef __APPLE__
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    if (pipe(sync_pipe) < 0)
        die("pipe");

    early = fork();
    if (early == 0) {
        close(sync_pipe[0]);
        /* close(sync_pipe[1]) happens implicitly in exit(0). */
        testing_end_quiet();
        _exit(0);
    }
    close(sync_pipe[1]);
    printf(" -- early-exit child created (pid %d)\n", (int) early);

    /*
     * Block until child closes its write end (= exits and goes
     * zombie).  read returning 0 here is the synchronisation point;
     * after this the child is in zombie state and a probe will see
     * it.
     */
    while (read(sync_pipe[0], &b, 1) > 0) { }
    close(sync_pipe[0]);

    test_no_kevents(ctx->kqfd);

    /*
     * Register AFTER the child has exited.  Pre-fix on the POSIX
     * backend this would silently sit forever; post-fix kn_create's
     * waitid probe queues the event for immediate delivery.
     */
    kevent_add(ctx->kqfd, &kev, early, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);

    kev.data = 0;
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;
    kevent_cmp(&kev, buf);

    /* Reap the zombie so we don't leave one around for later tests. */
    if (waitpid(early, NULL, 0) != early)
        die("waitpid");

    test_no_kevents(ctx->kqfd);
}

/*
 * Regression test for the fflags-mutation-on-modify path.
 *
 * Native kqueue allows EV_ADD on an existing knote to replace the
 * fflags.  libkqueue's Linux pidfd backend has to mirror that: if a
 * caller registers EVFILT_PROC with NOTE_EXIT, then re-EV_ADDs
 * without NOTE_EXIT, the pidfd must be detached from epoll and
 * closed.  Pre-fix the pidfd stayed armed and the kqueue still
 * delivered NOTE_EXIT for a registration the caller had explicitly
 * walked back.
 */
static void
test_kevent_proc_modify_disarms(struct test_context *ctx)
{
    struct kevent kev;
    int sync_pipe[2];
    char go = 'g';
    int fflags;

#ifdef __APPLE__
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char b;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &b, 1) != 1)
            _exit(127);
        printf(" -- child got go-ahead, exiting (0)\n");
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);

    /* Step 1: arm with NOTE_EXIT. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /*
     * Step 2: re-EV_ADD with no NOTE_* fflags.  kn_modify must tear
     * down the prior arm so the kqueue doesn't fire on exit.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, 0, 0, NULL);

    /* Release the child. */
    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    /*
     * Reap the child so the test harness's process accounting stays
     * sane.  The kqueue should NOT see the exit.
     */
    if (waitpid(pid, NULL, 0) != pid)
        die("waitpid");

    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_proc_multiple_kqueue(struct test_context *ctx)
{
    struct kevent kev, buf_a[2], buf_b[2];
    int fflags;
    int kq_b;
    int sync_pipe[2];
    char go = 'g';

    /*
     *  macOS requires NOTE_EXITSTATUS to get the
     *  exit code of the process, FreeBSD always
     *  provides it.
     */
#ifdef NOTE_EXITSTATUS
    fflags = NOTE_EXIT | NOTE_EXITSTATUS;
#else
    fflags = NOTE_EXIT;
#endif

    kq_b = kqueue();
    if (kq_b < 0)
        die("kqueue");

    /*
     * Use a pipe to gate the child's exit on the parent finishing
     * both EVFILT_PROC registrations.  EVFILT_PROC | NOTE_EXIT only
     * fires once per process (the kernel's exit1() calls KNOTE on
     * p->p_klist exactly once before detaching), so any kqueue not
     * yet attached when the child exits will block forever.  The
     * earlier 100ms sleep raced with kevent_add on slow VMs.
     */
    if (pipe(sync_pipe) < 0)
        die("pipe");

    pid = fork();
    if (pid == 0) {
        char buf;
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &buf, 1) != 1)
            _exit(127);  /* parent died before signalling */
        printf(" -- child got go-ahead, exiting (64)\n");
        testing_end_quiet();
        exit(64);
    }
    close(sync_pipe[0]);
    printf(" -- child created (pid %d)\n", (int) pid);

    test_no_kevents(ctx->kqfd);
    test_no_kevents(kq_b);

    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);
    kevent_add(kq_b, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /* Both kqueues are now attached to p_klist - release the child. */
    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    {
        struct timespec ts = { 5, 0 };  /* fail fast instead of CI-timeout */
        if (kevent_get_timeout(buf_a, NUM_ELEMENTS(buf_a), ctx->kqfd, &ts) != 1)
            die("test_kevent_proc_multiple_kqueue: timeout waiting for NOTE_EXIT on ctx->kqfd");
        ts.tv_sec = 5; ts.tv_nsec = 0;
        if (kevent_get_timeout(buf_b, NUM_ELEMENTS(buf_b), kq_b, &ts) != 1)
            die("test_kevent_proc_multiple_kqueue: timeout waiting for NOTE_EXIT on kq_b");
    }

    kev.data = 64 << 8; /* What we expected the process exit code to be */
    kev.flags = EV_ADD | EV_ONESHOT | EV_CLEAR | EV_EOF;

    kevent_cmp(&kev, buf_a);
    kevent_cmp(&kev, buf_b);
    test_no_kevents(ctx->kqfd);
    test_no_kevents(kq_b);

    close(kq_b);
}

#ifdef TODO
void
test_kevent_signal_disable(struct test_context *ctx)
{
    const char *test_id = "kevent(EVFILT_SIGNAL, EV_DISABLE)";
    struct kevent kev;

    test_begin(test_id);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DISABLE, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /* Block SIGUSR1, then send it to ourselves */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGKILL);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("sigprocmask");
    if (kill(getpid(), SIGKILL) < 0)
        die("kill");

    test_no_kevents();

    success();
}

void
test_kevent_signal_enable(struct test_context *ctx)
{
    const char *test_id = "kevent(EVFILT_SIGNAL, EV_ENABLE)";
    struct kevent kev, buf;

    test_begin(test_id);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ENABLE, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /* Block SIGUSR1, then send it to ourselves */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("sigprocmask");
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags = EV_ADD | EV_CLEAR;
#if LIBKQUEUE
    kev.data = 1; /* WORKAROUND */
#else
    kev.data = 2; // one extra time from test_kevent_signal_disable()
#endif
    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1)
    kevent_cmp(&kev, &buf);

    /* Delete the watch */
    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    success();
}

void
test_kevent_signal_del(struct test_context *ctx)
{
    const char *test_id = "kevent(EVFILT_SIGNAL, EV_DELETE)";
    struct kevent kev;

    test_begin(test_id);

    /* Delete the kevent */
    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /* Block SIGUSR1, then send it to ourselves */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("sigprocmask");
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    test_no_kevents();
    success();
}

void
test_kevent_signal_oneshot(struct test_context *ctx)
{
    const char *test_id = "kevent(EVFILT_SIGNAL, EV_ONESHOT)";
    struct kevent kev, buf;

    test_begin(test_id);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /* Block SIGUSR1, then send it to ourselves */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("sigprocmask");
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1)
    kevent_cmp(&kev, &buf);

    /* Send another one and make sure we get no events */
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    test_no_kevents();

    success();
}
#endif

void
test_evfilt_proc(struct test_context *ctx)
{
    signal(SIGUSR1, sig_handler);

    /* Create a child that waits to be killed and then exits */
    pid = fork();
    if (pid == 0) {
        pause();
        testing_end_quiet();
        exit(2);
    }
    printf(" -- child created (pid %d)\n", (int) pid);

    test(kevent_proc_add, ctx);
    test(kevent_proc_delete, ctx);
    test(kevent_proc_get, ctx);
    test(kevent_proc_exit_status_ok, ctx);
    test(kevent_proc_exit_status_error, ctx);
    test(kevent_proc_modify_arms_late, ctx);
    test(kevent_proc_modify_disarms, ctx);
    test(kevent_proc_disable_drains, ctx);
    test(kevent_proc_already_exited, ctx);
    test(kevent_proc_multiple_kqueue, ctx);

    signal(SIGUSR1, SIG_DFL);

#if TODO
    test_kevent_signal_add();
    test_kevent_signal_del();
    test_kevent_signal_get();
    test_kevent_signal_disable();
    test_kevent_signal_enable();
    test_kevent_signal_oneshot();
#endif
}
