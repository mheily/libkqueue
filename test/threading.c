/*
 * Copyright (c) 2025 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

struct trigger_args {
    int         kqfd;
    uintptr_t   ident;
};

struct cross_trigger_args {
    int             kqfd;
    struct timespec elapsed;
    int             got_events;
    int             kevent_errno;
};

static void *
wait_for_user_trigger(void *arg)
{
    struct cross_trigger_args   *a = arg;
    struct kevent               ret[1];
    struct timespec             start, end;
    /*
     * Long enough that a true cross-thread deadlock manifests as a
     * detectable wait, but bounded so a regression doesn't hang
     * the test suite.  With the libkqueue lock-drop fix in place
     * the trigger arrives in microseconds; without it, the wait
     * runs to completion and the test fails on either timing or
     * "no events received".
     */
    struct timespec             timeout = { 5, 0 };

    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
        die("clock_gettime");

    a->got_events = kevent_get_timeout(ret, 1, a->kqfd, &timeout);
    a->kevent_errno = (a->got_events < 0) ? errno : 0;

    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0)
        die("clock_gettime");

    a->elapsed.tv_sec  = end.tv_sec  - start.tv_sec;
    a->elapsed.tv_nsec = end.tv_nsec - start.tv_nsec;
    if (a->elapsed.tv_nsec < 0) {
        a->elapsed.tv_sec--;
        a->elapsed.tv_nsec += 1000000000L;
    }

    return NULL;
}

/*
 * Reproducer for the cross-thread NOTE_TRIGGER hang.
 *
 * Pre-fix, libkqueue held the per-kq mutex across kevent_wait() -
 * which is epoll_wait() on Linux.  A second thread calling
 * kevent(kq, EV_ENABLE | NOTE_TRIGGER) on the same kq would block
 * on the mutex until the waiter's epoll_wait returned for some
 * other reason, typically the wait's own timeout.  Cross-thread
 * EVFILT_USER wakes - the standard pattern for waking an event
 * loop owned by another thread - effectively didn't work.
 *
 * The test arms a 5-second wait on an EVFILT_USER knote, fires
 * the trigger from a second thread after a brief delay, and
 * asserts:
 *
 *   - The waiter's kevent() returned an event (rather than timing
 *     out).
 *   - The wait completed well under the 5s ceiling - 1s is the
 *     fail-budget; the fix makes it microseconds.
 *
 * Pre-fix the test fails on both axes; post-fix it passes
 * comfortably.
 */
static void
test_kevent_threading_user_trigger_cross_thread(struct test_context *ctx)
{
    struct kevent               kev;
    pthread_t                   th;
    struct cross_trigger_args   args = { 0 };
    struct timespec             warm_up = { 0, 100L * 1000L * 1000L };  /* 100ms */
    int                         kqfd;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue");
    args.kqfd = kqfd;

    /*
     * EV_DISPATCH so the knote auto-disables on first delivery and
     * the trigger uses the EV_ENABLE | NOTE_TRIGGER atomic re-arm
     * idiom - the same pattern rlm_kafka uses for cross-thread DR
     * dispatch.
     */
    kevent_add(kqfd, &kev, 0, EVFILT_USER, EV_ADD | EV_DISPATCH, 0, 0, NULL);

    if (pthread_create(&th, NULL, wait_for_user_trigger, &args) != 0)
        die("pthread_create");

    /*
     * Give the waiter time to enter kevent_wait.  100ms is way
     * more than needed but harmless under load.
     */
    if (nanosleep(&warm_up, NULL) < 0)
        die("nanosleep");

    /* Cross-thread trigger from the parent thread. */
    kevent_add(kqfd, &kev, 0, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);

    if (pthread_join(th, NULL) != 0)
        die("pthread_join");

    if (args.got_events < 0) {
        errno = args.kevent_errno;
        die("waiter kevent returned -1");
    }
    if (args.got_events != 1)
        die("waiter did not receive trigger event (got_events=%d)",
            args.got_events);
    if (args.elapsed.tv_sec >= 1)
        die("waiter took too long: %ld.%09lds (>= 1.0s budget) - "
            "cross-thread trigger likely blocked on the kq mutex",
            (long) args.elapsed.tv_sec, (long) args.elapsed.tv_nsec);

    if (close(kqfd) < 0)
        die("close");
}

static void *
close_kqueue(void *arg)
{
	/* Sleep until we're fairly sure the other thread is waiting */
	sleep(1);

    /* Close the waiting kqueue from this thread */
    if (close(*((int *)arg)) != 0)
		die("close failed");

    return NULL;
}

static void
test_kevent_threading_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    pthread_t th;
	int kqfd = kqueue();

    /* Add the event, then trigger it from another thread */
    kevent_add(kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    if (pthread_create(&th, NULL, close_kqueue, &kqfd) != 0)
        die("failed creating thread");

	/* Wait for event (should be interrupted by close) */
	if (kevent(kqfd, NULL, 0, ret, 1, NULL) == -1) {
		if (errno != EBADF)
			die("kevent failed with the wrong errno");
	} else {
		die("kevent did not fail");
	}

	/* Subsequent calls should also fail with EBADF */
	if (kevent(kqfd, NULL, 0, ret, 1, NULL) == -1) {
		if (errno != EBADF)
			die("kevent failed with the wrong errno (second call)");
	} else {
		die("kevent did not fail (second call)");
	}

    if (pthread_join(th, NULL) != 0)
        die("pthread_join failed");
}

void
test_threading(struct test_context *ctx)
{
#ifdef NATIVE_KQUEUE
	test(kevent_threading_close, ctx);
#endif
	test(kevent_threading_user_trigger_cross_thread, ctx);
}
