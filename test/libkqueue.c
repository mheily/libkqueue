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

void
test_evfilt_libkqueue(struct test_context *ctx)
{
    test(libkqueue_version, ctx);
    test(libkqueue_version_str, ctx);
//    test(libkqueue_fork_no_hang, ctx);
}
#endif
