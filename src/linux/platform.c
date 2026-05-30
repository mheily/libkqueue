/*
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
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

#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>

#include <inttypes.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#include "../common/private.h"

/*
 * Per-thread epoll event buffer used to ferry data between
 * kevent_wait() and kevent_copyout().
 */
static __thread struct epoll_event epoll_events[MAX_KEVENT];

/*
 * Monitoring thread that takes care of cleaning up kqueues (on linux only)
 */
static pthread_t monitoring_thread;
static pid_t monitoring_tid; /* Monitoring thread */
static pthread_mutex_t monitoring_thread_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t monitoring_thread_cond = PTHREAD_COND_INITIALIZER;

enum thread_exit_state {
    THREAD_EXIT_STATE_SELF_CANCEL = 0,
    THREAD_EXIT_STATE_CANCEL_LOCKED,
    THREAD_EXIT_STATE_CANCEL_UNLOCKED,
};

/*
 * Monitoring thread is exiting because the process is terminating
 */
static enum thread_exit_state monitoring_thread_state;

/*
 * Close-detection epoll.  Every kqueue's pipefd[0] (read end of its
 * close-detect pipe) is registered here with EPOLLHUP | EPOLLONESHOT
 * and ev.data.ptr set to the owning struct kqueue.  When the user
 * closes the kqueue fd (pipefd[1], the write end) the read end goes
 * HUP and the monitoring thread frees the kqueue, identifying it
 * directly from ev.data.ptr - no fd-number lookup.  EPOLLONESHOT stops
 * a deferred free (in-flight kevent() callers) from re-firing the
 * level-triggered HUP in a busy loop.
 *
 * Process-global, created lazily on the first kqueue(); persists across
 * monitoring-thread restarts.
 */
static int monitoring_epfd = -1;

/*
 * eventfd that libkqueue_drain_pending_close writes to wake the
 * monitoring thread for a synchronous flush.  Registered in
 * monitoring_epfd level-triggered (EPOLLIN) with ev.data.ptr == NULL,
 * which the monitor uses to distinguish it from a kqueue HUP.
 */
static int monitoring_drain_efd = -1;

/*
 * Bumped by the monitoring thread each time it finishes flushing a
 * drain request; libkqueue_drain_pending_close waits for it to advance.
 * Protected by kq_mtx, signalled via monitoring_drain_cond.
 */
static unsigned long monitoring_drain_gen;
static pthread_cond_t monitoring_drain_cond = PTHREAD_COND_INITIALIZER;

/* Per-epoll_wait batch the monitoring thread reaps. */
#define MONITORING_MAX_EVENTS 64

static void
linux_kqueue_free(struct kqueue *kq);

static void
linux_kqueue_interrupt(struct kqueue *kq);

/*
 * TSAN false-positive on this function.
 *
 * It's invoked from a pthread cleanup handler installed via
 * pthread_cleanup_push at the top of the monitoring thread loop.
 * When the cleanup runs (either from pthread_cancel or via
 * pthread_cleanup_pop on self-exit) the synchronisation chain
 * with the main thread's linux_libkqueue_free is:
 *
 *   main: tracing_mutex_lock(&kq_mtx)
 *   main: pthread_cancel(monitoring_thread)
 *   main: tracing_mutex_unlock(&kq_mtx)
 *   main: pthread_join(monitoring_thread)
 *   T1:   cleanup runs, conditionally tracing_mutex_lock(&kq_mtx)
 *
 * The lock-unlock-lock chain on kq_mtx is a happens-before, but
 * TSAN can't always track it across the pthread_cancel + cleanup
 * handler boundary.  TSAN reports races on the kq_mtx metadata
 * fields and on kq_list with M0 in main's lockset but not T1's,
 * even though both threads acquire the same mutex.
 *
 * The protection is real (lock is acquired before any access)
 * but the instrumentation can't see it.  The suppressions file
 * at tools/tsan.supp (`race:monitoring_thread_cleanup`) catches
 * every warning under this stack pattern, applied in CI via
 * TSAN_OPTIONS=suppressions=.
 */
static void
monitoring_thread_cleanup(UNUSED void *arg)
{
    struct kqueue *kq, *kq_tmp;

    if ((monitoring_thread_state == THREAD_EXIT_STATE_CANCEL_LOCKED) ||
        (monitoring_thread_state == THREAD_EXIT_STATE_CANCEL_UNLOCKED)) {

        /*
         * Keep the assertion in kqueue_free happy
         */
        if (monitoring_thread_state == THREAD_EXIT_STATE_CANCEL_UNLOCKED)
            tracing_mutex_lock(&kq_mtx);

        /*
         * If the entire process is exiting, then free all
         * the kqueues.
         *
         * We do this because a close HUP may not have been
         * processed before the process exits, and this avoids
         * ASAN or valgrind raising spurious memory leaks.
         *
         * If the user _hasn't_ closed a KQ fd, then we don't
         * free the underlying memory, and it'll be correctly
         * reported as a memory leak.
         */
        LIST_FOREACH_SAFE(kq, &kq_list, kq_entry, kq_tmp) {
            /*
             * We only cleanup kqueues where their file descriptor
             * has been closed.  Other kqueues may be in the middle
             * of operations with kq->kq_mtx held, and attempting to
             * clean them up would cause a deadlock.  Those are
             * legitimate leaks the user application should have
             * freed before exit.
             */
            dbg_printf("kq=%p - fd=%i explicitly checking for closure", kq, kq->kq_id);
            if (fcntl(kq->kq_id, F_GETFD) < 0) {
                dbg_printf("kq=%p - fd=%i closed, cleaning up: %s",
                           kq, kq->kq_id,
                           errno == EBADF ? "File descriptor already closed" : strerror(errno));
                kqueue_free(kq);
            } else {
                /*
                 * User never closed kqfd.  Interrupt any threads
                 * parked in epoll_wait so they exit kevent() with
                 * EBADF instead of staying stuck forever, then leave
                 * the kq leaked per the existing contract - the user
                 * is responsible for matching kqueue() with close().
                 */
                dbg_printf("kq=%p - fd=%i still open, likely a leak; skipping", kq, kq->kq_id);
                linux_kqueue_interrupt(kq);
            }
        }

        if (monitoring_thread_state == THREAD_EXIT_STATE_CANCEL_UNLOCKED)
            tracing_mutex_unlock(&kq_mtx);
    }

    dbg_printf("tid=%u - monitoring thread exiting (%s)",
               monitoring_tid,
               monitoring_thread_state == THREAD_EXIT_STATE_SELF_CANCEL ?
                   "no kqueues" : "process term");

    /* Reset so that thread can be restarted */
    monitoring_tid = 0;

    /*
     * Wake any libkqueue_drain_pending_close() blocked on us.  If we
     * exit on kq_cnt == 0 after a drain was requested but before we
     * read the drain eventfd, the drainer would otherwise wait on a
     * generation that never advances; monitoring_tid == 0 tells it the
     * monitor is gone and every kqueue is already freed.
     */
    pthread_cond_broadcast(&monitoring_drain_cond);

    if (monitoring_thread_state == THREAD_EXIT_STATE_CANCEL_LOCKED)
        tracing_mutex_unlock(&kq_mtx);
}


/** Free a kqueue whose close-detect pipe reported EPOLLHUP.
 *
 * Called with kq_mtx held.  The kq pointer comes straight from the
 * epoll event's udata, so there's no fd-number translation or lookup.
 * kqueue_free removes the kq from kq_list immediately; if in-flight
 * kevent() callers exist it defers the teardown to the last caller.
 * Either way the EPOLLONESHOT registration won't re-fire, and
 * linux_kqueue_free removes pipefd[0] from monitoring_epfd before
 * closing it.
 */
static void
monitoring_thread_close_kq(struct kqueue *kq)
{
    dbg_printf("kq=%p - fd=%i freeing due to close (EPOLLHUP)", kq, kq->kq_id);
    kqueue_free(kq);
}

/*
 * Monitoring thread: blocks in epoll_wait on monitoring_epfd and frees
 * each kqueue whose pipefd[0] reports EPOLLHUP (the user closed the
 * kqueue fd).  A readable monitoring_drain_efd (udata == NULL) is a
 * synchronous drain request from libkqueue_drain_pending_close.
 */
