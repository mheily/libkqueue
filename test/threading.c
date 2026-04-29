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
#include <semaphore.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <time.h>

struct trigger_args {
    int         kqfd;
    uintptr_t   ident;
};

struct cross_trigger_args {
    int             kqfd;
    sem_t          *ready;
    struct timespec elapsed;
    int             got_events;
    int             kevent_errno;
};

/* Forward decls so the helpers can be referenced before their
 * definitions further down the file. */
static sem_t *sem_open_anon(const char *tag);
static void   sem_close_anon(sem_t *sem);

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

    /* Signal readiness immediately before the blocking kevent.
     * There's an irreducible scheduling gap between the post and
     * the syscall, but the trigger path is robust to it: an
     * EVFILT_USER NOTE_TRIGGER landing before kevent() entry just
     * leaves a ready event queued for the very next call. */
    if (sem_post(a->ready) != 0) die("sem_post(ready)");

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

    args.ready = sem_open_anon("ready");

    if (pthread_create(&th, NULL, wait_for_user_trigger, &args) != 0)
        die("pthread_create");

    /* Wait for the waiter to be one syscall away from kevent(). */
    if (sem_wait(args.ready) != 0) die("sem_wait(ready)");

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

    sem_close_anon(args.ready);
}

struct close_kqueue_args {
    int     *kqfd;
    sem_t   *ready;
};

static void *
close_kqueue(void *arg)
{
    struct close_kqueue_args *a = arg;

    /* Wait until main is one syscall away from the blocking kevent.
     * Closing before or after entry both produce the EBADF the test
     * expects, so the irreducible post-then-syscall gap is benign. */
    if (sem_wait(a->ready) != 0) die("sem_wait(ready)");

    if (close(*a->kqfd) != 0)
        die("close failed");

    return NULL;
}

static void
test_kevent_threading_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    pthread_t th;
    int kqfd = kqueue();
    sem_t *ready;
    struct close_kqueue_args ka;

    /* Add the event, then trigger it from another thread */
    kevent_add(kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    ready = sem_open_anon("ready");
    ka.kqfd  = &kqfd;
    ka.ready = ready;

    if (pthread_create(&th, NULL, close_kqueue, &ka) != 0)
        die("failed creating thread");

    /* Tell the closer we're about to enter kevent. */
    if (sem_post(ready) != 0) die("sem_post(ready)");

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

    sem_close_anon(ready);
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
    sem_t              *ready;
    atomic_int          stop;
    int                 received;
    int                 errors;
};

