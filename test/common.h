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

#ifndef _COMMON_H
#define _COMMON_H

/*
 * Canonical common test set.
 *
 * Every EVFILT_* test suite should provide tests covering the
 * filter-agnostic kqueue contract.  Adding a new filter?  Start
 * here, then add filter-specific NOTE_* / data semantics.
 *
 * 1.  add                       Basic EV_ADD registration succeeds.
 * 2.  del                       EV_DELETE removes the knote, no
 *                               further deliveries.
 * 3.  del_nonexistent           EV_DELETE on a never-registered
 *                               ident returns ENOENT.
 * 4.  disable_and_enable        EV_DISABLE suppresses delivery,
 *                               EV_ENABLE restores.
 * 5.  oneshot                   EV_ONESHOT fires once then auto-
 *                               deletes (subsequent EV_DELETE on
 *                               the same ident returns ENOENT).
 * 6.  dispatch                  EV_DISPATCH fires once then auto-
 *                               disables; EV_ENABLE re-arms.
 * 7.  receipt_preserved         EV_RECEIPT echoes the kev in the
 *                               eventlist with EV_ERROR=0.
 * 8.  udata_preserved           udata set on EV_ADD round-trips
 *                               through delivery unchanged.
 * 9.  modify_clobbers_udata     Re-EV_ADD modify replaces udata
 *                               (BSD overwrites on every modify).
 * 10. modify_replaces_fflags    Re-EV_ADD with new fflags replaces,
 *                               not ORs (n/a where filter has no
 *                               fflags - document and skip).
 * 11. disable_drains            EV_DISABLE drops a pending event
 *                               queued before the disable.
 * 12. delete_drains             EV_DELETE drops a pending event.
 * 13. multi_kqueue              Same ident in two kqueues - state
 *                               is independent.
 * 14. ev_clear (filter-dependent) Edge-trigger semantics: data /
 *                               accumulator zeroes after delivery
 *                               so a second drain returns nothing
 *                               unless a fresh event happened.
 *
 * Filters whose underlying source can fire events while the knote
 * is disabled (EVFILT_PROC: child can exit; EVFILT_SIGNAL: signal
 * can be sent) should also have:
 *
 *  +. disable_preserves_events  Underlying event happens during
 *                               disable; EV_ENABLE must surface it
 *                               on the next drain.
 *
 * Where a backend genuinely can't pass a test (architectural
 * limit, e.g. POSIX stat-snapshot polling can't detect mtime-only
 * writes), gate it with `#if !defined(LIBKQUEUE_BACKEND_X)` and a
 * comment citing the kernel/syscall that's missing.  Don't gate
 * to silence flakes - investigate the cause.
 */

#if HAVE_ERR_H
# include <err.h>
#else
# define err(rc, fmt, ...) do { \
    char buf[128]; \
    snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);\
    perror(buf); \
    exit(rc); \
} while (0)

# define errx(rc, msg, ...) do { \
    fprintf(stderr, msg"\n", ##__VA_ARGS__);\
    exit(rc); \
} while (0)
#endif

#define die(fmt, ...)   do { \
    fprintf(stderr, "%s(): "fmt": %s (%i)\n", __func__, ##__VA_ARGS__, strerror(errno), errno);\
    abort();\
} while (0)

/* Best-effort kill: ESRCH is fine (target already gone), other errors die. */
#define kill_or_die(_pid, _sig) do { \
    if (kill((_pid), (_sig)) < 0 && errno != ESRCH) \
        die("kill(%d, %d)", (int)(_pid), (_sig)); \
} while (0)

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#  include "config.h"
#endif

#if defined(__FreeBSD__)
#  include <netinet/in.h>
#endif