static void *
monitoring_thread_loop(UNUSED void *arg)
{
    int res = 0;
    pid_t my_tid;
    sigset_t all;

    /* Set the thread's name to something descriptive so it shows up in gdb,
     * etc. glibc >= 2.1.2 supports pthread_setname_np, but this is a safer way
     * to do it for backwards compatibility. Max name length is 16 bytes. */
    prctl(PR_SET_NAME, "libkqueue_mon", 0, 0, 0);

    /*
     * Block all signals here.  Close detection is epoll-based, so this
     * thread needs no signal; blocking keeps stray application signals
     * from interrupting epoll_wait or being delivered to us.
     */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    my_tid = syscall(SYS_gettid);
    dbg_printf("tid=%u - monitoring thread started", my_tid);

    /*
     * Publish monitoring_tid and let kqueue init resume.  Writing
     * the global under the same mutex the parent reads it under
     * (start_thread's cond_wait predicate) gives TSAN the
     * happens-before edge it needs - without it the early
     * write-then-much-later-lock pattern leaves the parent's
     * initial monitoring_tid read unsynchronised with this write.
     */
    pthread_mutex_lock(&monitoring_thread_mtx);    /* Must try to lock to ensure parent is waiting on signal */
    monitoring_tid = my_tid;
    pthread_cond_signal(&monitoring_thread_cond);
    (void) pthread_mutex_unlock(&monitoring_thread_mtx);

    monitoring_thread_state = THREAD_EXIT_STATE_CANCEL_UNLOCKED;
    pthread_cleanup_push(monitoring_thread_cleanup, NULL)
    while (true) {
        struct epoll_event events[MONITORING_MAX_EVENTS];
        bool drain_req = false;
        int n, i;

        /*
         * epoll_wait is the cancellation point; linux_libkqueue_free
         * cancels + joins us here on teardown.  Everything below runs
         * with cancellation disabled so a cancel can't interrupt a
         * kqueue_free mid-flight.
         */
        n = epoll_wait(monitoring_epfd, events, MONITORING_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            dbg_printf("epoll_wait(2): %s", strerror(errno));
            continue;
        }

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        tracing_mutex_lock(&kq_mtx);

        for (i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                drain_req = true;       /* the drain eventfd */
                continue;
            }
            monitoring_thread_close_kq(events[i].data.ptr);
        }

        /*
         * A drain request: a caller wants every already-closed kqueue
         * freed before we acknowledge.  HUP is level-triggered and
         * every such kq's read end is HUP right now, so a zero-timeout
         * sweep collects them all (EPOLLONESHOT means each fires at
         * most once).  Then bump the generation the drainer waits on.
         */
        if (drain_req) {
            uint64_t v;

            while (read(monitoring_drain_efd, &v, sizeof(v)) > 0)
                ;       /* clear the eventfd counter */

            for (;;) {
                int m = epoll_wait(monitoring_epfd, events, MONITORING_MAX_EVENTS, 0);
                if (m <= 0)
                    break;
                for (i = 0; i < m; i++) {
                    if (events[i].data.ptr == NULL)
                        continue;
                    monitoring_thread_close_kq(events[i].data.ptr);
                }
            }

            monitoring_drain_gen++;
            pthread_cond_broadcast(&monitoring_drain_cond);
        }

        /*
         * Exit if there are no more kqueues to monitor
         */
        if (kq_cnt == 0)
            break;

        tracing_mutex_unlock(&kq_mtx);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }

    /*
     * Ensure that any cancellation requests are acted on
     */
    monitoring_thread_state = THREAD_EXIT_STATE_CANCEL_LOCKED;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_testcancel();

    monitoring_thread_state = THREAD_EXIT_STATE_SELF_CANCEL;
    pthread_cleanup_pop(true); /* Executes the cleanup function (monitoring_thread_cleanup) */
    res = pthread_detach(pthread_self());
    if (res != 0)
        dbg_printf("pthread_detach(3): %s", strerror(res));
    tracing_mutex_unlock(&kq_mtx);

    return NULL;
}

static int
linux_kqueue_start_thread(void)
{
    pthread_mutex_lock(&monitoring_thread_mtx);
    if (pthread_create(&monitoring_thread, NULL, monitoring_thread_loop, NULL)) {
         dbg_perror("linux_kqueue_start_thread failure");
         (void) pthread_mutex_unlock(&monitoring_thread_mtx);

         return (-1);
    }
    /*
     * Wait for thread creation to publish monitoring_tid.  Loop on
     * the predicate (monitoring_tid != 0) to be safe against
     * spurious wakeups.
     */
    while (monitoring_tid == 0)
        pthread_cond_wait(&monitoring_thread_cond, &monitoring_thread_mtx);
    (void) pthread_mutex_unlock(&monitoring_thread_mtx);

    return (0);
}

/** Free kqueues on fork
 *
 * Called with kq_mtx held.
 */
static void
linux_libkqueue_fork(void)
{
    struct kqueue *kq, *kq_tmp;

    /*
     * We don't have to cancel the monitor thread here
     * as fork() typically only duplicates the thread
     * which called it.
     *
     * Ensure we don't try and cancel it on exit by
     * clearing the thread id.
     *
     * We don't need to lock the kq_mtx here as we're in
     * the child, and none of our threads will have been
     * duplicated.
     */
    monitoring_tid = 0;

    /*
     * If the process has been forked, then we need
     * to destroy all of the kqueue instances in
     * the fork.
     *
     * This replicates the behaviour of real kqueues
     * on FreeBSD, and also prevents spurious leak
     * reports being raised by LSAN.
     */
    LIST_FOREACH_SAFE(kq, &kq_list, kq_entry, kq_tmp) {
        dbg_printf("kq=%p - cleaning up on fork", kq);

        /*
         * There's very limited cleanups we can do
         * here, as we're only allowed to call
         * async-signal-safe functions in this
         * handler.
         *
         * This means any functions which alloc or
         * free memory are ruled out, meaning we
         * can't release any of the memory allocated
         * to the kqueues or filters.
         *
         * Fortunately close() is async-signal-safe.
         */
        close(kq->epollfd);
        kq->epollfd = -1;

        if ((kq->pipefd[0] > 0) && (close(kq->pipefd[0]) < 0))
            dbg_perror("close(2)");
        kq->pipefd[0] = -1;

        if ((kq->pipefd[1] > 0) && (close(kq->pipefd[1]) < 0))
            dbg_perror("close(2)");
        kq->pipefd[1] = -1;
    }

    /*
     * Drop the inherited monitoring epoll + drain eventfd.  An epoll
     * fd duplicated across fork shares the parent's interest list, so
     * the child must start fresh; reset to -1 and the child's next
     * kqueue() lazily recreates them.  close() is async-signal-safe.
     */
    if (monitoring_epfd >= 0) {
        close(monitoring_epfd);
        monitoring_epfd = -1;
    }
    if (monitoring_drain_efd >= 0) {
        close(monitoring_drain_efd);
        monitoring_drain_efd = -1;
    }
    monitoring_drain_gen = 0;
}

/*
 * We have to use this instead of pthread_detach as there
 * seems to be some sort of race with LSAN and thread cleanup
 * on exit, and if we don't explicitly join the monitoring
 * thread, LSAN reports kqueues as leaked.
 */
static void
linux_libkqueue_free(void)
{
    pid_t tid;

    tracing_mutex_lock(&kq_mtx);
    tid = monitoring_tid; /* Gets trashed when the thread exits */
    if (tid) {
        void *retval;
        int ret;

        dbg_printf("tid=%u - cancelling", tid);
        ret = pthread_cancel(monitoring_thread);
        if (ret != 0)
           dbg_printf("tid=%u - cancel failed: %s", tid, strerror(ret));
        /*
         * We unlock here to allow the monitoring thread
         * to continue if it was processing a cleanup.
         */
        tracing_mutex_unlock(&kq_mtx);

        ret = pthread_join(monitoring_thread, &retval);
        if (ret == 0) {
            if (retval == PTHREAD_CANCELED) {
                dbg_printf("tid=%u - joined with exit_status=PTHREAD_CANCELED", tid);
            } else {
                dbg_printf("tid=%u - joined with exit_status=%" PRIdPTR, tid, (intptr_t)retval);
            }
        } else {
            dbg_printf("tid=%u - join failed: %s", tid, strerror(ret));
        }
    } else {
        tracing_mutex_unlock(&kq_mtx);
    }

    /*
     * Thread is joined (or was never started).  Tear down the
     * process-global monitoring epoll + drain eventfd.
     */
    if (monitoring_epfd >= 0) {
        if (close(monitoring_epfd) < 0)
            dbg_perror("close(monitoring_epfd)");
        monitoring_epfd = -1;
    }
    if (monitoring_drain_efd >= 0) {
        if (close(monitoring_drain_efd) < 0)
            dbg_perror("close(monitoring_drain_efd)");
        monitoring_drain_efd = -1;
    }
}

/** Block until the monitoring thread has freed every closed kqueue
 *
 * The normal close-cleanup path is asynchronous: close(kqfd) closes
 * the write end of the close-detect pipe (pipefd[1]), the read end
 * (pipefd[0]) goes EPOLLHUP in monitoring_epfd, and the monitoring
 * thread frees the kqueue.  Tests that close kqs and return
 * immediately can race the monitoring thread, leaving allocations
 * unfreed when LSAN runs at process teardown.
 *
 * This is a deterministic request/ack handshake, not a poll: we write
 * the drain eventfd to wake the monitoring thread, which flushes every
 * currently-ready HUP (all the just-closed kqs, since HUP is
 * level-triggered) and then bumps monitoring_drain_gen.  We wait for
 * that generation to advance.  No timeout, no fd probing, no fd-reuse
 * hazard - the HUP is the source of truth.
 *
 * If the monitoring thread isn't running (monitoring_tid == 0) it has
 * already exited, which only happens once every kqueue is freed, so
 * there is nothing to wait for.
 *
 * Intended for tests that need deterministic teardown.
 */