static void *
_multi_waiter(void *arg)
{
    struct multi_waiter_args   *a = arg;
    struct kevent               ret[8];
    struct timespec             timeout = { 0, 50L * 1000L * 1000L };  /* 50ms */
    int                         n;

    /* Tell the parent we're about to enter the kevent loop.  An event
     * that arrives between this post and the syscall is queued by the
     * kernel and picked up on the very next iteration. */
    if (a->ready && sem_post(a->ready) != 0) die("sem_post(ready)");

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
    sem_t                      *ready;
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue");

    ready = sem_open_anon("ready");

    for (i = 0; i < N_WAITERS; i++) {
        wargs[i].kqfd = kqfd;
        wargs[i].ready = ready;
        atomic_init(&wargs[i].stop, 0);
        wargs[i].received = 0;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, _multi_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    /* Wait until every waiter has posted ready (about to enter kevent). */
    for (i = 0; i < N_WAITERS; i++)
        if (sem_wait(ready) != 0) die("sem_wait(ready)");

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

    sem_close_anon(ready);
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
    sem_t                      *ready;
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue");

    ready = sem_open_anon("ready");

    for (i = 0; i < N_WAITERS; i++) {
        wargs[i].kqfd = kqfd;
        wargs[i].ready = ready;
        atomic_init(&wargs[i].stop, 0);
        wargs[i].received = 0;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, _multi_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    for (i = 0; i < N_WAITERS; i++)
        if (sem_wait(ready) != 0) die("sem_wait(ready)");

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

    sem_close_anon(ready);
}

/*
 * Common spawn / teardown for the per-filter delete-race tests.
 *
 * Each helper expects the caller to have created the kq and stored
 * its fd in `kqfd`.  spawn_waiters opens an anonymous `ready`
 * semaphore, hands it to every waiter, and blocks until each waiter
 * has posted - i.e. is one syscall away from kevent().  The caller
 * then does its filter-specific churn and calls stop_waiters before
 * close()ing the kq.  Caller is responsible for sem_close_anon on
 * the returned semaphore.
 */
static sem_t *
spawn_waiters(int kqfd, pthread_t *waiters, struct multi_waiter_args *wargs, int n)
{
    sem_t *ready = sem_open_anon("ready");
    int i;

    for (i = 0; i < n; i++) {
        wargs[i].kqfd = kqfd;
        wargs[i].ready = ready;
        atomic_init(&wargs[i].stop, 0);
        wargs[i].received = 0;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, _multi_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    for (i = 0; i < n; i++)
        if (sem_wait(ready) != 0) die("sem_wait(ready)");

    return ready;
}

static void
stop_waiters(pthread_t *waiters, struct multi_waiter_args *wargs, int n)
{
    int i;

    for (i = 0; i < n; i++) wargs[i].stop = 1;

    for (i = 0; i < n; i++) {
        if (pthread_join(waiters[i], NULL) != 0)
            die("pthread_join");
        if (wargs[i].errors > 0)
            die("waiter %d reported %d kevent errors", i, wargs[i].errors);
    }
}

/*
 * Per-filter delete-race torture tests.
 *
 * Same shape as test_kevent_threading_multi_waiter_delete_race but
 * exercising the kn_udata / fds_udata teardown paths of every Linux
 * filter that registers an fd with epoll.  Each test parks N waiters
 * in concurrent epoll_wait and has the parent thread cycle ADD ->
 * trigger (filter-specific) -> DELETE in a tight loop.  A waiter that
 * wakes between trigger and DELETE dispatches normally; one that
 * wakes after sees ud_stale=true in copyout and skips dispatch.
 *
 * ASAN+UBSAN catches any UAF on the deferred-free path; non-
 * instrumented builds at least don't crash.
 */

/* EVFILT_TIMER: each timerfd is its own fd, no shared-eventfd race. */
static void
test_kevent_threading_timer_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    sem_t                      *ready;
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        uintptr_t ident = (uintptr_t) (i + 1);
        struct timespec settle = { 0, 1L * 1000L * 1000L };  /* 1ms */

        /*
         * 1ms one-shot timer plus a 1ms sleep so the timer reliably
         * fires before EV_DELETE.  This forces the kernel to queue
         * a ready event for the timerfd and lets the multi-waiter
         * path actually exercise the timerfd drain.
         */
        EV_SET(&kev, ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (timer add)");

        if (nanosleep(&settle, NULL) < 0) die("nanosleep");

        EV_SET(&kev, ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
            die("kevent (timer delete)");
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close");
    sem_close_anon(ready);
}

/*
 * EVFILT_SIGNAL: signalfd is per-knote, registered with epoll.
 * raise() delivers to the process; signalfd queues; copyout drains.
 *
 * NOTE: multi-waiter delivery on EVFILT_SIGNAL currently dispatches
 * the same signal event to every waker (signalfd_reset returns void;
 * losers see EAGAIN but proceed to dispatch anyway).  That's a
 * separate correctness issue from the deferred-free path this test
 * is targeting - the test still exercises ADD/DELETE under churn,
 * which is what we care about here.
 */
static void
test_kevent_threading_signal_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    struct sigaction            old_sa, ign_sa;
    sem_t                      *ready;
    sigset_t                    mask;
    int                         kqfd;
    int                         i;

    (void) ctx;

    /*
     * Two layers of defence so SIGUSR1 can't terminate the test
     * process during the churn:
     *
     *  1. Install SIG_IGN on SIGUSR1.  EVFILT_SIGNAL fires off the
     *     kernel's signal-delivery path on Linux/macOS/FreeBSD
     *     regardless of disposition, so SIG_IGN doesn't suppress
     *     the kqueue notification - it just stops the default
     *     terminate action if the mask below is somehow bypassed.
     *
     *  2. Block SIGUSR1 in this thread (and inherited by waiters
     *     spawned after) so synchronous delivery becomes pending
     *     on the process rather than running the (now no-op)
     *     handler.
     *
     * Layer 1 alone is sufficient for the test to not die; we keep
     * the mask too because that's the original intended path on
     * Linux libkqueue.  An earlier mask-only version got the
     * FreeBSD CI killed by SIGUSR1 mid-loop, presumably via a
     * libc-internal thread that hadn't inherited the mask.
     */
    memset(&ign_sa, 0, sizeof(ign_sa));
    ign_sa.sa_handler = SIG_IGN;
    sigemptyset(&ign_sa.sa_mask);
    if (sigaction(SIGUSR1, &ign_sa, &old_sa) != 0)
        die("sigaction");

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0)
        die("pthread_sigmask");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (signal add)");

        if (raise(SIGUSR1) != 0) die("raise");

        EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (signal delete)");
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close");
    sem_close_anon(ready);

    /* Drain any signal we left pending and restore mask + handler. */
    {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGUSR1)) {
            int sig;
            sigwait(&mask, &sig);
        }
    }
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    sigaction(SIGUSR1, &old_sa, NULL);
}

