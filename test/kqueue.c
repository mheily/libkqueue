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

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/mman.h>

#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/resource.h>
#endif

/*
 * libkqueue-internal sync helper exposed for tests; see
 * src/linux/platform.c for the definition and rationale.  Lives
 * only in the Linux backend.  CMake passes
 * LIBKQUEUE_BACKEND_<NAME>=1 into the test binary; default-Linux
 * (no override) implies the linux backend, so we accept the
 * symbol there too.
 */
#if defined(LIBKQUEUE_BACKEND_LINUX) || \
    (defined(__linux__) && !defined(LIBKQUEUE_BACKEND_POSIX))
# define HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE 1
void libkqueue_drain_pending_close(void);
#endif

/*
 * Test the method for detecting when one end of a socketpair
 * has been closed. This technique is used in kqueue_validate()
 */
static void
test_peer_close_detection(void *unused)
{
#ifdef _WIN32
    return;
    //FIXME
#else
    int sockfd[2];
    char buf[1];
    struct pollfd pfd;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0)
        die("socketpair");

    pfd.fd = sockfd[0];
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;

    if (poll(&pfd, 1, 0) > 0)
        die("unexpected data");

    if (close(sockfd[1]) < 0)
        die("close");

    if (poll(&pfd, 1, 0) > 0) {
        if (recv(sockfd[0], buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT) != 0)
            die("failed to detect peer shutdown");
    }
    close(sockfd[0]);
#endif
}

void
test_kqueue_alloc(void *unused)
{
    int kqfd;

    if ((kqfd = kqueue()) < 0)
        die("kqueue()");
    test_no_kevents(kqfd);
    if (close(kqfd) < 0)
        die("close()");
}

void
test_kevent(void *unused)
{
    struct kevent kev;

    memset(&kev, 0, sizeof(kev));

    /* Provide an invalid kqueue descriptor */
    if (kevent(-1, &kev, 1, NULL, 0, NULL) == 0)
        die("invalid kq parameter");
}

#if defined(__linux__)
/* Maximum number of FD for current process */
#define MAX_FDS 32
/*
 * Test the cleanup process for Linux
 */