/*
 * NATIVE_KQUEUE is set when the test binary is linked against the
 * host kernel's kqueue rather than libkqueue.  It's the gate for
 * tests that exercise behaviour libkqueue's POSIX backend doesn't
 * implement (e.g. close-wake delivery).  When LIBKQUEUE_BACKEND_POSIX
 * is defined we're testing libkqueue's POSIX backend on the host,
 * not the host's native kqueue, so leave NATIVE_KQUEUE unset.
 */
#if (defined(__APPLE__) || defined(__FreeBSD__) || \
     defined(__OpenBSD__) || defined(__NetBSD__)) && \
    !defined(LIBKQUEUE_BACKEND_POSIX)
#define NATIVE_KQUEUE 1
#endif

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <netdb.h>
#else
#  include "include/sys/event.h"
#  include "src/windows/platform.h"
#  include "win32_compat.h"

/*
 * POSIX-style constants the test suite uses that aren't shipped
 * by the Win32 SDK.  Pick the closest analogue rather than
 * #ifdef-ing every callsite.
 */
#  ifndef MSG_DONTWAIT
/*
 * Win32 recv() takes flags but doesn't define MSG_DONTWAIT; the
 * test suite uses it as a "don't block during cleanup drain" hint.
 * Mapping it to 0 falls back to blocking recv, which is fine here
 * because the drains run on sockets that already have pending data.
 */
#    define MSG_DONTWAIT 0
#  endif
#  ifndef MSG_NOSIGNAL
/* Win32 sockets don't raise SIGPIPE; the flag is a no-op there. */
#    define MSG_NOSIGNAL 0
#  endif
#  ifndef SHUT_RDWR
#    define SHUT_RDWR SD_BOTH
#  endif
#  ifndef SHUT_RD
#    define SHUT_RD SD_RECEIVE
#  endif
#  ifndef SHUT_WR
#    define SHUT_WR SD_SEND
#  endif
#  ifndef S_IRWXU
#    define S_IRWXU (_S_IREAD | _S_IWRITE)
#  endif

/* usleep() shim: Win32 Sleep() is millisecond-granular. */
#  define usleep(_us) Sleep((DWORD)((_us) / 1000))
#endif

/*
 * close() on a SOCKET via the Win32 CRT _close() asserts in
 * Debug builds (the SOCKET isn't a CRT fd); use closesocket()
 * there.  POSIX fds (open/_open/_pipe) keep using close().
 */
#ifdef _WIN32
#  define closesock(s) closesocket((SOCKET)(s))
#else
#  define closesock(s) close(s)
#endif

#include "config.h"

/** Convenience macros
 *
 */
#define NUM_ELEMENTS(_t) (sizeof((_t)) / sizeof(*(_t)))

struct test_context;

struct unit_test {
    const char   *ut_name;
    int          ut_enabled;
    void         (*ut_func)(struct test_context *);
    int          ut_start;
    int          ut_end;
    unsigned int ut_num;
};

/*
 * Per-test-case metadata and runtime gate evaluation.
 *
 * Each EVFILT_* suite exposes a null-terminated lkq_test_case array.
 * run_test_suite() iterates it, evaluates gates against lkq_current_platform,
 * and either runs or reports SKIP.  --list-gated iterates the same arrays
 * without executing anything.
 *
 * lkq_current_platform is a bitmask of OS + backend bits computed in main()
 * from compile-time macros.  Gate entries match when any of their platform
 * bits overlap with the current platform bitmask.
 */
typedef unsigned int lkq_platform_t;

/* OS bits */
#define LKQ_PLATFORM_OS_LINUX     (1u <<  0)
#define LKQ_PLATFORM_OS_FREEBSD   (1u <<  1)
#define LKQ_PLATFORM_OS_NETBSD    (1u <<  2)
#define LKQ_PLATFORM_OS_OPENBSD   (1u <<  3)
#define LKQ_PLATFORM_OS_MACOS     (1u <<  4)
#define LKQ_PLATFORM_OS_SOLARIS   (1u <<  5)
#define LKQ_PLATFORM_OS_ANDROID   (1u <<  6)
#define LKQ_PLATFORM_OS_WINDOWS   (1u <<  7)

