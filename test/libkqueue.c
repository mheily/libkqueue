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
#include "common.h"
#include <semaphore.h>
#include <time.h>

#ifdef EVFILT_LIBKQUEUE
static void
test_libkqueue_version(struct test_context *ctx)
{
    struct kevent kev, receipt;

    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD, NOTE_VERSION, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, &receipt, 1, &(struct timespec){}) != 1) {
        printf("Unable to add the following kevent:\n%s\n",
               kevent_to_str(&kev));
        die("kevent");
    }

    if (receipt.data == 0) {
        printf("No version number returned");
        die("kevent");
    }
}

static void
test_libkqueue_version_str(struct test_context *ctx)
{
    struct kevent kev, receipt;

    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD, NOTE_VERSION_STR, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, &receipt, 1, &(struct timespec){}) != 1) {
        printf("Unable to add the following kevent:\n%s\n",
               kevent_to_str(&kev));
        die("kevent");
    }

    if (!strlen((char *)receipt.udata)) {
        printf("empty version number returned");
        die("kevent");
    }
}

struct fork_no_hang_args {
    struct test_context *ctx;
    sem_t               *ready;
};

static void *
test_libkqueue_fork_no_hang_thread(void *arg)
{
    struct fork_no_hang_args *fa = arg;
    struct test_context *ctx = fa->ctx;
    struct kevent receipt;

    /* Tell the parent we're one syscall away from kevent.  The parent
     * will pthread_cancel us; kevent() is a cancellation point and
     * cancellation is delivered whether we're inside the syscall or
     * about to enter it. */
    if (sem_post(fa->ready) != 0) die("sem_post(ready)");

    /*
     *  We shouldn't ever wait for 10 seconds...
     */
    if (kevent(ctx->kqfd, NULL, 0, &receipt, 1, &(struct timespec){ .tv_sec = 10 }) > 0) {
        printf("Failed waiting...\n");
        die("kevent - waiting");
    }

    printf("Shouldn't have hit timeout, expected to be cancelled\n");
    die("kevent - timeout");

    return NULL;
}

static void
test_libkqueue_fork_no_hang(struct test_context *ctx)
{
    struct kevent kev, receipt;
    pthread_t thread;
    time_t start, end;
    pid_t child;
    sem_t *ready;
    struct fork_no_hang_args fa;
    char ready_name[64];

    start = time(NULL);

    snprintf(ready_name, sizeof(ready_name), "/libkqueue-fnh-%d", (int) getpid());
    ready = sem_open(ready_name, O_CREAT | O_EXCL, 0600, 0);
    if (ready == SEM_FAILED) die("sem_open");
    if (sem_unlink(ready_name) != 0) die("sem_unlink");

    fa.ctx   = ctx;
    fa.ready = ready;

    /*
     *  Create a new thread
     */
    if (pthread_create(&thread, NULL, test_libkqueue_fork_no_hang_thread, &fa) < 0)
        die("kevent");

    printf("Created test_libkqueue_fork_no_hang_thread [%u]\n", (unsigned int)thread);

    /* Wait for the listener to be one syscall away from kevent. */
    if (sem_wait(ready) != 0) die("sem_wait(ready)");

    /*
     *  Test that we can fork... The child exits
     *  immediately, we're just check that we _can_
     *  fork().
     */
#if 0
    child = fork();
    if (child == 0) {
        testing_end_quiet();
        exit(EXIT_SUCCESS);
    }

    printf("Forked child [%u]\n", (unsigned int)child);
#endif

    /*
     *  This also tests proper behaviour of kqueues
     *  on cancellation.
     */
    if (pthread_cancel(thread) < 0)
        die("pthread_cancel");

    if ((time(NULL) - start) > 5) {
        printf("Thread hung instead of being cancelled");
        die("kevent");
    }

    if (sem_close(ready) != 0) die("sem_close");
}

#if defined(LIBKQUEUE_BACKEND_POSIX)
/*
 * NOTE_FILE_POLL_INTERVAL is the POSIX-backend hook for tuning how
 * the wait loop handles "only always-ready knotes" state (regular
 * file knotes with nothing else registered).  Default 0 ==
 * sched_yield + timeout=0; positive values clamp pselect to that
 * many nanoseconds.  Other backends return ENOSYS because they
 * have kernel-driven file-knote dispatch.
 */