/*
 * pthread_cond_wait on monitoring_drain_cond with kq_mtx held.
 *
 * kq_mtx is a tracing_mutex_t: in debug builds it wraps a
 * pthread_mutex_t with lock-tracking metadata, so cond_wait must
 * operate on the inner lock and we restore the bookkeeping by hand
 * around it.  In NDEBUG builds tracing_mutex_t IS a pthread_mutex_t.
 */
static void
monitoring_drain_cond_wait(void)
{
#ifdef NDEBUG
    pthread_cond_wait(&monitoring_drain_cond, &kq_mtx);
#else
    kq_mtx.mtx_status = MTX_UNLOCKED;
    kq_mtx.mtx_owner = -1;
    pthread_cond_wait(&monitoring_drain_cond, &kq_mtx.mtx_lock);
    kq_mtx.mtx_owner = THREAD_ID;
    kq_mtx.mtx_status = MTX_LOCKED;
#endif
}

void VISIBLE
libkqueue_drain_pending_close(void)
{
    unsigned long gen;
    uint64_t one = 1;

    tracing_mutex_lock(&kq_mtx);

    if (monitoring_tid == 0 || monitoring_drain_efd < 0) {
        tracing_mutex_unlock(&kq_mtx);
        return;
    }

    /*
     * Snapshot the generation, then wake the monitor.  We hold kq_mtx
     * across the write and the wait so the monitor can't process the
     * request and exit (kq_cnt == 0) before we start waiting - it
     * blocks on kq_mtx until our cond_wait releases it.
     */
    gen = monitoring_drain_gen;
    if (write(monitoring_drain_efd, &one, sizeof(one)) < 0)
        dbg_perror("write(monitoring_drain_efd)");

    /*
     * Wait until the monitor acknowledges the drain (generation
     * advances) or exits (monitoring_tid clears - it only exits on
     * kq_cnt == 0, so every kqueue is already freed and there is
     * nothing left to drain).
     */
    while (monitoring_drain_gen == gen && monitoring_tid != 0)
        monitoring_drain_cond_wait();

    tracing_mutex_unlock(&kq_mtx);
}

static int
linux_kqueue_init(struct kqueue *kq)
{
    kq->kq_next_epoch = 0;
    TAILQ_INIT(&kq->kq_inflight);
    TAILQ_INIT(&kq->ud_deferred_free);

    kq->epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (kq->epollfd < 0) {
        dbg_perror("epoll_create(2)");
        return (-1);
    }

    /*
     * The standard behaviour when closing a kqueue fd is
     * for the underlying resources to be freed.
     * In order to catch the close on the libkqueue fd,
     * we use a pipe and return the write end as kq_id.
     * Closing the end will cause the pipe to be close which
     * will be caught by the monitoring thread.
     */
    if (pipe2(kq->pipefd, O_CLOEXEC)) {
        if (close(kq->epollfd) < 0)
            dbg_perror("close(2)");
        kq->epollfd = -1;

        return (-1);
    }

    if (filter_register_all(kq) < 0) {
    error:
        if (close(kq->epollfd) < 0)
            dbg_perror("close(2)");
        kq->epollfd = -1;

        if (close(kq->pipefd[0]) < 0)
            dbg_perror("close(2)");
        kq->pipefd[0] = -1;

        if (close(kq->pipefd[1]) < 0)
            dbg_perror("close(2)");
        kq->pipefd[1] = -1;

        return (-1);
    }

    /*
     * O_NONBLOCK - Ensure pipe ends are non-blocking so that there's
     * no chance of them delaying close(), and so the wake-byte write
     * (linux_kqueue_interrupt) and the residual drain in
     * linux_kqueue_free never block.
     */
    if ((fcntl(kq->pipefd[0], F_SETFL, O_NONBLOCK) < 0) ||
        (fcntl(kq->pipefd[1], F_SETFL, O_NONBLOCK) < 0)) {
        dbg_perror("fcntl(2)");
        goto error;
    }

    kq->kq_id = kq->pipefd[1];

    /*
     * Register pipefd[0] in the kq's own epoll set.  When the user
     * closes pipefd[1] (= kq_id), the kernel marks pipefd[0] as
     * "no writers" and every epoll_wait that has it registered
     * returns with EPOLLHUP.  That wakes threads parked in this kq's
     * own epoll_wait so they exit kevent() instead of staying stuck.
     *
     * Copyout sees EPOLL_UDATA_KQ_WAKE and skips the slot so the
     * fake "ready" event isn't surfaced to the application.
     */
    kq->kq_wake_udata = epoll_udata_alloc(EPOLL_UDATA_KQ_WAKE, kq);
    if (kq->kq_wake_udata == NULL) {
        dbg_perror("epoll_udata_alloc(EPOLL_UDATA_KQ_WAKE)");
        goto error;
    }
    {
        struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP,
                                  .data = { .ptr = kq->kq_wake_udata } };
        if (epoll_ctl(kq->epollfd, EPOLL_CTL_ADD, kq->pipefd[0], &ev) < 0) {
            dbg_perror("epoll_ctl(ADD pipefd[0])");
            free(kq->kq_wake_udata);
            kq->kq_wake_udata = NULL;
            goto error;
        }
    }

    /*
     * Lazily create the process-global close-detection epoll and its
     * drain eventfd on first use.  The monitoring thread blocks on
     * this epoll; each kqueue's pipefd[0] is registered below so a
     * user close surfaces as EPOLLHUP.  The drain eventfd has
     * ev.data.ptr == NULL, which the monitor uses to tell it apart
     * from a kqueue HUP.
     */
    if (monitoring_epfd < 0) {
        struct epoll_event de = { .events = EPOLLIN, .data = { .ptr = NULL } };

        monitoring_epfd = epoll_create1(EPOLL_CLOEXEC);
        if (monitoring_epfd < 0) {
            dbg_perror("epoll_create1(monitoring)");
            goto error;
        }
        monitoring_drain_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (monitoring_drain_efd < 0) {
            dbg_perror("eventfd(monitoring_drain)");
            close(monitoring_epfd);
            monitoring_epfd = -1;
            goto error;
        }
        if (epoll_ctl(monitoring_epfd, EPOLL_CTL_ADD, monitoring_drain_efd, &de) < 0) {
            dbg_perror("epoll_ctl(ADD drain eventfd)");
            close(monitoring_drain_efd);
            monitoring_drain_efd = -1;
            close(monitoring_epfd);
            monitoring_epfd = -1;
            goto error;
        }
    }

    /*
     * Register pipefd[0] for close detection.  ev.data.ptr is the kq
     * itself, so the monitoring thread frees it directly with no fd
     * lookup.  EPOLLONESHOT: when kqueue_free defers behind in-flight
     * kevent() callers the HUP stays asserted until the last caller
     * completes teardown; without ONESHOT the level-triggered HUP
     * would busy-loop the monitor until then.
     */
    {
        struct epoll_event me = { .events = EPOLLHUP | EPOLLONESHOT,
                                  .data = { .ptr = kq } };
        if (epoll_ctl(monitoring_epfd, EPOLL_CTL_ADD, kq->pipefd[0], &me) < 0) {
            dbg_perror("epoll_ctl(monitoring ADD pipefd[0])");
            goto error;
        }
    }

    /* Start monitoring thread during first initialization */
    if (monitoring_tid == 0) {
        if (linux_kqueue_start_thread() < 0)
            goto error;
    }
    assert(monitoring_tid != 0);

    dbg_printf("kq=%p - fd=%i monitoring for closure", kq, kq->kq_id);

    return (0);
}

/*
 * Cleanup kqueue resources
 * Should be done while holding kq_mtx
 * return
 * - true if epoll fd and pipes were closed
 * - false if epoll fd was already closed
 */