/* Backend bits - orthogonal to OS */
#define LKQ_PLATFORM_BACKEND_NATIVE   (1u <<  8)  /* host's own kqueue(2) */
#define LKQ_PLATFORM_BACKEND_POSIX    (1u <<  9)  /* libkqueue POSIX backend */
#define LKQ_PLATFORM_BACKEND_LINUX    (1u << 10)  /* libkqueue Linux backend */
#define LKQ_PLATFORM_BACKEND_WINDOWS  (1u << 11)  /* libkqueue Windows backend */
#define LKQ_PLATFORM_BACKEND_SOLARIS  (1u << 12)  /* libkqueue Solaris backend */

/* All backend bits combined */
#define LKQ_PLATFORM_BACKEND_ANY \
    (LKQ_PLATFORM_BACKEND_NATIVE | LKQ_PLATFORM_BACKEND_POSIX  | \
     LKQ_PLATFORM_BACKEND_LINUX  | LKQ_PLATFORM_BACKEND_WINDOWS | \
     LKQ_PLATFORM_BACKEND_SOLARIS)

/*
 * NOT-backend helpers: gate fires on every backend EXCEPT the named one.
 * Use when a test only passes on one specific backend.
 */
#define LKQ_PLATFORM_NOT_BACKEND_NATIVE \
    (LKQ_PLATFORM_BACKEND_ANY & ~LKQ_PLATFORM_BACKEND_NATIVE)
#define LKQ_PLATFORM_NOT_BACKEND_POSIX \
    (LKQ_PLATFORM_BACKEND_ANY & ~LKQ_PLATFORM_BACKEND_POSIX)
#define LKQ_PLATFORM_NOT_BACKEND_LINUX \
    (LKQ_PLATFORM_BACKEND_ANY & ~LKQ_PLATFORM_BACKEND_LINUX)

/*
 * Native kqueue on non-FreeBSD platforms (macOS, OpenBSD, NetBSD).
 * Used when a gap affects those but not FreeBSD's native kqueue.
 */
#define LKQ_PLATFORM_NATIVE_NOT_FREEBSD \
    (LKQ_PLATFORM_OS_MACOS | LKQ_PLATFORM_OS_NETBSD | LKQ_PLATFORM_OS_OPENBSD)

/*
 * Compile-time bitmask for the platform this binary targets - the same
 * OS|backend bits init_platform() assigns to lkq_current_platform, but
 * usable inside static initializers.  A test whose body is compiled out
 * because a feature macro or capability is missing self-gates with
 * GATE(LKQ_BUILD_PLATFORM, ...) so the entry stays present and the gate
 * matrix is complete on every platform.
 */
#if defined(NATIVE_KQUEUE)
#  define LKQ_BUILD_BACKEND  LKQ_PLATFORM_BACKEND_NATIVE
#elif defined(LIBKQUEUE_BACKEND_POSIX)
#  define LKQ_BUILD_BACKEND  LKQ_PLATFORM_BACKEND_POSIX
#elif defined(LIBKQUEUE_BACKEND_LINUX)
#  define LKQ_BUILD_BACKEND  LKQ_PLATFORM_BACKEND_LINUX
#elif defined(_WIN32)
#  define LKQ_BUILD_BACKEND  LKQ_PLATFORM_BACKEND_WINDOWS
#elif defined(__sun)
#  define LKQ_BUILD_BACKEND  LKQ_PLATFORM_BACKEND_SOLARIS
#else
#  define LKQ_BUILD_BACKEND  0
#endif

#if defined(__linux__) && defined(__ANDROID__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_ANDROID
#elif defined(__linux__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_LINUX
#elif defined(__FreeBSD__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_FREEBSD
#elif defined(__NetBSD__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_NETBSD
#elif defined(__OpenBSD__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_OPENBSD
#elif defined(__APPLE__)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_MACOS
#elif defined(__sun)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_SOLARIS
#elif defined(_WIN32)
#  define LKQ_BUILD_OS  LKQ_PLATFORM_OS_WINDOWS
#else
#  define LKQ_BUILD_OS  0
#endif

