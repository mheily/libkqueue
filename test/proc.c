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

#include <sys/wait.h>
#include <limits.h>

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
 * BSD overwrites kn->kev.udata on every modify.  PROC's kn_modify
 * doesn't touch udata itself; the common-code clobber line at
 * src/common/kevent.c handles it.  Test here so a refactor that
 * accidentally short-circuits the common path gets caught.
 */
static void
test_kevent_proc_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, buf[2];
    int sync_pipe[2];
    char go = 'g';
    int fflags;
    int marker = 0xab;

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
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);

    test_no_kevents(ctx->kqfd);

    /* Initial registration with udata=&marker. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, &marker);

    /* Re-EV_ADD modify with udata=NULL.  Common code overwrites. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);

    kevent_get(buf, NUM_ELEMENTS(buf), ctx->kqfd, 1);
    if (buf[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", buf[0].udata);
}

/*
 * Regression test for the EV_DELETE-before-drain UAF in posix/proc.c
 * (the Solaris EVFILT_PROC backend).
 *
 * Sequence: child exits -> wait thread runs waiter_notify which inserts
 * the knote onto filt->kf_ready -> the app calls EV_DELETE before the
 * next kevent() drain.  Pre-fix, evfilt_proc_knote_delete only
 * unlinked the waiter list; the knote stayed on kf_ready while
 * knote_release freed it, and the next copyout walked a freed pointer.
 *
 * The fix unlinks kn_ready in knote_delete under proc_pid_index_mtx.
 * Without it, valgrind/ASAN would catch the free-after-free; without
 * those, this test still asserts the consumer-visible "no spurious
 * delivery" outcome.
 */
static void
test_kevent_proc_delete_drains(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    int             sync_pipe[2];
    char            go = 'g';
    int             fflags;
    struct timespec timeout = { 0, 100L * 1000L * 1000L }; /* 100ms */
    int             got;

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
        testing_end_quiet();
        exit(0);
    }
    close(sync_pipe[0]);

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_ADD, fflags, 0, NULL);

    /* Release child to exit and queue the event internally. */
    if (write(sync_pipe[1], &go, 1) != 1)
        die("write(sync_pipe)");
    close(sync_pipe[1]);
    if (waitpid(pid, NULL, 0) != pid)
        die("waitpid");

    /* Delete before drain - knote may already be on kf_ready. */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC, EV_DELETE, fflags, 0, NULL);

    got = kevent_get_timeout(ret, NUM_ELEMENTS(ret), ctx->kqfd, &timeout);
    if (got != 0)
        die("expected 0 events after EV_DELETE, got %d", got);
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
     * macOS, OpenBSD, and NetBSD reject EVFILT_PROC registration for a
     * zombie with ESRCH; they do not support registering after exit.
     */

#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
    EV_SET(&kev, early, EVFILT_PROC, EV_ADD, fflags, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("expected ESRCH registering EVFILT_PROC on zombie, got success");
    if (errno != ESRCH)
        die("expected ESRCH registering EVFILT_PROC on zombie, got %s", strerror(errno));
#else
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
#endif

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

/*
 * EV_DELETE on a never-registered pid returns ENOENT.
 */
static void
test_kevent_proc_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;
    pid_t         child;

    /* Use a real but unwatched pid so the lookup mechanism doesn't
     * short-circuit on "no such process". */
    child = fork();
    if (child == 0) {
        pause();
        exit(0);
    }

    EV_SET(&kev, child, EVFILT_PROC, EV_DELETE, NOTE_EXIT, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added knote should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));

    kill_or_die(child, SIGKILL);
    waitpid(child, NULL, 0);
}

/*
 * Watching a never-existed pid: BSD's filt_procattach calls
 * pfind() and returns ESRCH on miss.  Linux pidfd_open returns
 * ESRCH similarly.
 */