static void
linux_kqueue_free(struct kqueue *kq)
{
    char buffer;
    ssize_t ret;
    int pipefd;

    if (kq->epollfd > 0) {
        dbg_printf("epoll_fd=%i - closed", kq->epollfd);

        if (close(kq->epollfd) < 0)
            dbg_perror("close(2) - epoll_fd=%i", kq->epollfd);
        kq->epollfd = -1;
    }

    /*
     * read will return 0 on pipe EOF (i.e. if the write end of the pipe has been closed)
     *
     * kq->pipefd[1] should have already been called outside of libkqueue
     * as a signal the kqueue should be closed.
     */
    ret = read(kq->pipefd[0], &buffer, 1);
    if (ret == -1 && errno == EWOULDBLOCK) {
        // Shoudn't happen unless kqops.kqueue_free is called on an open FD
        dbg_puts("kqueue wasn't closed");

        if (close(kq->pipefd[1]) < 0) {
            dbg_perror("close(2) - pipefd[1]=%i", kq->pipefd[1]);
        } else {
            dbg_printf("pipefd[1]=%i - closed", kq->pipefd[1]);
        }
        kq->pipefd[1] = -1;
    } else if (ret > 0) {
        /*
         * Bytes left in the pipe.  Either an in-flight kevent's
         * KQ_WAKE handler hasn't drained yet, or no in-flight
         * waiter ran (e.g., linux_kqueue_interrupt fired but
         * the parked waiters were already gone).  Drain
         * non-blockingly and continue; the pipe is going away
         * with the close below anyway.
         */
        dbg_puts("draining residual data on pipefd[0]");
        while (read(kq->pipefd[0], &buffer, 1) > 0) {}
    }

    pipefd = kq->pipefd[0];
    if (pipefd > 0) {
        /*
         * Remove pipefd[0] from the close-detection epoll before
         * closing it.  Closing would auto-remove it anyway, but an
         * explicit DEL keeps the interest list tidy and closes the
         * window where a HUP could carry this (about-to-be-freed) kq
         * pointer as udata.  ENOENT (never registered, e.g. an init
         * error path) is benign.
         */
        if (monitoring_epfd >= 0)
            (void) epoll_ctl(monitoring_epfd, EPOLL_CTL_DEL, pipefd, NULL);

        if (close(pipefd) < 0) {
            dbg_perror("close(2) - kq_fd=%i", kq->pipefd[0]);
        } else {
            dbg_printf("kq_fd=%i - closed", kq->pipefd[0]);
        }
        kq->pipefd[0] = -1;
    }

    /*
     * Drop any udatas still pending deferred reclamation.  By the
     * time linux_kqueue_free runs, no kevent() callers can be in
     * flight on this kq (the close-on-pipe wakeup is what triggered
     * us).  So no one is left who could be holding a stale data.ptr
     * into one of these udatas - free unconditionally.
     */
    assert(TAILQ_EMPTY(&kq->kq_inflight));
    {
        struct epoll_udata *u;

        while ((u = TAILQ_FIRST(&kq->ud_deferred_free))) {
            TAILQ_REMOVE(&kq->ud_deferred_free, u, ud_deferred_entry);
            free(u);
        }
    }

    /*
     * The kq-wake sentinel udata never goes through deferred free
     * (its lifetime is bound to the kqueue itself), so reclaim it
     * here.  epollfd was already closed above, which removes the
     * registration; if it wasn't closed (test-only path), the udata
     * is still safe to free since no one is in epoll_wait.
     */
    if (kq->kq_wake_udata != NULL) {
        free(kq->kq_wake_udata);
        kq->kq_wake_udata = NULL;
    }
}

/** Wake threads parked in epoll_wait on this kqueue.
 *
 * Writes a single byte to pipefd[1].  The kernel makes pipefd[0]
 * readable, every parked epoll_wait registered against pipefd[0]
 * (via the EPOLL_UDATA_KQ_WAKE sentinel) returns, and copyout
 * surfaces the wake as -1/EBADF.  Best-effort: level-triggered
 * EPOLLIN on a pipe doesn't guarantee every parked waiter wakes
 * for a single byte (the kernel may dispatch to one), but the
 * primary case (user-close) is already handled by EPOLLHUP which
 * does wake all parked waiters.  This hook covers the secondary
 * cases (atexit/fork/kqueue_free_by_id) where the auto-wake
 * doesn't fire.
 *
 * The wake byte only makes pipefd[0] readable (EPOLLIN), not HUP, so
 * the close-detection epoll (which watches EPOLLHUP only) ignores it -
 * the monitoring thread is not disturbed.
 */
static void
linux_kqueue_interrupt(struct kqueue *kq)
{
    char b = 'x';

    if (kq->pipefd[1] < 0) {
        dbg_printf("kq=%p - pipefd[1] already closed, EPOLLHUP path will wake waiters", kq);
        return;
    }
    if (write(kq->pipefd[1], &b, 1) < 0) {
        if (errno != EBADF && errno != EPIPE)
            dbg_printf("kq=%p - write(pipefd[1]): %s", kq, strerror(errno));
        /* EBADF/EPIPE = user already closed kqfd; EPOLLHUP handles it. */
        return;
    }
    dbg_printf("kq=%p - wake byte written to pipefd[1]", kq);
}

/** Allocate a fresh epoll_udata
 *
 * The udata is heap-allocated so the udata's lifetime is independent
 * of the containing knote / fd_state / eventfd: when EV_DELETE frees
 * the containing object, the udata can linger on the kqueue's
 * deferred-free list until every kevent() caller that could have
 * observed the udata via a TLS `epoll_events[]` slot has exited.
 *
 * @param[in] type      What field in the udata union will be used.
 *                      There are different flavours of udata for
 *                      different knotes.
 * @param[in] parent    Pointer to the containing knote / fd_state /
 *                      eventfd.  Aliases ud_kn / ud_fds / ud_efd via
 *                      the union; the caller must pass a `type` that
 *                      matches the kind of pointer being passed.
 * @return the new udata, or NULL on allocation failure.
 */
struct epoll_udata *
epoll_udata_alloc(enum epoll_udata_type type, void *parent)
{
    struct epoll_udata *u = calloc(1, sizeof(*u));

    if (u == NULL) return NULL;

    u->ud_type = type;
    u->ud_kn = parent;

    return u;
}

/** Mark a udata as stale and queue the udata for deferred reclamation
 *
 * Called under kq_mtx by EV_DELETE (or any other path that's about
 * to free the udata's containing object).  After epoll_udata_defer_free
 * returns, the caller can safely free the containing object - copyout
 * will see `ud_stale == true` and skip dispatch without attempting to
 * dereference the knote.
 *
 * @param[in] kq        kqueue the udata belongs to.
 * @param[in] u         udata to defer-free.  Must not already be on
 *                      kq->ud_deferred_free.
 */
void
epoll_udata_defer_free(struct kqueue *kq, struct epoll_udata *u)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    if (u == NULL) return;
    assert(!u->ud_stale);

    /*
     * Insert into the deferred free list with an epoch value
     * greater than all the kevent() entries currently in flight.
     *
     * When those kevent() calls all complete, we guarantee nothing
     * references the udata, and we can free it.
     */
    u->ud_stale = true;
    u->ud_boundary_epoch = kq->kq_next_epoch;
    TAILQ_INSERT_TAIL(&kq->ud_deferred_free, u, ud_deferred_entry);

    dbg_printf("kq=%p udata=%p - deferred free, boundary_epoch=%" PRIu64,
               kq, u, u->ud_boundary_epoch);
}

/** Sweep the deferred-free list, reclaiming eligible udatas
 *
 * A deferred udata is eligible for free once every still-in-flight
 * kevent() caller has an entry epoch strictly greater than the
 * udata's epoch.
 *
 * Both kq_inflight and ud_deferred_free are TAIL-inserted, and
 * kq_next_epoch is incremented only by linux_kevent_enter, so each
 * list is naturally sorted by insertion epoch.  The head of
 * kq_inflight is therefore the lowest still-active entry epoch,
 * and the head of ud_deferred_free is the smallest boundary epoch
 * awaiting reclamation.  The sweep stop at the first udata with
 * an epoch greater than the oldest in-flight kevent() call.
 *
 * When kq_inflight is empty every deferred udata is reclaimed
 * immediately.
 *
 * @param[in] kq        kqueue to sweep.
 */
static void
linux_kqueue_sweep_deferred(struct kqueue *kq)
{
    struct epoll_udata             *ud;
    struct kqueue_kevent_state     *oldest;

    kqueue_mutex_assert(kq, MTX_LOCKED);

    oldest = TAILQ_FIRST(&kq->kq_inflight);
    if (!oldest) {
        /* No in-flight callers - every deferred udata is safe to free. */
        while ((ud = TAILQ_FIRST(&kq->ud_deferred_free))) {
            dbg_printf("kq=%p udata=%p - reclaiming, no callers in flight", kq, ud);
            TAILQ_REMOVE(&kq->ud_deferred_free, ud, ud_deferred_entry);
            free(ud);
        }
        return;
    }

    while ((ud = TAILQ_FIRST(&kq->ud_deferred_free)) && (ud->ud_boundary_epoch < oldest->epoch)) {
        dbg_printf("kq=%p udata=%p - reclaiming, boundary=%" PRIu64 " min_inflight=%" PRIu64,
                   kq, ud, ud->ud_boundary_epoch, oldest->epoch);
        TAILQ_REMOVE(&kq->ud_deferred_free, ud, ud_deferred_entry);
        free(ud);
    }
}

/** Rebase the epoch counter to head off uint64 wrap
 *
 * Both kq_inflight and ud_deferred_free use epoch values for ordering
 * comparisons only - the absolute values don't matter, just the
 * differences.  When kq_next_epoch nears UINT64_MAX, subtract the
 * lowest still-live epoch from every entry on both lists, then
 * rebase kq_next_epoch by the same amount.  Comparisons remain
 * correct because every value moves by the same delta.
 *
 * In practice this never runs - 2^64 increments at 1B kevent()/sec
 * is centuries.  The function exists so the invariant holds even
 * under contrived test conditions that drive kq_next_epoch near the
 * limit directly.
 */