#ifdef EVFILT_VNODE
/*
 * EVFILT_VNODE: each watched fd gets its own inotifyfd registered
 * with epoll.  The test doesn't trigger any event - it just churns
 * ADD / DELETE so the kn_udata is defer_free'd repeatedly.
 */
static void
test_kevent_threading_vnode_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    sem_t                      *ready;
    char                        path[1024];
    int                         kqfd, fd;
    int                         i;

    (void) ctx;

    snprintf(path, sizeof(path), "/tmp/libkqueue-vnode-race.%d", (int) getpid());
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (vnode add)");

        /* Touch the file so inotify queues an event. */
        if (write(fd, "x", 1) != 1) die("write");

        EV_SET(&kev, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (vnode delete)");
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close kqfd");
    if (close(fd) < 0) die("close fd");
    unlink(path);
    sem_close_anon(ready);
}
#endif /* EVFILT_VNODE */

#ifdef EVFILT_PROC
/*
 * EVFILT_PROC: each watched pid gets its own pidfd registered with
 * epoll.  Forking is heavy so this test runs fewer iterations.
 */
static void
test_kevent_threading_proc_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 50 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    sem_t                      *ready;
    int                         kqfd;
    int                         i;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        pid_t pid = fork();
        if (pid < 0) die("fork");
        if (pid == 0) {
            /* Child: exit promptly so the kernel queues a NOTE_EXIT. */
            _exit(0);
        }

        EV_SET(&kev, (uintptr_t) pid, EVFILT_PROC, EV_ADD,
               NOTE_EXIT, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (proc add)");

        EV_SET(&kev, (uintptr_t) pid, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
            die("kevent (proc delete)");

        /* Reap so we don't accumulate zombies. */
        if (waitpid(pid, NULL, 0) < 0) die("waitpid");
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close");
    sem_close_anon(ready);
}
#endif /* EVFILT_PROC */

/*
 * EVFILT_READ / EVFILT_WRITE share the platform's fd_state machinery
 * (multiple knotes can target the same underlying fd, so libkqueue
 * keeps a per-fd dedup struct - fds_udata - alive across both knotes).
 * Both tests churn pipe registrations so each iteration allocates a
 * fresh fd_state and a fresh fds_udata, then defer_frees both.
 */
static void
test_kevent_threading_read_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    sem_t                      *ready;
    int                         kqfd;
    int                         pipefd[2];
    int                         i;

    (void) ctx;

    if (pipe(pipefd) < 0) die("pipe");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        EV_SET(&kev, pipefd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (read add)");

        /* Write makes the read side readable; kernel queues an event. */
        if (write(pipefd[1], "x", 1) != 1) die("write");

        EV_SET(&kev, pipefd[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (read delete)");

        /* Drain the pipe so it doesn't fill up over many iterations. */
        {
            char buf[16];
            (void) read(pipefd[0], buf, sizeof(buf));
        }
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close kqfd");
    if (close(pipefd[0]) < 0) die("close pipe[0]");
    if (close(pipefd[1]) < 0) die("close pipe[1]");
    sem_close_anon(ready);
}

static void
test_kevent_threading_write_delete_race(struct test_context *ctx)
{
    enum { N_WAITERS = 4, N_ITERATIONS = 200 };
    pthread_t                   waiters[N_WAITERS];
    struct multi_waiter_args    wargs[N_WAITERS];
    struct kevent               kev;
    sem_t                      *ready;
    int                         kqfd;
    int                         pipefd[2];
    int                         i;

    (void) ctx;

    if (pipe(pipefd) < 0) die("pipe");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");
    ready = spawn_waiters(kqfd, waiters, wargs, N_WAITERS);

    for (i = 0; i < N_ITERATIONS; i++) {
        /* Write side starts writable; kernel will fire immediately. */
        EV_SET(&kev, pipefd[1], EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (write add)");

        EV_SET(&kev, pipefd[1], EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0)
            die("kevent (write delete)");
    }

    stop_waiters(waiters, wargs, N_WAITERS);
    if (close(kqfd) < 0) die("close kqfd");
    if (close(pipefd[0]) < 0) die("close pipe[0]");
    if (close(pipefd[1]) < 0) die("close pipe[1]");
    sem_close_anon(ready);
}

/*
 * Targeted single-delivery checks.
 *
 * BSD kqueue's contract is that each ready event is delivered to
 * exactly one kevent() caller, even with multiple threads concurrently
 * waiting on the same kq.  These tests pin that semantic for filters
 * where libkqueue's drain logic could let a single kernel-side event
 * be dispatched multiple times.
 *
 * Shape:
 *   - spawn N waiter threads on the same kq
 *   - register a knote
 *   - cause exactly one kernel-side fire
 *   - let the waiters drain
 *   - sum events received across all waiters
 *   - assert sum == 1
 *
 * On macOS native kqueue these pass trivially (kqueue itself enforces
 * single-delivery).  On Linux libkqueue they pin the consumer-side
 * drain logic per filter.
 */

/*
 * Coordination for the single-delivery tests.
 *
 *   ready  - posted once per waiter when it's about to enter its
 *            kevent() loop.  The driver sem_waits() N times before
 *            firing the trigger to guarantee every waiter has at
 *            least started; subsequent sched_yields let the
 *            waiters drop into epoll_wait.
 *
 *   events - posted once per kevent the waiter received.  The
 *            driver sem_waits() to consume expected events one at
 *            a time, no timeout.
 *
 * On Linux these are anonymous sem_init semaphores; macOS only
 * supports named semaphores via sem_open, so the tests below open
 * them with a unique name then sem_unlink immediately to keep them
 * anonymous-equivalent.
 */
struct count_waiter_args {
    int                  kqfd;
    atomic_int           stop;
    sem_t               *ready;
    sem_t               *events;
    int                  errors;
};

static void *
_count_waiter(void *arg)
{
    struct count_waiter_args   *a = arg;
    struct kevent               ret[8];
    struct timespec             timeout = { 0, 50L * 1000L * 1000L };  /* 50ms */
    int                         n;

    if (a->ready && sem_post(a->ready) != 0) die("sem_post(ready)");

    while (!a->stop) {
        n = kevent(a->kqfd, NULL, 0, ret, NUM_ELEMENTS(ret), &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            a->errors++;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (sem_post(a->events) != 0) die("sem_post(events)");
        }
    }

    return NULL;
}

/* Open a uniquely-named POSIX semaphore initialised to 0 and
 * unlink it immediately so it behaves like an anonymous semaphore
 * across linux + macOS. */
static sem_t *
sem_open_anon(const char *tag)
{
    char    name[64];
    sem_t  *sem;

    snprintf(name, sizeof(name), "/libkqueue-test-%s-%d", tag, (int) getpid());
    sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (sem == SEM_FAILED) die("sem_open");
    if (sem_unlink(name) != 0) die("sem_unlink");
    return sem;
}

static void
sem_close_anon(sem_t *sem)
{
    if (sem_close(sem) != 0) die("sem_close");
}

/*
 * Drive a single-delivery test for an arbitrary trigger.
 *
 * Spawns N waiters on the kq, fires the trigger via the supplied
 * callback, then drains expected_deliveries events from the events
 * semaphore.  After the drain, attempts one extra sem_trywait to
 * detect a spurious extra delivery (multi-deliver bug).
 *
 * The trigger fires AFTER all N waiters have posted to `ready`, so
 * we know they've all entered their kevent loop.  No nanosleep
 * anywhere in the coordination path.
 */
static void
run_single_delivery(int kqfd, int n_waiters, int expected_deliveries,
                    void (*fire)(void *), void *fire_ctx, const char *label)
{
    pthread_t                   waiters[16];
    struct count_waiter_args    wargs[16];
    sem_t                      *ready, *events;
    int                         i;

    if (n_waiters > (int) NUM_ELEMENTS(waiters))
        die("n_waiters=%d exceeds local buffer", n_waiters);

    ready  = sem_open_anon("ready");
    events = sem_open_anon("events");

    for (i = 0; i < n_waiters; i++) {
        wargs[i].kqfd = kqfd;
        atomic_init(&wargs[i].stop, 0);
        wargs[i].ready = ready;
        wargs[i].events = events;
        wargs[i].errors = 0;
        if (pthread_create(&waiters[i], NULL, _count_waiter, &wargs[i]) != 0)
            die("pthread_create");
    }

    /* Wait until every waiter is about to enter kevent(). */
    for (i = 0; i < n_waiters; i++)
        if (sem_wait(ready) != 0) die("sem_wait(ready)");

    fire(fire_ctx);

    /* Drain the expected number of deliveries. */
    for (i = 0; i < expected_deliveries; i++)
        if (sem_wait(events) != 0) die("sem_wait(events)");

    /* Sanity check: any extra delivery now is a multi-deliver bug. */
    if (sem_trywait(events) == 0)
        die("%s: spurious extra delivery (expected %d)",
            label, expected_deliveries);

    for (i = 0; i < n_waiters; i++) wargs[i].stop = 1;
    for (i = 0; i < n_waiters; i++) {
        if (pthread_join(waiters[i], NULL) != 0) die("pthread_join");
        if (wargs[i].errors > 0)
            die("waiter %d reported %d errors", i, wargs[i].errors);
    }

    sem_close_anon(ready);
    sem_close_anon(events);
}

struct signal_single_delivery_fire_ctx { int signum; };

static void
_signal_single_delivery_fire(void *vctx)
{
    struct signal_single_delivery_fire_ctx *fc = vctx;
    /* kill(getpid(), ...) targets the process so the signal lands in
     * the process-wide pending set; raise() targets the current thread
     * which would put it in *this thread's* pending set, where the
     * waiter threads' signalfd reads might not see it. */
    if (kill(getpid(), fc->signum) != 0) die("kill");
}

static void
_signal_single_delivery_noop_handler(int sig)
{
    (void) sig;
}

static void
test_kevent_threading_signal_single_delivery(struct test_context *ctx)
{
    struct kevent                            kev;
    struct sigaction                         sa, prev;
    struct signal_single_delivery_fire_ctx   fire_ctx = { .signum = SIGUSR1 };
    int                                      kqfd;

    (void) ctx;

    /*
     * Install a no-op handler.  SIG_IGN would tell the kernel to
     * discard the signal entirely - neither signalfd nor BSD's
     * EVFILT_SIGNAL would see it.  The no-op handler runs to
     * completion (does nothing) but the kernel still routes the
     * signal through the EVFILT_SIGNAL machinery.
     */
    sa.sa_handler = _signal_single_delivery_noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, &prev) < 0) die("sigaction");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (signal add)");

    run_single_delivery(kqfd, 4, 1, _signal_single_delivery_fire, &fire_ctx, "EVFILT_SIGNAL");

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    (void) kevent(kqfd, &kev, 1, NULL, 0, NULL);
    if (close(kqfd) < 0) die("close");
    sigaction(SIGUSR1, &prev, NULL);
}

#ifdef EVFILT_PROC
struct proc_single_delivery_fire_ctx { pid_t pid; };

static void
_proc_single_delivery_fire(void *vctx)
{
    struct proc_single_delivery_fire_ctx *fc = vctx;
    /*
     * Just send the signal.  Don't waitpid here - evfilt_proc
     * copyout calls waitid(WNOWAIT) to read the exit status, and
     * that fails with ECHILD as soon as the child is reaped.  With
     * multiple waiters dispatching the same exit, the first reap
     * would race the rest.  Defer waitpid() until run_single_delivery
     * has finished draining.
     */
    if (kill(fc->pid, SIGTERM) < 0) die("kill");
}

static void
test_kevent_threading_proc_single_delivery(struct test_context *ctx)
{
    struct kevent                          kev;
    struct proc_single_delivery_fire_ctx   fire_ctx;
    int                                    kqfd;
    pid_t                                  pid;

    (void) ctx;

    /* Child waits in pause() until the parent explicitly kills it.
     * No timing race between fork and the parent registering the
     * watcher. */
    pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        for (;;) pause();
    }
    fire_ctx.pid = pid;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, (uintptr_t) pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (proc add)");

    run_single_delivery(kqfd, 4, 1, _proc_single_delivery_fire, &fire_ctx, "EVFILT_PROC");

    /* All dispatch is done; safe to reap. */
    if (waitpid(pid, NULL, 0) < 0) die("waitpid");
    if (close(kqfd) < 0) die("close");
}
#endif /* EVFILT_PROC */

struct user_single_delivery_fire_ctx { int kqfd; uintptr_t ident; };

static void
_user_single_delivery_fire(void *vctx)
{
    struct user_single_delivery_fire_ctx *fc = vctx;
    struct kevent k;
    EV_SET(&k, fc->ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);
    if (kevent(fc->kqfd, &k, 1, NULL, 0, NULL) < 0) die("kevent (user trigger)");
}

static void
test_kevent_threading_user_single_delivery(struct test_context *ctx)
{
    struct kevent                          kev;
    struct user_single_delivery_fire_ctx   fire_ctx;
    int                                    kqfd;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (user add)");

    fire_ctx.kqfd = kqfd;
    fire_ctx.ident = 1;
    run_single_delivery(kqfd, 4, 1, _user_single_delivery_fire, &fire_ctx, "EVFILT_USER");

    if (close(kqfd) < 0) die("close");
}

struct timer_single_delivery_fire_ctx { int kqfd; uintptr_t ident; };

static void
_timer_single_delivery_fire(void *vctx)
{
    struct timer_single_delivery_fire_ctx *fc = vctx;
    struct kevent k;
    /*
     * EV_ADD with a 1ms one-shot timer.  The fire is the kernel's
     * arming of the timer plus the 1ms expiry; from the test's
     * perspective the EV_ADD kevent() returns immediately and the
     * waiters then race for the single timer expiration event.
     */
    EV_SET(&k, fc->ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1, NULL);
    if (kevent(fc->kqfd, &k, 1, NULL, 0, NULL) < 0) die("kevent (timer add)");
}

static void
test_kevent_threading_timer_single_delivery(struct test_context *ctx)
{
    struct kevent                           kev;
    struct timer_single_delivery_fire_ctx   fire_ctx;
    int                                     kqfd;

    (void) ctx;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    fire_ctx.kqfd = kqfd;
    fire_ctx.ident = 1;
    run_single_delivery(kqfd, 4, 1, _timer_single_delivery_fire, &fire_ctx, "EVFILT_TIMER");

    /* Clean up - the EV_ONESHOT may have already auto-deleted, so
     * tolerate ENOENT. */
    EV_SET(&kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
        die("kevent (timer delete)");
    if (close(kqfd) < 0) die("close");
}

#ifdef EVFILT_VNODE
struct vnode_single_delivery_fire_ctx { int fd; };

static void
_vnode_single_delivery_fire(void *vctx)
{
    struct vnode_single_delivery_fire_ctx *fc = vctx;
    if (write(fc->fd, "x", 1) != 1) die("write");
}

static void
test_kevent_threading_vnode_single_delivery(struct test_context *ctx)
{
    struct kevent                          kev;
    struct vnode_single_delivery_fire_ctx  fire_ctx;
    char                                   path[1024];
    int                                    kqfd, fd;

    (void) ctx;

    snprintf(path, sizeof(path), "/tmp/libkqueue-vnode-single.%d", (int) getpid());
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open");
    fire_ctx.fd = fd;

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (vnode add)");

    run_single_delivery(kqfd, 4, 1, _vnode_single_delivery_fire, &fire_ctx, "EVFILT_VNODE");

    EV_SET(&kev, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    (void) kevent(kqfd, &kev, 1, NULL, 0, NULL);
    if (close(kqfd) < 0) die("close kqfd");
    if (close(fd) < 0) die("close fd");
    unlink(path);
}
#endif /* EVFILT_VNODE */

struct read_single_delivery_fire_ctx { int writefd; };

static void
_read_single_delivery_fire(void *vctx)
{
    struct read_single_delivery_fire_ctx *fc = vctx;
    if (write(fc->writefd, "x", 1) != 1) die("write");
}

static void
test_kevent_threading_read_single_delivery(struct test_context *ctx)
{
    struct kevent                          kev;
    struct read_single_delivery_fire_ctx   fire_ctx;
    int                                    kqfd, pipefd[2];

    (void) ctx;

    if (pipe(pipefd) < 0) die("pipe");
    fire_ctx.writefd = pipefd[1];

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, pipefd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (read add)");

    run_single_delivery(kqfd, 4, 1, _read_single_delivery_fire, &fire_ctx, "EVFILT_READ");

    EV_SET(&kev, pipefd[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (read delete)");
    if (close(kqfd) < 0) die("close kqfd");
    if (close(pipefd[0]) < 0) die("close pipe[0]");
    if (close(pipefd[1]) < 0) die("close pipe[1]");
}

struct write_single_delivery_fire_ctx { int kqfd; int writefd; };

static void
_write_single_delivery_fire(void *vctx)
{
    /*
     * Write side starts writable; the fire is the EV_ADD itself,
     * which causes the kernel to immediately mark the fd ready.
     */
    struct write_single_delivery_fire_ctx *fc = vctx;
    struct kevent k;
    EV_SET(&k, fc->writefd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(fc->kqfd, &k, 1, NULL, 0, NULL) < 0) die("kevent (write add)");
}

static void
test_kevent_threading_write_single_delivery(struct test_context *ctx)
{
    struct kevent                          kev;
    struct write_single_delivery_fire_ctx  fire_ctx;
    int                                    kqfd, pipefd[2];

    (void) ctx;

    if (pipe(pipefd) < 0) die("pipe");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    fire_ctx.kqfd = kqfd;
    fire_ctx.writefd = pipefd[1];
    run_single_delivery(kqfd, 4, 1, _write_single_delivery_fire, &fire_ctx, "EVFILT_WRITE");

    EV_SET(&kev, pipefd[1], EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    (void) kevent(kqfd, &kev, 1, NULL, 0, NULL);
    if (close(kqfd) < 0) die("close kqfd");
    if (close(pipefd[0]) < 0) die("close pipe[0]");
    if (close(pipefd[1]) < 0) die("close pipe[1]");
}

void
test_threading(struct test_context *ctx)
{
#if defined(NATIVE_KQUEUE) && !defined(__FreeBSD__)
	/*
	 * Skipped on FreeBSD: native kqueue doesn't unblock a kevent()
	 * parked on a kq when another thread close()s that kq.  The
	 * close caller waits for in-flight kevents to drain (kq_state
	 * KQ_TASKDRAIN), the kevent caller waits for an event that
	 * never arrives, and we deadlock.  libkqueue's Linux backend
	 * has explicit close-wake plumbing that this test exercises;
	 * macOS native kqueue happens to wake parked threads on close;
	 * FreeBSD native kqueue does not.  No portable fix available
	 * without a non-close wake mechanism, which would invalidate
	 * what the test is checking.
	 */
	test(kevent_threading_close, ctx);
#endif
#ifdef TEST_DROP_LOCK_WAKE
	/*
	 * Requires the backend to drop the kq lock across kevent_wait
	 * (KEVENT_WAIT_DROP_LOCK).  Backends that hold the lock through
	 * the wait can't deliver a cross-thread EVFILT_USER trigger
	 * until the waiter returns for some other reason.
	 */
	test(kevent_threading_user_trigger_cross_thread, ctx);
#endif
	test(kevent_threading_multi_waiter_delete_race, ctx);
	test(kevent_threading_multi_waiter_oneshot, ctx);
	test(kevent_threading_timer_delete_race, ctx);
	test(kevent_threading_signal_delete_race, ctx);
#ifndef __sun
	/*
	 * Concurrent delete-race tests exercise EV_ADD/EV_DELETE
	 * against another thread parked in kevent_wait.  Solaris
	 * holds the kq lock across port_getn (no KEVENT_WAIT_DROP_LOCK)
	 * AND lacks UAF-safe portev_user wrappers around port_event
	 * retrieval, so these races deadlock or trip EFAULT.  Gated
	 * until the backend grows knote refcounting + lock-drop.
	 */
# ifdef EVFILT_VNODE
	test(kevent_threading_vnode_delete_race, ctx);
# endif
# ifdef EVFILT_PROC
	test(kevent_threading_proc_delete_race, ctx);
# endif
#endif
	test(kevent_threading_read_delete_race, ctx);
	test(kevent_threading_write_delete_race, ctx);
	test(kevent_threading_user_single_delivery, ctx);
	test(kevent_threading_timer_single_delivery, ctx);
	test(kevent_threading_signal_single_delivery, ctx);
#ifdef EVFILT_VNODE
	test(kevent_threading_vnode_single_delivery, ctx);
#endif
#ifdef EVFILT_PROC
	test(kevent_threading_proc_single_delivery, ctx);
#endif
	test(kevent_threading_read_single_delivery, ctx);
	test(kevent_threading_write_single_delivery, ctx);
}