#define LKQ_BUILD_PLATFORM  (LKQ_BUILD_OS | LKQ_BUILD_BACKEND)

/*
 * Gate entry: skip the test when (lkq_current_platform & platform) != 0.
 * A NULL reason field terminates the gate array.
 */
struct lkq_test_gate {
    lkq_platform_t  platform;
    const char     *reason;    /* NULL terminates the array */
};

/*
 * Per-test-case entry.
 *
 * tc_name == NULL  : end-of-array sentinel.
 * tc_name == ""    : setup/teardown step.  tc_func() is called but the entry
 *                    is not counted, gated, or printed.  Use LKQ_SETUP().
 * tc_name[0] != NUL: normal test case.
 */
struct lkq_test_case {
    const char              *name;
    const char              *desc;
    void                    (*func)(struct test_context *);
    const struct lkq_test_gate *gates;  /* NULL = always run */
};

/* Convenience macros for test-case array entries. */
#define LKQ_SETUP(_fn)   { "", NULL, (_fn), NULL }
#define LKQ_SUITE_END    { NULL, NULL, NULL, NULL }

/* Inline gate-entry helper for use in named gate arrays. */
#define GATE(_platform, _reason)  { (_platform), (_reason) }

/*
 * Test-case entries are metadata and must always be present so that
 * --list-gated reports the full set on every platform; #ifdef'ing an
 * entry out takes its .gates with it and leaves the matrix incomplete.
 * Keep every entry, gate only the function body, and resolve .func to
 * NULL on the build where the body is compiled out (a matching gate
 * skips it before run_test_suite would call it).
 *
 *   TEST_FUNC_NEEDS_POSIX(fn) - fn on POSIX builds, NULL on Windows.
 *   TEST_GATES(...)   - a self-terminating gate array as a compound
 *                      literal, for gates used by a single test.
 */
#ifdef _WIN32
#  define TEST_FUNC_NEEDS_POSIX(_fn)  NULL
#else
#  define TEST_FUNC_NEEDS_POSIX(_fn)  (_fn)
#endif

#define TEST_GATES(...)  ((const struct lkq_test_gate[]){ __VA_ARGS__, { 0, NULL } })

/*
 * Per-feature resolvers for a test whose body needs an optional macro.
 * The entry and its gate stay present on every build (never #ifdef'd
 * out); only the resolved values change with the macro's availability:
 *
 *   TEST_FUNC_NEEDS_<X>(fn) - fn where <X> is defined, NULL where it
 *                             isn't (the body is compiled out there).
 *   TEST_GATE_NEEDS_<X>     - 0 where <X> is defined (gate inert, test
 *                             runs), else LKQ_BUILD_PLATFORM (the gate
 *                             skips and lists the test on this build).
 */
#ifdef NOTE_TRUNCATE
#  define TEST_FUNC_NEEDS_NOTE_TRUNCATE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_TRUNCATE       0
#else
#  define TEST_FUNC_NEEDS_NOTE_TRUNCATE(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_TRUNCATE       LKQ_BUILD_PLATFORM
#endif

#ifdef EV_RECEIPT
#  define TEST_FUNC_NEEDS_EV_RECEIPT(_fn)  (_fn)
#  define TEST_GATE_NEEDS_EV_RECEIPT       0
#else
#  define TEST_FUNC_NEEDS_EV_RECEIPT(_fn)  NULL
#  define TEST_GATE_NEEDS_EV_RECEIPT       LKQ_BUILD_PLATFORM
#endif