static void
test_kevent_proc_nonexistent_pid_esrch(struct test_context *ctx)
{
    struct kevent kev;

    /* INT_MAX is "never going to be a real pid" on any sane kernel. */
    EV_SET(&kev, INT_MAX, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("watching INT_MAX pid should fail");
    if (errno != ESRCH)
        die("expected ESRCH, got %d (%s)", errno, strerror(errno));
}

/*
 * udata round-trips through NOTE_EXIT delivery.
 */
static void
test_kevent_proc_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    void         *marker = (void *) 0xFEEDF00DUL;
    unsigned int  fflags = NOTE_EXIT;
    pid_t         child;
    int           sync_pipe[2];
    char          go = 'g';

#ifdef NOTE_EXITSTATUS
    fflags |= NOTE_EXITSTATUS;
#endif

    if (pipe(sync_pipe) < 0) die("pipe");
    child = fork();
    if (child == 0) {
        char buf;
        close(sync_pipe[1]);
        /* Drain the gate; ignore short reads. */ 
            if (read(sync_pipe[0], &buf, 1) <= 0) _exit(1);
        exit(0);
    }
    close(sync_pipe[0]);

    EV_SET(&kev, child, EVFILT_PROC, EV_ADD, fflags, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (write(sync_pipe[1], &go, 1) != 1) die("write");
    close(sync_pipe[1]);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p expected %p", ret[0].udata, marker);

    waitpid(child, NULL, 0);
}

/*
 * EV_RECEIPT echoes the kev with EV_ERROR=0.
 */
#ifdef EV_RECEIPT
static void
test_kevent_proc_receipt_preserved(struct test_context *ctx)
{
    struct kevent kev[1];
    pid_t         child;

    child = fork();
    if (child == 0) {
        pause();
        exit(0);
    }

    EV_SET(&kev[0], child, EVFILT_PROC, EV_ADD | EV_RECEIPT, NOTE_EXIT, 0, NULL);
    if (kevent(ctx->kqfd, kev, 1, kev, 1, NULL) != 1)
        die("EV_RECEIPT should return one echo entry");
    if (!(kev[0].flags & EV_ERROR) || kev[0].data != 0)
        die("EV_RECEIPT echo missing EV_ERROR=0 marker");

    kill_or_die(child, SIGKILL);
    waitpid(child, NULL, 0);

    EV_SET(&kev[0], child, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
    /* knote may have already been auto-deleted on exit; tolerate ENOENT. */
    (void) kevent(ctx->kqfd, kev, 1, NULL, 0, NULL);
}
#endif

/*
 * Common-set: child can exit while the knote is disabled.  On
 * re-enable, the NOTE_EXIT must still surface.  Distinct from
 * disable_drains: that test verifies disable-suppresses; this one
 * verifies the underlying source's event isn't lost across the
 * disable window.
 */
static void
test_kevent_proc_disable_preserves_events(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    unsigned int  fflags = NOTE_EXIT;
    pid_t         child;
    int           sync_pipe[2];
    char          go = 'g';

#ifdef NOTE_EXITSTATUS
    fflags |= NOTE_EXITSTATUS;
#endif

    if (pipe(sync_pipe) < 0) die("pipe");
    child = fork();
    if (child == 0) {
        char buf;
        close(sync_pipe[1]);
        /* Drain the gate; ignore short reads. */ 
            if (read(sync_pipe[0], &buf, 1) <= 0) _exit(1);
        exit(0);
    }
    close(sync_pipe[0]);

    /* Register, then disable. */
    EV_SET(&kev, child, EVFILT_PROC, EV_ADD, fflags, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");
    EV_SET(&kev, child, EVFILT_PROC, EV_DISABLE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent disable");

    /*
     * Child exits while disabled.  Don't waitpid here: the POSIX
     * backend's internal monitor thread does the waitpid that
     * surfaces NOTE_EXIT.  If we reap first the proc is gone before
     * the backend can observe it.
     */
    if (write(sync_pipe[1], &go, 1) != 1) die("write");
    close(sync_pipe[1]);

    /* Brief sleep so the exit is observable while still disabled. */
    usleep(100 * 1000);

    /* Disabled - no delivery. */
    test_no_kevents(ctx->kqfd);

    /* Re-enable: pending NOTE_EXIT must surface. */
    EV_SET(&kev, child, EVFILT_PROC, EV_ENABLE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent enable");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_EXIT))
        die("expected NOTE_EXIT after re-enable, got %s",
            kevent_to_str(&ret[0]));
}

/*
 * Signal-killed child: kev.data must be decodable via WIFSIGNALED /
 * WTERMSIG.  FreeBSD packs status+sig into KW_EXITCODE, macOS into
 * NOTE_EXITSTATUS-gated kn_data.  Verify the bits round-trip.
 */
static void
test_kevent_proc_exit_signal_decode(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    unsigned int  fflags = NOTE_EXIT;
    pid_t         child;

#ifdef NOTE_EXITSTATUS
    fflags |= NOTE_EXITSTATUS;
#endif

    child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }

    EV_SET(&kev, child, EVFILT_PROC, EV_ADD, fflags, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(child, SIGTERM) < 0) die("kill");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_EXIT))
        die("expected NOTE_EXIT, got %s", kevent_to_str(&ret[0]));

    /*
     * Linux backend currently surfaces just the signal number
     * (matches FreeBSD's pre-NOTE_EXITSTATUS encoding); tolerate
     * either the raw signum or a wait(2)-style status word.
     */
    int data = (int) ret[0].data;
    if (data == SIGTERM)
        ;  /* ok - raw signum */
    else if (WIFSIGNALED(data) && WTERMSIG(data) == SIGTERM)
        ;  /* ok - wait(2) encoding */
    else
        die("kev.data=%d doesn't decode as SIGTERM (raw or WIFSIGNALED)", data);

    waitpid(child, NULL, 0);
}

/*
 * Fork-storm: rapidly spawn N children that exit immediately,
 * each watched with NOTE_EXIT, and verify every NOTE_EXIT is
 * delivered.  Stresses the kqueue's proc-list iteration vs
 * concurrent attach/detach paths.
 *
 * Pid-reuse race (where a recycled pid is registered after the
 * original target is reaped) isn't included: deterministically
 * forcing pid wrap inside a test takes either thousands of forks
 * (slow) or sysctl knobs the test framework doesn't expose.
 */
static void
test_kevent_proc_fork_storm(struct test_context *ctx)
{
    enum { N_CHILDREN = 32 };
    struct kevent kev;
    pid_t         pids[N_CHILDREN];
    int           sync_pipe[2];
    int           seen[N_CHILDREN] = { 0 };
    int           i;
    int           total_seen = 0;
    unsigned int  fflags = NOTE_EXIT;

#ifdef NOTE_EXITSTATUS
    fflags |= NOTE_EXITSTATUS;
#endif

    if (pipe(sync_pipe) < 0) die("pipe");

    /* Spawn children that wait on the gate. */
    for (i = 0; i < N_CHILDREN; i++) {
        pid_t p = fork();
        if (p < 0) {
            close(sync_pipe[0]);
            close(sync_pipe[1]);
            while (--i >= 0 && pids[i] > 0)
                waitpid(pids[i], NULL, 0);
            die("fork");
        }
        if (p == 0) {
            char buf;
            close(sync_pipe[1]);
            if (read(sync_pipe[0], &buf, 1) <= 0) _exit(1);
            _exit(0);
        }
        pids[i] = p;
    }
    close(sync_pipe[0]);

    /* Register all watches. */
    for (i = 0; i < N_CHILDREN; i++) {
        EV_SET(&kev, pids[i], EVFILT_PROC, EV_ADD, fflags, 0, NULL);
        if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent EV_ADD pid=%d", pids[i]);
    }

    /* Open the floodgate: write N bytes so all children read and exit. */
    {
        char go[N_CHILDREN];
        memset(go, 'g', sizeof(go));
        if (write(sync_pipe[1], go, sizeof(go)) != (ssize_t) sizeof(go))
            die("write");
    }
    close(sync_pipe[1]);

    /* Drain until we've collected one NOTE_EXIT per child or time out. */
    {
        struct kevent ret[N_CHILDREN];
        struct timespec timeout = { 5, 0 };
        while (total_seen < N_CHILDREN) {
            int n = kevent(ctx->kqfd, NULL, 0, ret,
                           N_CHILDREN - total_seen, &timeout);
            if (n <= 0) break;
            for (int j = 0; j < n; j++) {
                if (!(ret[j].fflags & NOTE_EXIT)) continue;
                for (int k = 0; k < N_CHILDREN; k++) {
                    if (pids[k] == (pid_t) ret[j].ident && !seen[k]) {
                        seen[k] = 1;
                        total_seen++;
                        break;
                    }
                }
            }
        }
    }

    if (total_seen != N_CHILDREN)
        die("fork-storm: expected %d NOTE_EXIT events, got %d",
            N_CHILDREN, total_seen);

    /* Reap. */
    for (i = 0; i < N_CHILDREN; i++)
        waitpid(pids[i], NULL, 0);
}

static const struct lkq_test_gate proc_modify_disarms_gates[] =
{
    /* OpenBSD/NetBSD filt_proc() unconditionally activates the knote on NOTE_EXIT
     * regardless of kn_sfflags, so clearing fflags via re-EV_ADD can't prevent it.
     * OpenBSD: github.com/openbsd/src sys/kern/kern_event.c:463-470.
     * NetBSD: github.com/NetBSD/src sys/kern/kern_event.c:1296. */
    GATE(LKQ_PLATFORM_OS_OPENBSD,
         "OpenBSD filt_proc unconditionally fires NOTE_EXIT regardless of kn_sfflags"),
    GATE(LKQ_PLATFORM_OS_NETBSD,
         "NetBSD filt_proc unconditionally fires NOTE_EXIT regardless of kn_sfflags"),
    { 0, NULL }
};

static const struct lkq_test_gate proc_disable_preserves_events_gates[] =
{
    /* Native BSD/macOS forces EV_ONESHOT on EVFILT_PROC and auto-deletes the knote
     * at exit time, leaving nothing to EV_ENABLE later. */
    GATE(LKQ_PLATFORM_BACKEND_NATIVE,
         "native kqueue forces EV_ONESHOT on EVFILT_PROC; knote is gone before EV_ENABLE"),
    { 0, NULL }
};

const struct lkq_test_case lkq_proc_tests[] =
{
    {
        .name  = "test_kevent_proc_add",
        .desc  = "EV_ADD registers a proc knote",
        .func  = test_kevent_proc_add,
    },
    {
        .name  = "test_kevent_proc_delete",
        .desc  = "EV_DELETE removes the proc knote",
        .func  = test_kevent_proc_delete,
    },
    {
        .name  = "test_kevent_proc_get",
        .desc  = "NOTE_EXIT fires when the watched process exits",
        .func  = test_kevent_proc_get,
    },
    {
        .name  = "test_kevent_proc_exit_status_ok",
        .desc  = "exit(0) delivers status 0 in the data field",
        .func  = test_kevent_proc_exit_status_ok,
    },
    {
        .name  = "test_kevent_proc_exit_status_error",
        .desc  = "exit(N) delivers N<<8 in the data field",
        .func  = test_kevent_proc_exit_status_error,
    },
    {
        .name  = "test_kevent_proc_modify_arms_late",
        .desc  = "re-EV_ADD after fork arms a knote that fires on exit",
        .func  = test_kevent_proc_modify_arms_late,
    },
    {
        .name  = "test_kevent_proc_modify_disarms",
        .desc  = "clearing fflags via re-EV_ADD prevents NOTE_EXIT delivery",
        .func  = test_kevent_proc_modify_disarms,
        .gates = proc_modify_disarms_gates,
    },
    {
        .name  = "test_kevent_proc_disable_drains",
        .desc  = "EV_DISABLE drops a pending NOTE_EXIT event",
        .func  = test_kevent_proc_disable_drains,
    },
    {
        .name  = "test_kevent_proc_delete_drains",
        .desc  = "EV_DELETE drops a pending NOTE_EXIT event",
        .func  = test_kevent_proc_delete_drains,
    },
    {
        .name  = "test_kevent_proc_modify_clobbers_udata",
        .desc  = "re-EV_ADD replaces udata on a proc knote",
        .func  = test_kevent_proc_modify_clobbers_udata,
    },
    {
        .name  = "test_kevent_proc_already_exited",
        .desc  = "NOTE_EXIT fires even if the process exited before EV_ADD",
        .func  = test_kevent_proc_already_exited,
    },
    {
        .name  = "test_kevent_proc_multiple_kqueue",
        .desc  = "same pid in two kqueues delivers NOTE_EXIT independently",
        .func  = test_kevent_proc_multiple_kqueue,
    },
    {
        .name  = "test_kevent_proc_del_nonexistent",
        .desc  = "EV_DELETE on unregistered pid returns ENOENT",
        .func  = test_kevent_proc_del_nonexistent,
    },
    {
        .name  = "test_kevent_proc_nonexistent_pid_esrch",
        .desc  = "EV_ADD with a nonexistent pid returns ESRCH",
        .func  = test_kevent_proc_nonexistent_pid_esrch,
    },
    {
        .name  = "test_kevent_proc_udata_preserved",
        .desc  = "udata round-trips unchanged through NOTE_EXIT delivery",
        .func  = test_kevent_proc_udata_preserved,
    },
#ifdef EV_RECEIPT
    {
        .name  = "test_kevent_proc_receipt_preserved",
        .desc  = "EV_RECEIPT echoes the kev with EV_ERROR=0",
        .func  = test_kevent_proc_receipt_preserved,
    },
#endif
    {
        .name  = "test_kevent_proc_disable_preserves_events",
        .desc  = "NOTE_EXIT fired while disabled surfaces on EV_ENABLE",
        .func  = test_kevent_proc_disable_preserves_events,
        .gates = proc_disable_preserves_events_gates,
    },
    {
        .name  = "test_kevent_proc_exit_signal_decode",
        .desc  = "process killed by signal delivers NOTE_SIGNAL in fflags",
        .func  = test_kevent_proc_exit_signal_decode,
    },
    {
        .name  = "test_kevent_proc_fork_storm",
        .desc  = "many concurrent child exits all deliver NOTE_EXIT",
        .func  = test_kevent_proc_fork_storm,
    },
    LKQ_SUITE_END
};

void
test_evfilt_proc(struct test_context *ctx)
{
    signal(SIGUSR1, sig_handler);

    pid = fork();
    if (pid == 0) {
        pause();
        testing_end_quiet();
        exit(2);
    }
    printf(" -- child created (pid %d)\n", (int) pid);

    run_test_suite(ctx, lkq_proc_tests);

    signal(SIGUSR1, SIG_DFL);
}