static void
linux_kqueue_epoch_rebase(struct kqueue *kq)
{
    struct kqueue_kevent_state *s;
    struct epoll_udata         *u;
    uint64_t                    base;

    kqueue_mutex_assert(kq, MTX_LOCKED);

    s = TAILQ_FIRST(&kq->kq_inflight);
    base = s ? (s->epoch - 1) : kq->kq_next_epoch;

    dbg_printf("kq=%p - epoch rebase, subtracting %" PRIu64 " from all live epochs", kq, base);

    TAILQ_FOREACH(s, &kq->kq_inflight, entry) {
        s->epoch -= base;
    }
    TAILQ_FOREACH(u, &kq->ud_deferred_free, ud_deferred_entry) {
        u->ud_boundary_epoch -= base;
    }
    kq->kq_next_epoch -= base;
}

/** kevent() entry hook
 *
 * Assigns the caller a monotonic epoch and links the caller's
 * kqueue_kevent_state into kq_inflight under kq_mtx.  linux_kevent_enter
 * runs before any copyin work, so an EV_DELETE issued by the caller's
 * own changelist will see the caller's epoch in the in-flight set when
 * EV_DELETE captures ud_boundary_epoch.
 */
void
linux_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    if (unlikely(kq->kq_next_epoch == UINT64_MAX)) linux_kqueue_epoch_rebase(kq);

    state->epoch = ++kq->kq_next_epoch;
    TAILQ_INSERT_TAIL(&kq->kq_inflight, state, entry);

    dbg_printf("kq=%p - kevent_enter epoch=%" PRIu64, kq, state->epoch);
}

/** kevent() exit hook
 *
 * Unlinks the caller's kqueue_kevent_state from kq_inflight and runs
 * the deferred-free sweep.  Removing the caller may lower the head
 * of kq_inflight (the lowest still-active epoch), which in turn may
 * permit deferred udatas with smaller boundary epochs to be
 * reclaimed by the sweep.
 */
void
linux_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    TAILQ_REMOVE(&kq->kq_inflight, state, entry);
    dbg_printf("kq=%p - kevent_exit epoch=%" PRIu64, kq, state->epoch);

    linux_kqueue_sweep_deferred(kq);
}

static int
linux_kevent_wait_hires(
        struct kqueue *kq,
        const struct timespec *timeout)
{
    int n;
#if HAVE_DECL_PPOLL
    struct pollfd fds;

    dbg_printf("waiting for events (timeout=%ld sec %ld nsec)",
            timeout->tv_sec, timeout->tv_nsec);
    fds.fd = kqueue_epoll_fd(kq);
    fds.events = POLLIN;

    n = ppoll(&fds, 1, timeout, NULL);
#else
    int epoll_fd;
    fd_set fds;

    dbg_printf("waiting for events (timeout=%ld sec %ld nsec)",
            timeout->tv_sec, timeout->tv_nsec);

    epoll_fd = kqueue_epoll_fd(kq);
    FD_ZERO(&fds);
    FD_SET(epoll_fd, &fds);
    n = pselect(epoll_fd + 1, &fds, NULL , NULL, timeout, NULL);
#endif

    if (n < 0) {
        if (errno == EINTR) {
            dbg_puts("signal caught");
            return (-1);
        }
        dbg_perror("ppoll(2) or pselect(2)");
        return (-1);
    }
    return (n);
}

static int
linux_kevent_wait(struct kqueue *kq, int nevents, const struct timespec *ts)
{
    int timeout, nret;

    /* Use a high-resolution syscall if the timeout value's tv_nsec value has a resolution
     * finer than a millisecond. */
    if (ts != NULL && (ts->tv_nsec % 1000000 != 0)) {
        nret = linux_kevent_wait_hires(kq, ts);
        if (nret <= 0)
            return (nret);

        /* epoll_wait() should have ready events */
        timeout = 0;
    } else {
        /* Convert timeout to the format used by epoll_wait() */
        if (ts == NULL)
            timeout = -1;
        else
            timeout = (1000 * ts->tv_sec) + (ts->tv_nsec / 1000000);
    }

    dbg_puts("waiting for events");
    nret = epoll_wait(kqueue_epoll_fd(kq), epoll_events, nevents, timeout);
    if (nret < 0) {
        dbg_perror("epoll_wait");
        return (-1);
    }

    return (nret);
}

static inline int linux_kevent_copyout_ev(struct kevent *el, int nevents, struct epoll_event *ev,
                                          struct filter *filt, struct knote *kn)
{
    int rv;

    rv = filt->kf_copyout(el, nevents, filt, kn, ev);
    dbg_printf("rv=%i", rv);

    if (unlikely(rv < 0)) {
        dbg_puts("knote_copyout failed");
        assert(0);
        return rv;
    }

    /*
     * Don't emit bad events...
     *
     * Fixme - We shouldn't be emitting bad events
     * in the first place?
     */
    if (unlikely(el->filter == 0)) {
        dbg_puts("spurious wakeup, discarding event");
        rv = 0;
    }

    return rv;
}