void
test_cleanup(void *unused)
{
    int i;
    int max_fds = MAX_FDS;
    struct rlimit curr_rlim, rlim;
    int kqfd1, kqfd2;
    struct kevent kev;

    /* Remeber current FD limit */
    if (getrlimit(RLIMIT_NOFILE, &curr_rlim) < 0) {
        die("getrlimit failed");
    }

    /* lower FD limit to 32 */
    if (max_fds < curr_rlim.rlim_cur) {
        /* Set FD limit to MAX_FDS */
        rlim = curr_rlim;
        rlim.rlim_cur = 32;
        if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
            die("setrlimit failed");
        }
    } else {
        max_fds = curr_rlim.rlim_cur;
    }

    /* Create initial kqueue to avoid cleanup thread being destroyed on each close */
    if ((kqfd1 = kqueue()) < 0)
        die("kqueue() - used_fds=%u max_fds=%u", print_fd_table(), max_fds);

    /* Create and close 2 * max fd number of kqueues */
    for (i=0; i < 2 * max_fds + 1; i++) {
        if ((kqfd2 = kqueue()) < 0)
            die("kqueue() - i=%i used_fds=%u max_fds=%u", i, print_fd_table(), max_fds);

        kevent_add(kqfd2, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

        if (close(kqfd2) < 0)
            die("close()");

        nanosleep(&(struct timespec) { .tv_nsec = 25000000 }, NULL);   /* deschedule thread */
    }

    if (close(kqfd1) < 0)
        die("close()");

    /*
     * Run same test again but without extra kqueue
     * Cleanup thread will be destroyed
     * Create and close 2 * max fd number of kqueues
     */
    for (i=0; i < 2 * max_fds + 1; i++) {
        if ((kqfd2 = kqueue()) < 0)
            die("kqueue()");

        kevent_add(kqfd2, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

        if (close(kqfd2) < 0)
            die("close()");

        nanosleep(&(struct timespec) { .tv_nsec = 25000000 }, NULL);   /* deschedule thread */
    }

    /* Restore FD limit */
    if (setrlimit(RLIMIT_NOFILE, &curr_rlim) < 0) {
        die("setrlimit failed");
    }
}

void
test_fork(void *unused)
{
    int kqfd;
    pid_t pid;

    kqfd = kqueue();
    if (kqfd < 0)
        die("kqueue()");

    pid = fork();
    if (pid == 0) {
        /*
         * fork should immediately close all open
         * kqueues and their file descriptors.
         */
        if (close(kqfd) != -1)
           die("kqueue fd still open in child");

        testing_end_quiet();
        exit(0);
    } else if (pid == -1)
        die("fork()");

    close(kqfd);
}
#endif

/* EV_RECEIPT is not available or running on Win32 */
#if !defined(_WIN32)
void
test_ev_receipt(struct test_context *ctx)
{
    struct kevent kev;
    struct kevent buf;

    EV_SET(&kev, SIGUSR2, EVFILT_SIGNAL, EV_ADD | EV_ERROR | EV_RECEIPT, 0, 0, NULL);
    buf = kev;

    kevent_rv_cmp(1, kevent(ctx->kqfd, &kev, 1, &buf, 1, NULL));

#if defined(__FreeBSD__)
    kev.flags &= ~EV_RECEIPT;           /* FreeBSD drops EV_RECEIPT but keeps EV_ADD */
#elif defined(__OpenBSD__) || defined(__NetBSD__)
    kev.flags &= ~(EV_ADD | EV_RECEIPT); /* OpenBSD/NetBSD keep only EV_ERROR */
#endif

    kevent_cmp(&kev, &buf);

    /*
     * Clean up SIGUSR2 so subsequent tests (notably the EVFILT_SIGNAL
     * suite) get a fresh knote.  Without this, macOS/FreeBSD persist
     * EV_RECEIPT on the SIGUSR2 knote and any later test that uses
     * SIGUSR2 sees it leaked into returned events.
     */
    EV_SET(&kev, SIGUSR2, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
}
#endif

/* Maximum number of threads that can be created */
#define MAX_THREADS 100

/**
 * Don't build the below function on OSX due to a known-issue
 * breaking the build with FD_SET()/FD_ISSET() + OSX.
 * e.g: https://www.google.com/search?q=___darwin_check_fd_set_overflow
 */
#ifndef __APPLE__
void
test_kqueue_descriptor_is_pollable(void *unused)
{
    int kq, rv;
    struct kevent kev;
    fd_set fds;
    struct timeval tv;

    if ((kq = kqueue()) < 0)
        die("kqueue()");

    test_no_kevents(kq);
    kevent_add(kq, &kev, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1000, NULL);
    test_no_kevents(kq);

    FD_ZERO(&fds);
    FD_SET(kq, &fds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    rv = select(1, &fds, NULL, NULL, &tv);
    if (rv < 0)
        die("select() error");
    if (rv == 0)
        die("select() no events");
    if (!FD_ISSET(kq, &fds)) {
        die("descriptor is not ready for reading");
    }

    close(kq);
}
#endif

#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
/*
 * close(kqfd) with active knotes must reclaim every kqueue-side
 * allocation, even without explicit EV_DELETEs.
 *
 * Linux libkqueue's close-cleanup is asynchronous: the kernel
 * signals the monitoring thread when the close-detect pipe sees
 * its write end go away, and the monitoring thread runs
 * kqueue_free.  A test that closes and exits immediately can race
 * past LSAN's leak check before the cleanup signal lands, surfacing
 * spurious "leaks" on every iteration.
 *
 * libkqueue_drain_pending_close() is the synchronous escape hatch:
 * it walks kq_list under kq_mtx and forcibly frees any kq whose
 * user-facing fd is no longer valid.  This test exercises a
 * representative selection of filters - one that uses kn_udata
 * directly (EVFILT_TIMER), one that exercises the shared fd_state
 * machinery (EVFILT_READ on a pipe), and EVFILT_USER for the
 * eventfd path - then closes the kq without any EV_DELETEs and
 * drains.  LSAN at process exit catches any allocation that
 * escaped the close+drain path.
 *
 * Only relevant for backends that ship libkqueue_drain_pending_close
 * (currently the Linux backend's monitoring thread).
 */
static void
test_close_cleans_up_active_knotes(void *unused)
{
    struct kevent kev;
    int           kqfd;
    int           pipefd[2];

    (void) unused;

    if (pipe(pipefd) < 0) die("pipe");

    kqfd = kqueue();
    if (kqfd < 0) die("kqueue");

    EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (user add)");

    EV_SET(&kev, 2, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (timer add)");

    EV_SET(&kev, pipefd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent (read add)");

    /* Close without any EV_DELETE. */
    if (close(kqfd) < 0) die("close kqfd");

    /* Force the monitoring thread's cleanup path to run synchronously
     * before any new fds get allocated that could collide with the
     * just-closed kq_id. */
    libkqueue_drain_pending_close();

    if (close(pipefd[0]) < 0) die("close pipe[0]");
    if (close(pipefd[1]) < 0) die("close pipe[1]");
}
#endif

/*
 * Recursive kqueue: kq1 watches kq2's fd via EVFILT_READ; trigger
 * an event on kq2 and verify kq1 sees it.  Tests the kqueue's own
 * f_kqfilter / fo_kqfilter glue and lock-ordering when kqueues
 * nest.  Native BSD limits depth (XNU MAX_NESTED_KQ=10); this test
 * pins single-level recursion.
 */
static void
test_kqueue_recursive(void *unused)
{
    struct kevent kev, ret[1];
    int           kq1, kq2;
    struct timespec poll = { 1, 0 };

    (void) unused;
    if ((kq1 = kqueue()) < 0) die("kqueue 1");
    if ((kq2 = kqueue()) < 0) die("kqueue 2");

    /* Watch kq2's fd from kq1. */
    EV_SET(&kev, kq2, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq1, &kev, 1, NULL, 0, NULL) < 0)
        die("recursive kq: kq1 EV_ADD on kq2 fd");

    /* Fire an EVFILT_USER event on kq2 so kq2 becomes "readable". */
    EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kq2, &kev, 1, NULL, 0, NULL) < 0) die("kq2 user add");
    EV_SET(&kev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    if (kevent(kq2, &kev, 1, NULL, 0, NULL) < 0) die("kq2 trigger");

    /* kq1 should report kq2 as readable. */
    if (kevent(kq1, NULL, 0, ret, 1, &poll) != 1)
        die("recursive kq: kq1 didn't see kq2 readable");
    if (ret[0].ident != (uintptr_t) kq2 || ret[0].filter != EVFILT_READ)
        die("recursive kq: wrong event %s", kevent_to_str(&ret[0]));

    /* Close in adverse order: kq2 (the watched one) first. */
    close(kq2);
    close(kq1);
}

/*
 * kevent() with negative or absurdly large nevents must not
 * crash or copyout past the eventlist's valid range.  FreeBSD
 * D30480 added explicit EINVAL rejection; other platforms
 * silently treat negative as 0.  The portable safety contract
 * is simpler: no buffer overrun.
 *
 * Verified by sandwiching a single-kevent eventlist between
 * PROT_NONE guard pages.  Any write past the buffer (in either
 * direction) traps to SIGSEGV and the test process dies hard.
 */
static void
test_kqueue_nevents_validation(void *unused)
{
    int           kq;
    long          page = sysconf(_SC_PAGESIZE);
    char         *region;
    struct kevent *evlist;

    (void) unused;
    if ((kq = kqueue()) < 0) die("kqueue");

    /*
     * 3-page region: [PROT_NONE | RW | PROT_NONE].  Place the
     * single-element eventlist at the END of the middle page so
     * the byte immediately after kevent[0] is PROT_NONE.  Map
     * RW first then mprotect the outer pages DOWN to PROT_NONE -
     * NetBSD (and some PaX-hardened kernels) refuse to elevate
     * permissions via mprotect from PROT_NONE.
     */
    region = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) die("mmap");
    if (mprotect(region, page, PROT_NONE) < 0)
        die("mprotect(guard 0)");
    if (mprotect(region + 2 * page, page, PROT_NONE) < 0)
        die("mprotect(guard 2)");
    evlist = (struct kevent *)
             (region + 2 * page - sizeof(struct kevent));
    /* Sanity: dereferenceable. */
    memset(evlist, 0, sizeof(*evlist));

    /*
     * Negative nevents.  Whether the kernel rejects or treats
     * as 0 is implementation-defined.  What's universal: it
     * MUST NOT write through evlist or anywhere it shouldn't.
     */
    {
        int rv = kevent(kq, NULL, 0, evlist, -1, NULL);
        if (rv > 0)
            die("kevent(nevents=-1) returned events, must not");
#if !defined(NATIVE_KQUEUE)
        /*
         * libkqueue (POSIX/Linux) rejects with EINVAL.  Native
         * BSDs don't validate; skip the errno assertion there
         * and rely on the guard pages above to catch unsafe
         * behaviour.
         */
        if (rv != -1 || errno != EINVAL)
            die("kevent(nevents=-1) expected EINVAL (errno=%d)", errno);
#endif
    }

#if !defined(NATIVE_KQUEUE)
    /*
     * INT_MAX nevents: NetBSD hangs in the eventlist iteration
     * loop here; FreeBSD/OpenBSD/macOS clamp safely.  Gate to
     * libkqueue where MAX_KEVENT capping is enforced explicitly.
     * Guard pages above remain in force for the entire call so
     * any kernel-side overrun on the surviving backends still
     * traps to SIGSEGV.
     */
    {
        struct timespec poll = { 0, 1000000 };  /* 1ms */
        int rv = kevent(kq, NULL, 0, evlist, INT_MAX, &poll);
        if (rv == -1 && errno != EINVAL && errno != ENOMEM)
            die("kevent(nevents=INT_MAX) errno=%d (%s)",
                errno, strerror(errno));
    }
#endif

    munmap(region, 3 * page);
    close(kq);
}

/*
 * fd reuse: register on a pipe fd, close it, open a fresh socket
 * that ends up at the same fd number, register on the socket, and
 * verify no events from the old knote leak into the new one.
 *
 * Native BSD's knote_fdclose() is the kernel side; libkqueue must
 * not reference a freed knote when the same fd number is reused.
 */
static void
test_kqueue_fd_reuse_no_stale_events(void *unused)
{
    struct kevent kev, ret[1];
    int           kq;
    int           pfd[2];
    int           reused;
    struct timespec poll = { 0, 100 * 1000 * 1000 };  /* 100ms */

    (void) unused;
    if ((kq = kqueue()) < 0) die("kqueue");
    if (pipe(pfd) < 0) die("pipe");

    /* Register on the read end. */
    EV_SET(&kev, pfd[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) die("EV_ADD on pipe");

    /* Close the watched fd; close write end too so any kernel-side
     * residual events would have something to fire on. */
    close(pfd[0]);
    close(pfd[1]);

    /* Try to claim the same fd number for a new socket.  May or may
     * not succeed depending on kernel fd allocation; only run the
     * stale-event check if we actually got the same number. */
    reused = socket(AF_INET, SOCK_DGRAM, 0);
    if (reused < 0) die("socket");

    if (reused == pfd[0]) {
        /*
         * Same fd number recycled.  The old knote (if libkqueue
         * doesn't clean up on close) would now think this socket
         * is its watch target.  Drain - no event must surface.
         */
        if (kevent(kq, NULL, 0, ret, 1, &poll) != 0)
            die("stale knote fired on reused fd: %s",
                kevent_to_str(&ret[0]));
    }

    close(reused);
    close(kq);
}

/*
 * Recursive kqueue chain depth-N: register kq1 watching kq2's fd,
 * kq2 watching kq3's, ..., kq[N-1] watching kqN's.  Trigger via
 * leaf and let the wakeup propagate back up.  XNU caps at
 * MAX_NESTED_KQ=10 (returns ELOOP); FreeBSD/OpenBSD/NetBSD don't
 * cap and may stack-overflow on the recursive wake.
 *
 * If the kernel survives, register fails partway with ELOOP/EINVAL
 * and we clean up; if it doesn't, the QEMU VM dies and CI marks
 * failure - that's the point, file upstream.
 *
 * Depth tuned conservatively (100) for first run.  Increase if
 * no kernels panic.
 */
#ifdef NATIVE_KQUEUE
static void
test_kqueue_deep_recursive_chain(void *unused)
{
    enum { CHAIN_DEPTH = 100 };
    int           kqs[CHAIN_DEPTH];
    struct kevent kev;
    int           i, depth;

    (void) unused;

    for (depth = 0; depth < CHAIN_DEPTH; depth++) {
        kqs[depth] = kqueue();
        if (kqs[depth] < 0) {
            /* Out of fds or similar; tear down. */
            break;
        }
    }

    /* Chain: kq[0] watches kq[1], kq[1] watches kq[2], ... */
    for (i = 0; i + 1 < depth; i++) {
        EV_SET(&kev, kqs[i + 1], EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(kqs[i], &kev, 1, NULL, 0, NULL) < 0) {
            /*
             * ELOOP / EINVAL from a depth cap is the safe
             * outcome; bail without panicking.
             */
            depth = i + 1;
            break;
        }
    }

    /* Tear down in reverse so closes propagate down the chain. */
    for (i = depth - 1; i >= 0; i--) close(kqs[i]);
}
#endif

/*
 * Knote-pool exhaustion: register many EVFILT_USER knotes on one
 * kqueue and verify the kernel returns ENOMEM/EMFILE rather than
 * panicking on allocation failure.  Count kept moderate (8192) so
 * the bulk-close path completes before fd-pressure tests run.
 */
static void
test_kqueue_knote_pool_exhaustion(void *unused)
{
    enum { N = 8192 };
    int  kq;
    int  i, registered = 0;

    (void) unused;
    if ((kq = kqueue()) < 0) die("kqueue");

    for (i = 0; i < N; i++) {
        struct kevent kev;
        EV_SET(&kev, i + 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
            /* ENOMEM / EAGAIN / EMFILE all acceptable. */
            break;
        }
        registered++;
    }
    close(kq);
#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
    /* Linux backend cleans up asynchronously; drain so subsequent
     * fd-pressure tests don't see lingering allocations. */
    libkqueue_drain_pending_close();
#endif
    (void) registered;
}

/*
 * Pipe peer-close detach race: tight-loop registering EVFILT_WRITE
 * on a pipe write end and closing both ends in a sibling thread.
 * filt_pipedetach walking pipe->pipe_peer while pipeclose nulls it
 * is a UAF candidate per the FreeBSD/NetBSD audits.  No deterministic
 * synchronisation - racing as fast as possible.
 */
struct pipe_uaf_args { atomic_int stop; };

static void *
_pipe_uaf_worker(void *arg)
{
    struct pipe_uaf_args *a = arg;
    while (!atomic_load(&a->stop)) {
        int p[2];
        if (pipe(p) < 0) continue;
        int kq = kqueue();
        if (kq < 0) { close(p[0]); close(p[1]); continue; }
        struct kevent kev;
        EV_SET(&kev, p[1], EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        (void) kevent(kq, &kev, 1, NULL, 0, NULL);
        /* Close in adverse order: read end first, then write. */
        close(p[0]);
        close(p[1]);
        close(kq);
    }
    return NULL;
}

static void
test_kqueue_pipe_peer_close_uaf(void *unused)
{
    enum { N_THREADS = 4, DURATION_MS = 500 };
    pthread_t              th[N_THREADS];
    struct pipe_uaf_args   args;
    int                    i;
    struct timespec        deadline, now;

    (void) unused;
    atomic_init(&args.stop, 0);

    for (i = 0; i < N_THREADS; i++) {
        if (pthread_create(&th[i], NULL, _pipe_uaf_worker, &args) != 0)
            die("pthread_create");
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += DURATION_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    do {
        struct timespec sleep = { 0, 50 * 1000000L };
        nanosleep(&sleep, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec &&
              now.tv_nsec < deadline.tv_nsec));

    atomic_store(&args.stop, 1);
    for (i = 0; i < N_THREADS; i++) pthread_join(th[i], NULL);
#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
    libkqueue_drain_pending_close();
#endif
}

/*
 * EVFILT_TIMER callout-vs-detach race: spawn many short periodic
 * timers, then tear them all down concurrently from sibling threads.
 * filt_timerdetach barriering against an in-flight callout has had
 * historical races (the audits flagged this for OpenBSD/NetBSD/
 * FreeBSD).  Stresses the barrier path.
 */
struct timer_race_args { int kqfd; atomic_int stop; };

static void *
_timer_race_worker(void *arg)
{
    struct timer_race_args *a = arg;
    while (!atomic_load(&a->stop)) {
        struct kevent kev;
        uintptr_t ident = (uintptr_t)(rand() % 32 + 1);
        /* Sub-ms periodic so the callout fires repeatedly. */
        EV_SET(&kev, ident, EVFILT_TIMER, EV_ADD,
               0, 1, NULL);
        if (kevent(a->kqfd, &kev, 1, NULL, 0, NULL) == 0) {
            EV_SET(&kev, ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
            (void) kevent(a->kqfd, &kev, 1, NULL, 0, NULL);
        }
    }
    return NULL;
}

static void
test_kqueue_timer_callout_detach_race(void *unused)
{
    enum { N_THREADS = 4, DURATION_MS = 500 };
    pthread_t                th[N_THREADS];
    struct timer_race_args   args;
    int                      kq;
    int                      i;
    struct timespec          deadline, now;

    (void) unused;
    if ((kq = kqueue()) < 0) die("kqueue");
    args.kqfd = kq;
    atomic_init(&args.stop, 0);

    for (i = 0; i < N_THREADS; i++) {
        if (pthread_create(&th[i], NULL, _timer_race_worker, &args) != 0)
            die("pthread_create");
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += DURATION_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    do {
        struct timespec sleep = { 0, 50 * 1000000L };
        nanosleep(&sleep, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec &&
              now.tv_nsec < deadline.tv_nsec));

    atomic_store(&args.stop, 1);
    for (i = 0; i < N_THREADS; i++) pthread_join(th[i], NULL);
    close(kq);
#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
    libkqueue_drain_pending_close();
#endif
}

void
test_kqueue(struct test_context *ctx)
{
    test(peer_close_detection, ctx);

    test(kqueue_alloc, ctx);
    test(kevent, ctx);
    /*
     * Recursive kqueue: requires the kqueue's own fd to be pollable
     * by EVFILT_READ on another kqueue.  Native BSD/macOS support
     * this; libkqueue's POSIX/Linux backends don't yet expose the
     * kqfd as a generic readable fd.  Gate to native until the
     * backends grow kq->kq_id pollability.
     */
#ifdef NATIVE_KQUEUE
    test(kqueue_recursive, ctx);
#endif
    /*
     * NetBSD's kqueue_scan loop hangs the calling thread for
     * minutes on nevents=-1 (likely casts int to size_t for the
     * eventlist iteration; -1 -> SIZE_MAX iterations).  Real
     * upstream DoS-class kernel bug.  Skip on NetBSD until
     * filed/fixed; guard pages still validate the safety
     * contract everywhere else.
     */
#if !defined(__NetBSD__)
    test(kqueue_nevents_validation, ctx);
#endif
    test(kqueue_fd_reuse_no_stale_events, ctx);

#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
    test(cleanup, ctx);
    test(close_cleans_up_active_knotes, ctx);

    /*
     * Only run the fork test if we can do TSAN
     * suppressions, as there's false positives
     * generated by libkqueue_pre_fork.
     */
#  ifdef HAVE_TSAN_IGNORE
    test(fork, ctx);
#  endif
#endif

#if !defined(_WIN32)
    test(ev_receipt, ctx);
#endif

    /*
     * Kernel-DoS / panic probes.  Surface real upstream bugs.
     * Run last so any leaked fds don't poison test_cleanup
     * (which lowers RLIMIT_NOFILE and is fragile to residue).
     */
#ifdef NATIVE_KQUEUE
    test(kqueue_deep_recursive_chain, ctx);
#endif
    test(kqueue_knote_pool_exhaustion, ctx);
    test(kqueue_pipe_peer_close_uaf, ctx);
    test(kqueue_timer_callout_detach_race, ctx);
    /* TODO: this fails now, but would be good later
    test(kqueue_descriptor_is_pollable, ctx);
    */
}