#ifdef EV_DISPATCH
#  define TEST_FUNC_NEEDS_EV_DISPATCH(_fn)  (_fn)
#  define TEST_GATE_NEEDS_EV_DISPATCH       0
#else
#  define TEST_FUNC_NEEDS_EV_DISPATCH(_fn)  NULL
#  define TEST_GATE_NEEDS_EV_DISPATCH       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_WRITE
#  define TEST_FUNC_NEEDS_NOTE_WRITE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_WRITE       0
#else
#  define TEST_FUNC_NEEDS_NOTE_WRITE(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_WRITE       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_RENAME
#  define TEST_FUNC_NEEDS_NOTE_RENAME(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_RENAME       0
#else
#  define TEST_FUNC_NEEDS_NOTE_RENAME(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_RENAME       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_EXTEND
#  define TEST_FUNC_NEEDS_NOTE_EXTEND(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_EXTEND       0
#else
#  define TEST_FUNC_NEEDS_NOTE_EXTEND(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_EXTEND       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_LINK
#  define TEST_FUNC_NEEDS_NOTE_LINK(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_LINK       0
#else
#  define TEST_FUNC_NEEDS_NOTE_LINK(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_LINK       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_SECONDS
#  define TEST_FUNC_NEEDS_NOTE_SECONDS(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_SECONDS       0
#else
#  define TEST_FUNC_NEEDS_NOTE_SECONDS(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_SECONDS       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_USECONDS
#  define TEST_FUNC_NEEDS_NOTE_USECONDS(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_USECONDS       0
#else
#  define TEST_FUNC_NEEDS_NOTE_USECONDS(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_USECONDS       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_NSECONDS
#  define TEST_FUNC_NEEDS_NOTE_NSECONDS(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_NSECONDS       0
#else
#  define TEST_FUNC_NEEDS_NOTE_NSECONDS(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_NSECONDS       LKQ_BUILD_PLATFORM
#endif

#ifdef NOTE_ABSOLUTE
#  define TEST_FUNC_NEEDS_NOTE_ABSOLUTE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NOTE_ABSOLUTE       0
#else
#  define TEST_FUNC_NEEDS_NOTE_ABSOLUTE(_fn)  NULL
#  define TEST_GATE_NEEDS_NOTE_ABSOLUTE       LKQ_BUILD_PLATFORM
#endif

#ifdef EVFILT_VNODE
#  define TEST_FUNC_NEEDS_EVFILT_VNODE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_EVFILT_VNODE       0
#else
#  define TEST_FUNC_NEEDS_EVFILT_VNODE(_fn)  NULL
#  define TEST_GATE_NEEDS_EVFILT_VNODE       LKQ_BUILD_PLATFORM
#endif

#ifdef EVFILT_PROC
#  define TEST_FUNC_NEEDS_EVFILT_PROC(_fn)  (_fn)
#  define TEST_GATE_NEEDS_EVFILT_PROC       0
#else
#  define TEST_FUNC_NEEDS_EVFILT_PROC(_fn)  NULL
#  define TEST_GATE_NEEDS_EVFILT_PROC       LKQ_BUILD_PLATFORM
#endif

#ifdef SIGRTMIN
#  define TEST_FUNC_NEEDS_SIGRTMIN(_fn)  (_fn)
#  define TEST_GATE_NEEDS_SIGRTMIN       0
#else
#  define TEST_FUNC_NEEDS_SIGRTMIN(_fn)  NULL
#  define TEST_GATE_NEEDS_SIGRTMIN       LKQ_BUILD_PLATFORM
#endif

/*
 * Build-capability resolvers (same shape as the feature ones, keyed on
 * build configuration rather than an <sys/event.h> macro).
 */
