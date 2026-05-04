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
#if (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(LIBKQUEUE_BACKEND_POSIX)
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

#define test(f, ctx ,...) do {                                            \
    if ((ctx->test->ut_num >= ctx->test->ut_start) && (ctx->test->ut_num <= ctx->test->ut_end)) {\
        assert(ctx != NULL); \
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