char const *epoll_op_dump(int op)
{
    static __thread char buf[14];

    buf[0] = '\0';

#define EPOP_DUMP(attrib) \
    if (op == attrib) { \
        strcpy(buf, #attrib); \
        return buf; \
    }

    EPOP_DUMP(EPOLL_CTL_MOD);
    EPOP_DUMP(EPOLL_CTL_ADD);
    EPOP_DUMP(EPOLL_CTL_DEL);

    return buf;
}

static const char *
udata_type(enum epoll_udata_type ud_type)
{
    const char *ud_name[] = {
        [EPOLL_UDATA_KNOTE] = "EPOLL_UDATA_KNOTE",
        [EPOLL_UDATA_FD_STATE] = "EPOLL_UDATA_FD_STATE",
        [EPOLL_UDATA_EVENT_FD] = "EPOLL_UDATA_EVENT_FD",
        [EPOLL_UDATA_KQ_WAKE] = "EPOLL_UDATA_KQ_WAKE",
    };

    if (ud_type < 0 || ud_type >= NUM_ELEMENTS(ud_name))
        return "EPOLL_UDATA_INVALID";
    else
        return ud_name[ud_type];
}

static const char *
epoll_udata_type_dump(const struct epoll_event *ev)
{
    static __thread char buf[64];
    enum epoll_udata_type ud_type;

    ud_type = ((struct epoll_udata *)(ev->data.ptr))->ud_type;

    snprintf(buf, sizeof(buf), "%d (%s)",
             ud_type, udata_type(ud_type));
    return ((const char *) buf);
}

static const char *
epoll_flags_dump(uint32_t events)
{
    static __thread char buf[1024];
    size_t len;

#define EEVENT_DUMP(attrib) \
    if (events & attrib) \
    strncat((char *) buf, #attrib" ", 64);

    snprintf(buf, sizeof(buf), "events=0x%08x (", events);
    EEVENT_DUMP(EPOLLIN);
    EEVENT_DUMP(EPOLLPRI);
    EEVENT_DUMP(EPOLLOUT);
    EEVENT_DUMP(EPOLLRDNORM);
    EEVENT_DUMP(EPOLLRDBAND);
    EEVENT_DUMP(EPOLLWRNORM);
    EEVENT_DUMP(EPOLLWRBAND);
    EEVENT_DUMP(EPOLLMSG);
    EEVENT_DUMP(EPOLLERR);
    EEVENT_DUMP(EPOLLHUP);
    EEVENT_DUMP(EPOLLRDHUP);
    EEVENT_DUMP(EPOLLONESHOT);
    EEVENT_DUMP(EPOLLET);

    len = strlen(buf);
    if (buf[len - 1] == ' ') buf[len - 1] = '\0';    /* Trim trailing space */
    strcat(buf, ")");

#undef EEVENT_DUMP

    return ((const char *) buf);
}

const char *
epoll_event_flags_dump(const struct epoll_event *ev)
{
    return epoll_flags_dump(ev->events);
}

const char *
epoll_event_dump(const struct epoll_event *ev)
{
    static __thread char buf[2147];

    snprintf((char *) buf, sizeof(buf),
             "{ %s, udata=%p, udata_type=%s }",
             epoll_event_flags_dump(ev),
             ev->data.ptr,
             epoll_udata_type_dump(ev));

    return ((const char *) buf);
}

int
linux_kevent_copyout(struct kqueue *kq, int nready, struct kevent *el, int nevents)
{
    struct kevent   *el_p = el, *el_end = el + nevents;
    int             i;

    dbg_printf("got %i events from epoll", nready);

    for (i = 0; i < nready; i++) {
        struct epoll_event    *ev = &epoll_events[i];    /* Thread local storage populated in linux_kevent_wait */
        struct epoll_udata    *epoll_udata = ev->data.ptr;
        int                   rv;

        if (!epoll_udata) {
            dbg_puts("event has no knote, skipping..."); /* Forgot to call KN_UDATA_ALLOC()? */
            continue;
        }

        /*
         * The udata may have been queued for deferred free by an
         * EV_DELETE that ran while we were inside epoll_wait.  In
         * that case the back-pointer (ud_kn / ud_fds / ud_efd) is
         * dangling: the knote / fd_state / eventfd has already been
         * freed.  The udata itself is still alive (the kq_inflight
         * tracking we added to kevent_enter ensures it can't be
         * reclaimed before our matching kevent_exit) but we must
         * skip dispatch.
         */
        if (epoll_udata->ud_stale) {
            dbg_printf("[%i] udata=%p stale, skipping dispatch", i, epoll_udata);
            continue;
        }

        dbg_printf("[%i] %s", i, epoll_event_dump(ev));

        /*
         * epoll event is associated with a single filter
         * so we just have one knote per event.
         *
         * As different filters store pointers to different
         * structures, we need to examine ud_type to figure
         * out what epoll_data contains.
         */
        switch (epoll_udata->ud_type) {
        case EPOLL_UDATA_KNOTE:
        {
            struct knote *kn = epoll_udata->ud_kn;

            assert(kn);
            if (el_p >= el_end) {
            oos:
                dbg_printf("no more available kevent slots, used %zu", el_p - el);
                goto done;
            }

            rv = linux_kevent_copyout_ev(el_p, (el_end - el_p), ev, knote_get_filter(kn), kn);
            if (rv < 0) goto done;
            el_p += rv;
        }
            break;

        /*
         * epoll event is associated with one filter for
         * reading and one filter for writing.
         */
        case EPOLL_UDATA_FD_STATE:
        {
            struct fd_state   *fds = epoll_udata->ud_fds;
            struct knote      *kn, *write;
            assert(fds);

            /*
             * fds can be freed after the first linux_kevent_copyout_ev
             * so cache the pointer value here.
             */
            write = fds->fds_write;

            /*
             *    FD, or errored, or other side shutdown
             */
            if ((kn = fds->fds_read) && (ev->events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR))) {
                if (el_p >= el_end) goto oos;

                rv = linux_kevent_copyout_ev(el_p, (el_end - el_p), ev, knote_get_filter(kn), kn);
                if (rv < 0) goto done;
                el_p += rv;
            }

            /*
             *    FD is writable, or errored, or other side shutdown
             */
            if ((kn = write) && (ev->events & (EPOLLOUT | POLLHUP | EPOLLERR))) {
                if (el_p >= el_end) goto oos;

                rv = linux_kevent_copyout_ev(el_p, (el_end - el_p), ev, knote_get_filter(kn), kn);
                if (rv < 0) goto done;
                el_p += rv;
            }
        }
            break;

        case EPOLL_UDATA_EVENT_FD:
        {
            struct eventfd    *efd = epoll_udata->ud_efd;

            assert(efd);

            rv = linux_kevent_copyout_ev(el_p, (el_end - el_p), ev, efd->ef_filt, NULL);
            if (rv < 0) goto done;
            el_p += rv;
            break;
        }

        case EPOLL_UDATA_KQ_WAKE:
        {
            /*
             * Kq close-detect pipe[0] became readable.  Two
             * triggers:
             *   1. User closed kqfd (= pipefd[1]) - kernel marks
             *      pipefd[0] as "no writers", EPOLLHUP fires for
             *      every parked epoll_wait.  No bytes to drain.
             *   2. linux_kqueue_interrupt() wrote a byte to
             *      pipefd[1] from kqueue_free's defer path so a
             *      parked waiter exits and the deferred free can
             *      complete.  The byte must be drained here so
             *      linux_kqueue_free's later read doesn't see
             *      stray data.
             *
             * Either way, surface as -1/EBADF so the caller's
             * outer kevent() returns the same error native
             * kqueue produces, instead of a 0-event timeout.
             */
            char drain[16];
            ssize_t n;

            do {
                n = read(epoll_udata->ud_kq->pipefd[0], drain, sizeof(drain));
            } while (n > 0);
            /* EAGAIN/EBADF/0 all benign here. */

            dbg_printf("kq=%p - EPOLL_UDATA_KQ_WAKE, returning EBADF",
                       epoll_udata->ud_kq);
            errno = EBADF;
            return (-1);
        }

        /*
         *    Bad udata value. Maybe use after free?
         */
        default:
            assert(0);
            return (-1);
        }
    }

done:
    return el_p - el;
}

int
linux_eventfd_register(struct kqueue *kq, struct eventfd *efd)
{
    EVENTFD_UDATA_ALLOC(efd); /* setup udata for the efd */

    if (epoll_ctl(kq->epollfd, EPOLL_CTL_ADD, kqops.eventfd_descriptor(efd), EPOLL_EV_EVENTFD(EPOLLIN, efd)) < 0) {
        dbg_perror("epoll_ctl(2) - register epoll_fd=%i eventfd=%i", kq->epollfd, kqops.eventfd_descriptor(efd));
        /* Kernel never accepted the registration; free direct. */
        EVENTFD_UDATA_FREE(efd);
        return (-1);
    }

    return (0);
}

void
linux_eventfd_unregister(struct kqueue *kq, struct eventfd *efd)
{
    if (epoll_ctl(kq->epollfd, EPOLL_CTL_DEL, kqops.eventfd_descriptor(efd), NULL) < 0)
        dbg_perror("epoll_ctl(2) - unregister epoll_fd=%i eventfd=%i", kq->epollfd, kqops.eventfd_descriptor(efd));

    EVENTFD_UDATA_DEFER_FREE(kq, efd);
}

static int
linux_eventfd_init(struct eventfd *efd, struct filter *filt)
{
    int evfd;

    evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0) {
        dbg_perror("eventfd");
        return (-1);
    }
    dbg_printf("eventfd=%i - created", evfd);
    efd->ef_id = evfd;
    efd->ef_filt = filt;

    return (0);
}

static void
linux_eventfd_close(struct eventfd *efd)
{
    dbg_printf("eventfd=%i - closed", efd->ef_id);
    if (close(efd->ef_id) < 0)
        dbg_perror("close(2)");
    efd->ef_id = -1;
}

static int
linux_eventfd_raise(struct eventfd *efd)
{
    uint64_t counter;
    int rv = 0;

    dbg_printf("eventfd=%i - raising event level", efd->ef_id);
    counter = 1;
    if (write(efd->ef_id, &counter, sizeof(counter)) < 0) {
        switch (errno) {
        case EAGAIN:
            /* Not considered an error */
            break;

        case EINTR:
            rv = -EINTR;
            break;

        default:
            dbg_printf("write(2): %s", strerror(errno));
            rv = -1;
        }
    }
    return (rv);
}

static int
linux_eventfd_lower(struct eventfd *efd)
{
    uint64_t cur;
    ssize_t n;
    int rv = 0;

    /*
     * Reset the counter
     * Because we're not using EFD_SEMPAHOR the level
     * state of the eventfd is cleared.
     *
     * Thus if there were multiple calls to
     * linux_eventfd_raise, and a single call to
     * linux_eventfd_lower, the eventfd state would
     * still be lowered.
     */
    dbg_printf("eventfd=%i - lowering event level", efd->ef_id);
    n = read(efd->ef_id, &cur, sizeof(cur));
    if (n < 0) {
        switch (errno) {
        case EAGAIN:
            /* Not considered an error */
            break;

        case EINTR:
            rv = -EINTR;
            break;

        default:
            dbg_printf("read(2): %s", strerror(errno));
            rv = -1;
        }
    } else if (n != sizeof(cur)) {
        dbg_puts("short read");
        rv = -1;
    }

    return (rv);
}

static int
linux_eventfd_descriptor(struct eventfd *efd)
{
    return (efd->ef_id);
}

/** Determine what type of file descriptor the knote describes
 *
 * Sets the kn_flags field of the knote to one of:
 * - KNFL_FILE               FD is a regular file.
 * - KNFL_PIPE               FD is one end of a pipe.
 * - KNFL_BLK                FD is a block device.
 * - KNFL_CHR                FD is a character device.
 * - KNFL_SOCKET_STREAM      FD is a streaming socket(reliable connection-oriented byte streams).
 * - KNFL_SOCKET_DGRAM       FD is a datagram socket (unreliable connectionless messages).
 * - KNFL_SOCKET_RDM         FD is a reliable datagram socket (reliable connectionless messages).
 * - KNFL_SOCKET_SEQPACKET   FD is a sequenced packet socket (reliable connection-oriented messages).
 * - KNFL_SOCKET_RAW         FD is a raw socket as documented in raw(7).
 *
 * Additionally KNFL_SOCKET_* types may have the KNFL_SOCKET_PASSIVE flag set if they
 * are never expected to return data, but only provide an indication of whether data is available.
 *
 * We currently check whether the socket is a 'listening' socket (SO_ACCEPTCONN) or has a BPF rule
 * attached (SO_GET_FILTER) to determine if it's passive.
 *
 * @param[in] kn    holding the file descriptor.
 * @return
 *    - 0 on success.
 *    - -1 on failure.
 */
