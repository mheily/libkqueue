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

static void *
test_libkqueue_fork_no_hang_thread(void *arg)
{
    struct test_context *ctx = arg;
    struct kevent receipt;

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

    start = time(NULL);

    /*
     *  Create a new thread
     */
    if (pthread_create(&thread, NULL, test_libkqueue_fork_no_hang_thread, ctx) < 0)
        die("kevent");

    printf("Created test_libkqueue_fork_no_hang_thread [%u]\n", (unsigned int)thread);

    /*
     *  We don't know when the thread will start
     *  listening on the kqueue, so we just
     *  deschedule ourselves for 10ms and hope...
     */
    nanosleep(&(struct timespec){ .tv_nsec = 10000000}, NULL);

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
}

void
test_evfilt_libkqueue(struct test_context *ctx)
{
    test(libkqueue_version, ctx);
    test(libkqueue_version_str, ctx);
    test(libkqueue_fork_no_hang, ctx);
}
#endif
