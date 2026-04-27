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

/*
 * Stress harness for the multi-waiter / deferred-free path.
 *
 * The Linux libkqueue implementation keeps an `epoll_udata` heap
 * allocation alive across the kevent_wait window so that a thread
 * which has a stale `data.ptr` in its TLS `epoll_events[]` buffer
 * can still safely deref it after another thread has run EV_DELETE
 * on the underlying knote.  The deferred udata is reclaimed only
 * when no kevent() caller with an entry epoch <= the udata's
 * boundary epoch is still in flight.
 *
 * The test below pounds on that path by:
 *
 *   - parking N waiter threads in concurrent epoll_wait calls on
 *     the same kq, with timeouts long enough that the parent's
 *     churn lands while every waiter is sleeping;
 *
 *   - cycling EVFILT_USER knotes from the parent: ADD a knote,
 *     NOTE_TRIGGER it (kernel queues a ready event for whichever
 *     waiter is dispatched), then EV_DELETE it (defer_free the
 *     udata).  A waiter that wakes between the trigger and the
 *     delete will dispatch normally; a waiter that wakes after
 *     the delete will see ud_stale=true in copyout and skip.
 *
 * The dangerous case pre-fix was the second one: the waiter's TLS
 * buffer carried a pointer into a freed heap slot.  ASAN/UBSAN
 * builds catch the UAF directly; non-instrumented builds at least
 * don't crash.
 */
struct multi_waiter_args {
    int                 kqfd;
    volatile int        stop;
    int                 received;
    int                 errors;
};

static void *
multi_waiter(void *arg)
{
    struct multi_waiter_args   *a = arg;
    struct kevent               ret[8];
    struct timespec             timeout = { 0, 50L * 1000L * 1000L };  /* 50ms */
    int                         n;

    while (!a->stop) {
        n = kevent_get_timeout(ret, NUM_ELEMENTS(ret), a->kqfd, &timeout);
        if (n < 0) {
            a->errors++;
            break;
        }
        a->received += n;
    }

    return NULL;
}

static void
test_kevent_threading_multi_waiter_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev[2];
    struct timespec             warm_up = { 0, 50L * 1000L * 1000L };  /* 50ms */
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue");

    for (i = 0; i < N_WAITERS; i++) {
        wargs[i].kqfd = kqfd;
        wargs[i].stop = 0;
        wargs[i].received = 0;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, multi_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    /*
     * Give waiters time to enter epoll_wait at least once so the
     * first round of churn lands while all of them are sleeping.
     */
    if (nanosleep(&warm_up, NULL) < 0)
        die("nanosleep");

    for (i = 0; i < N_ITERATIONS; i++) {
        uintptr_t ident = (uintptr_t) (i + 1);

        /*
         * Add the knote (allocates kn_udata, registers with epoll)
         * and trigger it in a single changelist.  Kernel queues a
         * ready event for whichever waiter epoll_wait dispatches.
         */
        EV_SET(&kev[0], ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        EV_SET(&kev[1], ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);
        if (kevent(kqfd, kev, 2, NULL, 0, NULL) < 0)
            die("kevent (add+trigger)");

        /*
         * Immediately delete the knote.  This calls
         * epoll_ctl(EPOLL_CTL_DEL) and defer_frees the udata while
         * a ready event for it may still be in the waiter's TLS
         * buffer.  Copyout in the waiter must see ud_stale=true and
         * skip dispatch without dereferencing the dangling back-
         * pointer.
         */
        EV_SET(&kev[0], ident, EVFILT_USER, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, kev, 1, NULL, 0, NULL) < 0)
            die("kevent (delete)");
    }

    /*
     * Stop the waiters and let any final timeout cycle drain so
     * every kevent() caller exits and the deferred-free sweep
     * gets a chance to reclaim everything before close().
     */
    for (i = 0; i < N_WAITERS; i++) wargs[i].stop = 1;

    for (i = 0; i < N_WAITERS; i++) {
        if (pthread_join(waiters[i], NULL) != 0)
            die("pthread_join");
        if (wargs[i].errors > 0)
            die("waiter %d reported %d kevent errors", i, wargs[i].errors);
    }

    if (close(kqfd) < 0)
        die("close");
}

/*
 * Reproducer for copyout-time defer_free races.
 *
 * Each ADDed knote carries EV_ONESHOT, so when a waiter dispatches
 * the triggered event the filter's kn_delete runs from inside
 * copyout (via knote_copyout_flag_actions).  That path calls
 * KN_UDATA_DEFER_FREE on the same kq while the caller is still
 * holding kq_mtx and still in kq_inflight.  The boundary captured
 * is the current kq_next_epoch, which may be higher than the
 * caller's own entry epoch if other threads entered during the
 * wait.  The deferred-free sweep is run at the caller's exit and
 * must correctly leave the entry pinned until every later caller
 * has also exited.
 *
 * Combined with N concurrent waiters, this stresses the
 * "boundary >= caller's epoch" path that the rebase + sweep code
 * has to get right.
 */
static void
test_kevent_threading_multi_waiter_oneshot(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev[2];
    struct timespec             warm_up = { 0, 50L * 1000L * 1000L };
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue");

    for (i = 0; i < N_WAITERS; i++) {
        wargs[i].kqfd = kqfd;
        wargs[i].stop = 0;
        wargs[i].received = 0;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, multi_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    if (nanosleep(&warm_up, NULL) < 0)
        die("nanosleep");

    for (i = 0; i < N_ITERATIONS; i++) {
        uintptr_t ident = (uintptr_t) (i + 1);

        EV_SET(&kev[0], ident, EVFILT_USER,
               EV_ADD | EV_ONESHOT | EV_CLEAR, 0, 0, NULL);
        EV_SET(&kev[1], ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);
        if (kevent(kqfd, kev, 2, NULL, 0, NULL) < 0)
            die("kevent (add+trigger oneshot)");
    }

    for (i = 0; i < N_WAITERS; i++) wargs[i].stop = 1;

    for (i = 0; i < N_WAITERS; i++) {
        if (pthread_join(waiters[i], NULL) != 0)
            die("pthread_join");
        if (wargs[i].errors > 0)
            die("waiter %d reported %d kevent errors", i, wargs[i].errors);
    }

    if (close(kqfd) < 0)
        die("close");
}

void
test_threading(struct test_context *ctx)
{
#ifdef NATIVE_KQUEUE
	test(kevent_threading_close, ctx);
#endif
	test(kevent_threading_user_trigger_cross_thread, ctx);
	test(kevent_threading_multi_waiter_delete_race, ctx);
	test(kevent_threading_multi_waiter_oneshot, ctx);
}