int
linux_get_descriptor_type(struct knote *kn)
{
    socklen_t slen;
    struct stat sb;
    int ret, lsock, stype;
    const int fd = (int)kn->kev.ident;

    /*
     * Determine the actual descriptor type.
     */
    if (fstat(fd, &sb) < 0) {
        dbg_perror("fstat(2)");
        return (-1);
    }

    switch (sb.st_mode & S_IFMT) {
        default:
            errno = EBADF;
            dbg_perror("fd=%i unknown fd type, st_mode=0x%x", fd, sb.st_mode & S_IFMT);
            return (-1);

        case S_IFREG:
            dbg_printf("fd=%i is a regular file", fd);
            kn->kn_flags |= KNFL_FILE;
            return (0);

        case S_IFIFO:
            dbg_printf("fd=%i is a pipe", fd);
            kn->kn_flags |= KNFL_PIPE;
            return (0);

        case S_IFBLK:
            dbg_printf("fd=%i is a block device", fd);
            kn->kn_flags |= KNFL_BLOCKDEV;
            return (0);

        case S_IFCHR:
            dbg_printf("fd=%i is a character device", fd);
            kn->kn_flags |= KNFL_CHARDEV;
            return (0);

        case S_IFSOCK:
            dbg_printf("fd=%i is a socket", fd);
            break; /* deferred type determination */

        case 0: /* seen with eventfd */
            dbg_printf("fd=%i fstat() provided no S_IFMT flags, treating fd as passive socket", fd);
            kn->kn_flags |= KNFL_SOCKET;
            kn->kn_flags |= KNFL_SOCKET_PASSIVE;
            return (0);
    }

    /*
     * Determine socket type.
     */
    slen = sizeof(stype);
    stype = 0;
    ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, &stype, &slen);
    if (ret < 0) {
        dbg_perror("getsockopt(3)");
        return (-1);
    }
    switch (stype) {
        case SOCK_STREAM:
            dbg_printf("fd=%i is a stream socket", fd);
            kn->kn_flags |= KNFL_SOCKET_STREAM;
            break;

        case SOCK_DGRAM:
            dbg_printf("fd=%i is a datagram socket", fd);
            kn->kn_flags |= KNFL_SOCKET_DGRAM;
            break;

        case SOCK_RDM:
            dbg_printf("fd=%i is a reliable datagram socket", fd);
            kn->kn_flags |= KNFL_SOCKET_RDM;
            break;

        case SOCK_SEQPACKET:
            dbg_printf("fd=%i is a sequenced and reliable datagram socket", fd);
            kn->kn_flags |= KNFL_SOCKET_SEQPACKET;
            break;

        case SOCK_RAW:
            dbg_printf("fd=%i is a raw socket", fd);
            kn->kn_flags |= KNFL_SOCKET_RAW;
            break;

        default:
            errno = EBADF;
            dbg_perror("unknown socket type");
            return (-1);
    }

    slen = sizeof(lsock);
    lsock = 0;
    ret = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &lsock, &slen);
    if (ret < 0) {
        switch (errno) {
            case ENOTSOCK:   /* same as lsock = 0 */
                break;

            default:
                dbg_perror("getsockopt(3)");
                return (-1);
        }
    } else if (lsock)
        kn->kn_flags |= KNFL_SOCKET_PASSIVE;

#ifdef SO_GET_FILTER
    {
        socklen_t out_len = 0;

        /*
         * Test if socket has a filter
         * pcap file descriptors need to be considered as passive sockets as
         * SIOCINQ always returns 0 even if data is available.
         * Looking at SO_GET_FILTER is a good way of doing this.
         */
        ret = getsockopt(fd, SOL_SOCKET, SO_GET_FILTER, NULL, &out_len);
        if (ret < 0) {
            switch (errno) {
                case ENOTSOCK:   /* same as lsock = 0 */
                    break;
                default:
                    dbg_perror("getsockopt(3)");
                    return (-1);
            }
        } else if (out_len)
            kn->kn_flags |= KNFL_SOCKET_PASSIVE;
    }
#endif /* SO_GET_FILTER */

    return (0);
}

/** Determine if two fd state entries are equal
 *
 * @param[in] a           first entry.
 * @param[in] b           second entry.
 * @return
 *        - 0 if fd is equal.
 *        - 1 if fd_a > fd_b
 *        - -1 if fd_a < fd_b
 */
static int
epoll_fd_state_cmp(struct fd_state *a, struct fd_state *b)
{
    return (a->fds_fd > b->fds_fd) - (a->fds_fd < b->fds_fd); /* branchless comparison */
}

/** Create type specific rbtree functions
 *
 */
RB_GENERATE(fd_st, fd_state, fds_entries, epoll_fd_state_cmp)

/** Determine current fd_state/knote associations
 *
 * @param[in,out] fds_p   to query.  If *fds_p is NULL and
 *                          kn->kn_fds is NULL, we attempt a
 *                          lookup based on the FD associated with
 *                          the kn.
 * @param[in] kn          to return EPOLLIN|EPOLLOUT flags for.
 * @param[in] disabled    if true, only disabled knotes will
 *                        be included in the set.
 *                        if false, only enabled knotes will
 *                        be included in the set.
 * @return
 *    - EPOLLIN     if the FD has a read knote associated with it.
 *    - EPOLLOUT    if the FD has a write knote associated with it.
 */
int epoll_fd_state(struct fd_state **fds_p, struct knote *kn, bool disabled)
{
    int             state = 0;
    int             fd = kn->kev.ident;
    struct fd_state *fds = *fds_p;

    if (!fds) {
        fds = kn->kn_fds;
        if(fds) dbg_printf("fd_state: from-kn fd=%i", fd);
    }
    if (!fds) {
        dbg_printf("fd_state: find fd=%i", fd);

        fds = RB_FIND(fd_st, &kn->kn_kq->kq_fd_st, &(struct fd_state){ .fds_fd = fd });
        if (!fds) return (0);
    }

    *fds_p = fds;

    state |= (fds->fds_read && (disabled == ((fds->fds_read->kev.flags & (EV_DISABLE | EV_EOF)) != 0))) * EPOLLIN;
    state |= (fds->fds_write && (disabled == ((fds->fds_write->kev.flags & (EV_DISABLE | EV_EOF)) != 0))) * EPOLLOUT;

    return state;
}

/** Associate a knote with the fd_state
 *
 * @param[in,out] fds_p   to modify knote associations for.
 *                        If *fds_p is NULL and kn->kn_fds is
 *                        NULL, we attempt a lookup based on
 *                        the FD associated with the kn.
 * @param[in] kn          to add FD tracking entry for.
 * @param[in] ev          the file descriptor was registered for,
 *                        either EPOLLIN or EPOLLOUT.
 * @return
 *    - 0 on success.
 *    - -1 on failure.
 */
int epoll_fd_state_mod(struct fd_state **fds_p, struct knote *kn, int ev)
{
    struct kqueue       *kq = kn->kn_kq;
    int                    fd = kn->kev.ident;
    struct fd_state     *fds = *fds_p;

    assert(ev & (EPOLLIN | EPOLLOUT));

    /*
     * The kqueue_lock around copyin and copyout
     * operations means we don't need mutexes
     * around tree access.
     *
     * Only one thread can be copying in or copying
     * out at a time.
     *
     * The only potential issue we have were
     * modifying the tree in kevent_wait
     * (which we're not).
     */
    if (!fds) fds = kn->kn_fds;
    if (!fds) {
        /*
         * Also used as an initialiser if we can't find
         * an existing fd_state.
         */
        struct fd_state     query = { .fds_fd = fd };

        fds = RB_FIND(fd_st, &kq->kq_fd_st, &query);
        if (!fds) {
            dbg_printf("fd_state: new fd=%i events=0x%08x (%s)", fd, ev, epoll_flags_dump(ev));

            fds = malloc(sizeof(struct fd_state));
            if (!fds) return (-1);

            *fds = query;
            FDS_UDATA_ALLOC(fds);    /* Prepare for insertion into epoll */
            RB_INSERT(fd_st, &kq->kq_fd_st, fds);

        } else {
        mod:
            dbg_printf("fd_state: mod fd=%i events=0x%08x (%s)", fd, ev, epoll_flags_dump(ev));
        }
    } else goto mod;

    /*
     * Place the knote in the correct slot.
     */
    if (ev & EPOLLIN) {
        assert(!fds->fds_read || (fds->fds_read == kn));
        fds->fds_read = kn;
    }
    if (ev & EPOLLOUT) {
        assert(!fds->fds_write || (fds->fds_write == kn));
        fds->fds_write = kn;
    }

    kn->kn_fds = fds;
    *fds_p = fds;

    return (0);
}

/** Disassociate a knote from an fd_ev possibly freeing the fd_ev
 *
 * @param[in] kn   to remove FD tracking entry for.
 * @param[in] ev   the file descriptor was de-registered for,
 *                 either EPOLLIN or EPOLLOUT.
 */