static void
test_libkqueue_file_poll_interval_set(struct test_context *ctx)
{
    struct kevent kev;

    /* Positive value: 50ms in nanoseconds. */
    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD,
           NOTE_FILE_POLL_INTERVAL, 50 * 1000 * 1000, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0)
        die("kevent: %s", strerror(errno));

    /* Zero: switches back to yield-mode (the default). */
    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD,
           NOTE_FILE_POLL_INTERVAL, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0)
        die("kevent (interval=0): %s", strerror(errno));
}

static void
test_libkqueue_file_poll_interval_rejects_negative(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD,
           NOTE_FILE_POLL_INTERVAL, -1, NULL);
    errno = 0;
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) >= 0)
        die("negative interval should have been rejected");
    if (errno != EINVAL)
        die("expected EINVAL on negative interval, got %s",
            strerror(errno));
}

/*
 * Behavioural test: with NOTE_FILE_POLL_INTERVAL set to 50ms and
 * an only-always-ready kqueue (one zero-byte file knote), a
 * blocking kevent() should sleep for ~the interval before
 * re-evaluating, not busy-loop.  Verify by checking elapsed time
 * is at least one interval and bounded above.
 */
static void
test_libkqueue_file_poll_interval_sleeps(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    char            path[1024];
    struct timespec t0, t1;
    long            elapsed_ms;
    int             rfd, tmpfd;

    snprintf(path, sizeof(path), "%s/libkqueue-test-XXXXXX", test_tmpdir());
    tmpfd = mkstemp(path);
    if (tmpfd < 0) die("mkstemp");
    close(tmpfd);

    rfd = open(path, O_RDONLY);
    if (rfd < 0) die("open");

    /* 50ms interval. */
    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD,
           NOTE_FILE_POLL_INTERVAL, 50 * 1000 * 1000, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0)
        die("set interval: %s", strerror(errno));

    /* Zero-byte file knote: kqueue is now only-always-ready. */
    EV_SET(&kev, rfd, EVFILT_READ, EV_ADD, 0, 0, &rfd);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0)
        die("EV_ADD: %s", strerror(errno));

    if (clock_gettime(CLOCK_MONOTONIC, &t0) < 0) die("clock_gettime");

    /* No data, no other wakers: should park for ~one interval, then
     * return 0 (the wait loop's "only always_ready, but file knote
     * is at EOF" path).  Use a timeout of 200ms as the upper bound. */
    if (kevent(ctx->kqfd, NULL, 0, ret, 1,
               &(struct timespec){ .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 }) < 0)
        die("wait: %s", strerror(errno));

    if (clock_gettime(CLOCK_MONOTONIC, &t1) < 0) die("clock_gettime");
    elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                 (t1.tv_nsec - t0.tv_nsec) / 1000000;

    /* The kqueue should have actually slept (not busy-looped).  A
     * sleeping wait spends most of the budget in the kernel; a
     * busy-loop returns in microseconds.  Floor at 25ms (half the
     * interval, generous for jitter); ceiling at 250ms. */
    if (elapsed_ms < 25)
        die("kevent returned in %ldms, expected >= 25ms (busy-loop?)",
            elapsed_ms);
    if (elapsed_ms > 250)
        die("kevent returned in %ldms, expected <= 250ms",
            elapsed_ms);

    EV_SET(&kev, rfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
    close(rfd);
    unlink(path);
}
#else
/*
 * Backends with kernel-driven file-knote dispatch
 * (Linux/epoll, Solaris/event ports, Windows/IOCP) don't need
 * a userspace polling interval, so the NOTE returns ENOSYS.
 * Verify the contract here so the call doesn't silently
 * succeed if someone wires it up incorrectly later.
 */
static void
test_libkqueue_file_poll_interval_unsupported(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 0, EVFILT_LIBKQUEUE, EV_ADD,
           NOTE_FILE_POLL_INTERVAL, 50 * 1000 * 1000, NULL);
    errno = 0;
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) >= 0)
        die("expected ENOSYS, NOTE_FILE_POLL_INTERVAL was accepted");
    if (errno != ENOSYS)
        die("expected ENOSYS, got %s", strerror(errno));
}
#endif /* LIBKQUEUE_BACKEND_POSIX */

void
test_evfilt_libkqueue(struct test_context *ctx)
{
    test(libkqueue_version, ctx);
    test(libkqueue_version_str, ctx);
//    test(libkqueue_fork_no_hang, ctx);
#if defined(LIBKQUEUE_BACKEND_POSIX)
    test(libkqueue_file_poll_interval_set, ctx);
    test(libkqueue_file_poll_interval_rejects_negative, ctx);
    test(libkqueue_file_poll_interval_sleeps, ctx);
#else
    test(libkqueue_file_poll_interval_unsupported, ctx);
#endif
}
#endif