#ifdef NATIVE_KQUEUE
#  define TEST_FUNC_NEEDS_NATIVE_KQUEUE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NATIVE_KQUEUE       0
#  define TEST_FUNC_NEEDS_LIBKQUEUE(_fn)      NULL
#  define TEST_GATE_NEEDS_LIBKQUEUE           LKQ_BUILD_PLATFORM
#else
#  define TEST_FUNC_NEEDS_NATIVE_KQUEUE(_fn)  NULL
#  define TEST_GATE_NEEDS_NATIVE_KQUEUE       LKQ_BUILD_PLATFORM
#  define TEST_FUNC_NEEDS_LIBKQUEUE(_fn)      (_fn)
#  define TEST_GATE_NEEDS_LIBKQUEUE           0
#endif

/*
 * libkqueue_drain_pending_close() is a Linux-backend-only entry point.
 * Define the availability macro here, before the resolver below uses it
 * - if it were only #defined in kqueue.c (after this header is included)
 * the resolver would always see it undefined and gate every drain test
 * on every platform.
 */
#if defined(LIBKQUEUE_BACKEND_LINUX) || \
    (defined(__linux__) && !defined(LIBKQUEUE_BACKEND_POSIX))
#  define HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE 1
void libkqueue_drain_pending_close(void);
#endif

#ifdef HAVE_LIBKQUEUE_DRAIN_PENDING_CLOSE
#  define TEST_FUNC_NEEDS_DRAIN(_fn)  (_fn)
#  define TEST_GATE_NEEDS_DRAIN       0
#else
#  define TEST_FUNC_NEEDS_DRAIN(_fn)  NULL
#  define TEST_GATE_NEEDS_DRAIN       LKQ_BUILD_PLATFORM
#endif

#ifdef HAVE_TSAN_IGNORE
#  define TEST_FUNC_NEEDS_TSAN_IGNORE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_TSAN_IGNORE       0
#else
#  define TEST_FUNC_NEEDS_TSAN_IGNORE(_fn)  NULL
#  define TEST_GATE_NEEDS_TSAN_IGNORE       LKQ_BUILD_PLATFORM
#endif

#ifdef TEST_DROP_LOCK_WAKE
#  define TEST_FUNC_NEEDS_DROP_LOCK_WAKE(_fn)  (_fn)
#  define TEST_GATE_NEEDS_DROP_LOCK_WAKE       0
#else
#  define TEST_FUNC_NEEDS_DROP_LOCK_WAKE(_fn)  NULL
#  define TEST_GATE_NEEDS_DROP_LOCK_WAKE       LKQ_BUILD_PLATFORM
#endif

#if WITH_NATIVE_KQUEUE_BUGS
#  define TEST_FUNC_NEEDS_NATIVE_KQUEUE_BUGS(_fn)  (_fn)
#  define TEST_GATE_NEEDS_NATIVE_KQUEUE_BUGS       0
#else
#  define TEST_FUNC_NEEDS_NATIVE_KQUEUE_BUGS(_fn)  NULL
#  define TEST_GATE_NEEDS_NATIVE_KQUEUE_BUGS       LKQ_BUILD_PLATFORM
#endif

#if defined(_WIN32)
#  define TEST_FUNC_NEEDS_WIN32(_fn)  (_fn)
#  define TEST_GATE_NEEDS_WIN32       0
#else
#  define TEST_FUNC_NEEDS_WIN32(_fn)  NULL
#  define TEST_GATE_NEEDS_WIN32       LKQ_BUILD_PLATFORM
#endif

#if defined(_WIN32) && _WIN32_WINNT >= 0x0A00
#  define TEST_FUNC_NEEDS_WIN10(_fn)  (_fn)
#  define TEST_GATE_NEEDS_WIN10       0
#else
#  define TEST_FUNC_NEEDS_WIN10(_fn)  NULL
#  define TEST_GATE_NEEDS_WIN10       LKQ_BUILD_PLATFORM
#endif

/*
 * Current platform bitmask, OR of one OS bit and one backend bit.
 * Defined in main.c; computed from compile-time macros.
 */
extern lkq_platform_t lkq_current_platform;

/*
 * Iterate a null-terminated lkq_test_case array, evaluating gates and
 * respecting the test-number range in ctx->test.
 */