void epoll_fd_state_del(struct fd_state **fds_p, struct knote *kn, int ev)
{
    struct fd_state     *fds = kn->kn_fds;
    struct kqueue       *kq = kn->kn_kq;

    assert(ev & (EPOLLIN | EPOLLOUT));
    assert(fds); /* There ~must~ be an entry else something has gone horribly wrong */
    assert(!*fds_p || (*fds_p == kn->kn_fds));

    /*
     * copyin/copyout lock means we don't need
     * to protect operations here.
     */
    if (ev & EPOLLIN) {
        assert(fds->fds_read);
        fds->fds_read = NULL;
    }

    if (ev & EPOLLOUT) {
        assert(fds->fds_write);
        fds->fds_write = NULL;
    }

    if (!fds->fds_read && !fds->fds_write) {
        dbg_printf("fd_state: rm fd=%i", fds->fds_fd);
        RB_REMOVE(fd_st, &kq->kq_fd_st, fds);

        /*
         * Defer-free the fds_udata: a concurrent epoll_wait may have
         * already pulled an event referencing it into another
         * thread's TLS buffer.  fds itself can be freed inline since
         * nothing in the kernel or other threads holds a pointer to
         * it directly - all references are via the udata.
         */
        FDS_UDATA_DEFER_FREE(kq, fds);
        free(fds);
        *fds_p = NULL;
    } else {
        dbg_printf("fd_state: mod fd=%i events=0x%08x (%s)", fds->fds_fd, ev, epoll_flags_dump(ev));
    }
    kn->kn_fds = NULL;
}

bool
epoll_fd_registered(struct filter *filt, struct knote *kn)
{
    struct fd_state    *fds = NULL;
    int fd = kn->kev.ident;
    int have_ev;

    /*
     * The vast majority of the time if the knote
     * has already been removed then kn->kn_fds
     * will be false.
     */
    if (!kn->kn_fds) return false;        /* No file descriptor state, can't be in epoll */

    have_ev = epoll_fd_state(&fds, kn, false);            /* ...enabled only */
    if (!have_ev) return false;

    /*
     * This *SHOULD* be a noop if the FD is already
     * registered.
     */
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_MOD, fd, EPOLL_EV_FDS(have_ev, fds)) < 0) return false;

    return true;
}
int
epoll_update(int op, struct filter *filt, struct knote *kn, int ev, bool delete)
{
    struct fd_state *fds = NULL;
    int have_ev, want, want_ev;
    int opn;
    int fd;

    fd = kn->kev.ident;

#define EV_EPOLLINOUT(_x) ((_x) & (EPOLLIN | EPOLLOUT))

    if (KNOTE_DISABLED(kn)) dbg_printf("fd=%i kn=%p is disabled", fd, kn);
    if (KNOTE_IS_EOF(kn)) dbg_printf("fd=%i kn=%p is EOF", fd, kn);

    /*
     * Determine the current state of the file descriptor
     * and see if we need to make changes.
     */
    have_ev = epoll_fd_state(&fds, kn, false);            /* ...enabled only */

    dbg_printf("fd=%i have_ev=0x%04x (%s)", fd, have_ev, epoll_flags_dump(have_ev));
    switch (op) {
    case EPOLL_CTL_ADD:
        want = have_ev | ev;            /* This also preserves other option flags */
        break;

    case EPOLL_CTL_DEL:
        want = have_ev & ~ev;           /* No options for delete */

        /*
         * If we're performing a delete we need
         * to check for previously disabled
         * knotes that may now be being deleted.
         */
        if (delete) {
            int to_delete;

            to_delete = epoll_fd_state(&fds, kn, true); /* ...disabled only */
            dbg_printf("fd=%i disabled_ev=0x%04x (%s)", fd, to_delete, epoll_flags_dump(to_delete));
            to_delete &= EV_EPOLLINOUT(ev);

            if (to_delete) {
                dbg_printf("fd=%i ev=%i removing disabled fd state", fd, op);
                epoll_fd_state_del(&fds, kn, to_delete);
            }
        }
        break;

    case EPOLL_CTL_MOD:
        want = ev;                   /* We assume the caller knows what its doing... */

        if (delete) {
            int to_delete;

            to_delete = epoll_fd_state(&fds, kn, true); /* ...disabled only */
            dbg_printf("fd=%i disabled_ev=0x%04x (%s)", fd, to_delete, epoll_flags_dump(to_delete));
            to_delete &= ~ev;

            if (to_delete) {
                dbg_printf("fd=%i ev=%i removing disabled fd state", fd, op);
                epoll_fd_state_del(&fds, kn, to_delete);
            }
        }
        break;

    default:
        assert(0);
        return (-1);
    }

    /*
     * We only want the read/write flags for comparisons.
     */
    want_ev = EV_EPOLLINOUT(want);
     if (!have_ev && want_ev) {        /* There's no events registered and we want some */
        opn = EPOLL_CTL_ADD;
        epoll_fd_state_mod(&fds, kn, want_ev & ~have_ev);
    }
    else if (have_ev && !want_ev)       /* We have events registered but don't want any */
        opn = EPOLL_CTL_DEL;
    else if (have_ev != want_ev)        /* There's events but they're not what we want */
        opn = EPOLL_CTL_MOD;
    else
        return (0);

    dbg_printf("fd=%i op=0x%04x (%s) opn=0x%04x (%s) %s",
               fd,
               op, epoll_op_dump(op),
               opn, epoll_op_dump(opn),
               epoll_event_dump(EPOLL_EV_FDS(want, fds)));

    if (epoll_ctl(filter_epoll_fd(filt), opn, fd, EPOLL_EV_FDS(want, fds)) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));

        switch (opn) {
        case EPOLL_CTL_ADD:
            epoll_fd_state_del(&fds, kn, want_ev & ~have_ev);
            break;

        case EPOLL_CTL_DEL:
        case EPOLL_CTL_MOD:
            /*
             * The fd's registration in our epoll is already gone and
             * we weren't notified - not necessarily an error.  Two
             * ways this happens, both meaning "drop our fd_state":
             *   EBADF  - the fd was closed (kernel auto-removed it).
             *   ENOENT - the fd was closed AND its number reused by an
             *            unrelated open, so it's a valid fd that was
             *            never in our epoll.  Common when close
             *            detection is asynchronous: the monitoring
             *            thread reaps the kqueue long after the user
             *            closed the watched fd, by which point the
             *            number has been recycled.  Without handling
             *            this, the fd_state (and its udata) leak.
             */
            if (errno == EBADF || errno == ENOENT) {
                int kn_ev = 0;

                if (kn == fds->fds_read) {
                    kn_ev = EPOLLIN;
                } else if (kn == fds->fds_write) {
                    kn_ev = EPOLLOUT;
                }
                kn_ev &= ~want_ev;    /* If it wasn't wanted... */

                if (kn_ev) {
                    dbg_printf("clearing fd=%i fds=%p ev=%s", fd, fds, epoll_flags_dump(kn_ev));
                    epoll_fd_state_del(&fds, kn, kn_ev);
                    return (0);
                }
            }
            break;
        }

        return (-1);
    }

    /*
     * Only change fd state for del and mod on success
     * we need to 'add' before so that we get the fd_state
     * structure to pass in to epoll.
     */
    switch (opn) {
    case EPOLL_CTL_DEL:
        if (delete) {
            dbg_printf("fd=%i ev=%i removing fd state", fd, op);
            epoll_fd_state_del(&fds, kn, have_ev & ~want_ev);      /* We rely on the caller to mark the knote as disabled */
        }
        break;

    case EPOLL_CTL_MOD:
    {
        int add, del;

        add = want_ev & ~have_ev;
        del = have_ev & ~want_ev;

        if (add) epoll_fd_state_mod(&fds, kn, add);
        if (del && delete) {
            dbg_printf("fd=%i ev=%i removing fd state", fd, op);
            epoll_fd_state_del(&fds, kn, del);                 /* We rely on the caller to mark the knote as disabled */
        }
    }
        break;
    }

    return (0);
}

/*
 * Given a file descriptor, return the path to the file it refers to.
 */
int
linux_fd_to_path(char *buf, size_t bufsz, int fd)
{
    char path[51];    // 6 + 20 + 4 + 20 + 1

    if (snprintf(path, sizeof(path), "/proc/%d/fd/%d", getpid(), fd) < 0)
        return (-1);

    memset(buf, 0, bufsz);
    return (readlink(path, buf, bufsz));
}

const struct kqueue_vtable kqops = {
    .libkqueue_fork     = linux_libkqueue_fork,
    .libkqueue_free     = linux_libkqueue_free,
    .kqueue_init        = linux_kqueue_init,
    .kqueue_free        = linux_kqueue_free,
    .flags              = KQUEUE_FLAG_CLOSE_ASYNC,   /* monitoring thread frees on close; eviction must not double-free */
    .kqueue_interrupt   = linux_kqueue_interrupt,
    .kevent_wait        = linux_kevent_wait,
    .kevent_copyout     = linux_kevent_copyout,
    .eventfd_register   = linux_eventfd_register,
    .eventfd_unregister = linux_eventfd_unregister,
    .eventfd_init       = linux_eventfd_init,
    .eventfd_close      = linux_eventfd_close,
    .eventfd_raise      = linux_eventfd_raise,
    .eventfd_lower      = linux_eventfd_lower,
    .eventfd_descriptor = linux_eventfd_descriptor,
};