void run_test_suite(struct test_context *ctx, const struct lkq_test_case *cases);

#define MAX_TESTS 50
struct test_context {
    struct unit_test tests[MAX_TESTS];
    struct unit_test *test; /* Current test being run */

    char *cur_test_id;
    int iterations;
    int iteration;
    int kqfd;

    /* EVFILT_READ and EVFILT_WRITE */
    int client_fd;
    int server_fd;
    int listen_fd;

    /* EVFILT_VNODE */
    int vnode_fd;
    char testfile[1024];
};

void test_kqueue(struct test_context *);
void test_evfilt_read(struct test_context *);
void test_evfilt_signal(struct test_context *);
void test_evfilt_vnode(struct test_context *);
void test_evfilt_write(struct test_context *);
void test_evfilt_timer(struct test_context *);
void test_evfilt_proc(struct test_context *);
#ifdef EVFILT_USER
void test_evfilt_user(struct test_context *);
#endif
void test_evfilt_libkqueue(struct test_context *);
void test_threading(struct test_context *);

extern void watchdog_heartbeat(const char *test_name);
#define test(f, ctx ,...) do {                                            \
    if ((ctx->test->ut_num >= ctx->test->ut_start) && (ctx->test->ut_num <= ctx->test->ut_end)) {\
        assert(ctx != NULL); \
        watchdog_heartbeat("test_"#f); \
        test_begin(ctx, "test_"#f"()\t"__VA_ARGS__); \
        errno = 0; \
        test_##f(ctx); \
        test_end(ctx); \
    } \
    ctx->test->ut_num++; \
} while (/*CONSTCOND*/0)

extern const char * kevent_to_str(struct kevent *);
void kevent_get(struct kevent kev[], int numevents, int kqfd, int expect_rv);
void kevent_get_hires(struct kevent kev[], int numevents, int kqfd, struct timespec *timeout);
int kevent_get_timeout(struct kevent kev[], int numevents, int kqfd, struct timespec *timeout);

void kevent_update(int kqfd, struct kevent *kev);
void kevent_update_expect_fail(int kqfd, struct kevent *kev);

#define kevent_cmp(a,b) _kevent_cmp(a,b, __FILE__, __LINE__)
void _kevent_cmp(struct kevent *expected, struct kevent *got, const char *file, int line);

#define kevent_rv_cmp(a,b) _kevent_rv_cmp(a,b, __FILE__, __LINE__)
void _kevent_rv_cmp(int expected, int got, const char *file, int line);

void
kevent_add(int kqfd, struct kevent *kev,
        uintptr_t ident,
        short     filter,
        u_short   flags,
        u_int     fflags,
        intptr_t  data,
        void      *udata);


#define kevent_add_with_receipt(...) _kevent_add_with_receipt(__VA_ARGS__, __FILE__, __LINE__)
void
_kevent_add_with_receipt(int kqfd, struct kevent *kev,
        uintptr_t ident,
        short     filter,
        u_short   flags,
        u_int     fflags,
        intptr_t  data,
        void      *udata,
        char const *file,
        int line);

/* Checks if any events are pending, which is an error. */
#define test_no_kevents(_kq) _test_no_kevents(_kq, __FILE__, __LINE__)
void _test_no_kevents(int, const char *, int);

unsigned int print_fd_table(void);
unsigned int get_fd_limit(void);

/* From test.c */
void    test_begin(struct test_context *, const char *);
void    test_end(struct test_context *);

/* Where on-disk test files live; --tmpdir override, $TMPDIR, then
 * platform default ("/tmp" usually). */
const char *test_tmpdir(void);
void        test_tmpdir_set(const char *path);
void    test_atexit(void);
void    testing_begin(void);
void    testing_end(void);
void    testing_end_quiet(void);
int     testing_make_uid(void);

#endif  /* _COMMON_H */
