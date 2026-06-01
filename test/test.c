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
#include <sys/types.h>
#include <assert.h>
#include <limits.h>

#if defined(__linux__) && (defined(__GLIBC__) && !defined(__UCLIBC__))
#  include <execinfo.h>
#endif
#ifdef _WIN32
#  include <io.h>
#  include <process.h>
#endif

#include "common.h"

static int testnum = 1;
static int error_flag = 1;

/*
 * Per-test debug-log routing.
 *
 * libkqueue's KQUEUE_DEBUG output and the crash backtrace both land on
 * stderr; across a 50-iteration soak that is gigabytes in a single
 * stream, useless for finding the one test that died.  With
 * --log-file=<template>, stderr is redirected to a freshly truncated
 * file around each test body so each test's trail lands in its own
 * small file, while stdout keeps the clean pass/skip/crash progress.
 * stderr is restored between tests so the sanitizers' at-exit report
 * still reaches the console.
 *
 * Template specifiers: %{t} test name, %{i} iteration, %{p} pid,
 * %% literal percent.
 */
#ifdef _WIN32
#  define kq_dup    _dup
#  define kq_dup2   _dup2
#  define kq_getpid ((long) _getpid())
#  define kq_open(p) _open((p), _O_WRONLY | _O_CREAT | _O_TRUNC, 0600)
#  define kq_close  _close
#else
#  define kq_dup    dup
#  define kq_dup2   dup2
#  define kq_getpid ((long) getpid())
#  define kq_open(p) open((p), O_WRONLY | O_CREAT | O_TRUNC, 0644)
#  define kq_close  close
#endif

static char *test_log_template    = NULL;
static int   test_log_orig_stderr = -1;

void
test_log_set_template(const char *tmpl)
{
    free(test_log_template);
    test_log_template = tmpl ? strdup(tmpl) : NULL;
}

static void
test_log_expand(char *out, size_t outlen, const char *tmpl,
                const char *test, int iter)
{
    const char *p = tmpl;
    size_t      o = 0;

    while (*p && ((o + 1) < outlen)) {
        if ((p[0] == '%') && (p[1] == '%')) {
            out[o++] = '%';
            p += 2;
            continue;
        }
        if ((p[0] == '%') && (p[1] == '{') && p[2] && (p[3] == '}')) {
            char        num[32];
            const char *ins = NULL;

            switch (p[2]) {
            case 't':
                ins = test;
                break;
            case 'i':
                snprintf(num, sizeof(num), "%d", iter);
                ins = num;
                break;
            case 'p':
                snprintf(num, sizeof(num), "%ld", kq_getpid);
                ins = num;
                break;
            default:
                break;
            }
            if (ins) {
                while (*ins && ((o + 1) < outlen))
                    out[o++] = *ins++;
                p += 4;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static void
test_log_begin(const char *test, int iter)
{
    char path[1024];
    int  fd;

    if (!test_log_template)
        return;

    test_log_expand(path, sizeof(path), test_log_template, test, iter);

    if (test_log_orig_stderr < 0)
        test_log_orig_stderr = kq_dup(fileno(stderr));

    fd = kq_open(path);
    if (fd < 0)
        return;

    /*
     * Repoint fd 2 at the per-test file with dup2() rather than
     * freopen(stderr).  freopen frees and reallocates the stderr FILE
     * object's buffer, which races libkqueue's monitoring thread
     * writing debug to stderr (a use-after-free flagged by ASAN/TSAN).
     * dup2 only swaps the kernel fd; the FILE object is untouched and
     * stdio's per-stream lock keeps concurrent writers safe.  stderr is
     * unbuffered by default, so a crash mid-test still leaves the trail
     * on disk without a setvbuf() that would itself touch the buffer.
     */
    fflush(stderr);
    kq_dup2(fd, fileno(stderr));
    kq_close(fd);
}

static void
test_log_end(void)
{
    if (!test_log_template || (test_log_orig_stderr < 0))
        return;

    fflush(stderr);
    kq_dup2(test_log_orig_stderr, fileno(stderr));
    clearerr(stderr);
}

#ifndef _WIN32
static void
error_handler(int signum)
{
#  if defined(__linux__) && (defined(__GLIBC__) && !defined(__UCLIBC__))
    void *buf[32];

    /* FIXME: the symbols aren't printing */
    printf("***** ERROR: Program received signal %d *****\n", signum);
    backtrace_symbols_fd(buf, sizeof(buf) / sizeof(void *), 2);
#  else
    printf("***** ERROR: Program received signal %d *****\n", signum);
#  endif
    exit(1);
}
#endif /* ! _WIN32 */

static void
testing_atexit(void)
{
    if (error_flag == 1) {
        printf(" *** TEST FAILED ***\n");
        //TODO: print detailed log
    } else if (error_flag == 0) {
        printf("\n---\n"
                "+OK All %d tests completed.\n", testnum - 1);
    }
}

void
test_begin(struct test_context *ctx, const char *func)
{
    extern void watchdog_heartbeat(const char *);

    if (ctx->cur_test_id)
        free(ctx->cur_test_id);
    ctx->cur_test_id = strdup(func);

    /*
     * Per-test heartbeat: lets the watchdog distinguish "this test is
     * hung" from "the test class is just slow under valgrind".
     */
    watchdog_heartbeat(ctx->cur_test_id);

    printf("%d: %s\n", ctx->test->ut_num, ctx->cur_test_id);
    //TODO: redirect stdout/err to logfile
    testnum++;
}

void
test_end(struct test_context *ctx)
{
    free(ctx->cur_test_id);
    ctx->cur_test_id = NULL;
}

void
testing_begin(void)
{
#ifndef _WIN32
    struct sigaction sa;

    /* Install a signal handler for crashes and hangs */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = error_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
#endif

    atexit(testing_atexit);

}

void
testing_end_quiet(void)
{
    error_flag = 2;
}

void
testing_end(void)
{
    error_flag = 0;
}

lkq_platform_t lkq_current_platform;

bool lkq_show_skips;

static int
test_case_is_gated(const struct lkq_test_case *tc, lkq_platform_t plat,
                   const char **reason_out)
{
    const struct lkq_test_gate *g;

    if (!tc->gates) return 0;
    for (g = tc->gates; g->reason != NULL; g++) {
        if (plat & g->platform) {
            *reason_out = g->reason;
            return 1;
        }
    }
    return 0;
}

void
run_test_suite(struct test_context *ctx, const struct lkq_test_case *cases)
{
    const struct lkq_test_case *tc;
    char display[512];

    for (tc = cases; tc->name != NULL; tc++) {
        /* Setup/teardown step: not a test, just call it. */
        if (tc->name[0] == '\0') {
            if (tc->func) tc->func(ctx);
            continue;
        }

        if (ctx->test->ut_num < ctx->test->ut_start ||
            ctx->test->ut_num > ctx->test->ut_end) {
            ctx->test->ut_num++;
            continue;
        }

        {
            const char *reason = NULL;
            if (test_case_is_gated(tc, lkq_current_platform, &reason)) {
                if (lkq_show_skips)
                    printf("%d: SKIP %s -- %s\n",
                           ctx->test->ut_num, tc->name, reason);
                ctx->test->ut_num++;
                continue;
            }
        }

        snprintf(display, sizeof(display), "%s()\t%s",
                 tc->name, tc->desc ? tc->desc : "");
        /*
         * A NULL func means the body was compiled out on this build
         * (TEST_FUNC_NEEDS_POSIX); a gate must have skipped it above.  Reaching
         * here without a func is a missing-gate misconfiguration.
         */
        assert(tc->func);

        watchdog_heartbeat(tc->name);
        test_begin(ctx, display);
        errno = 0;
        test_log_begin(tc->name, ctx->iteration);
        tc->func(ctx);
        test_log_end();
        test_end(ctx);
        ctx->test->ut_num++;
    }
}

/* Generate a unique ID */
int
testing_make_uid(void)
{
    static int id = 0;

    if (id == INT_MAX)
        abort();
    id++;

    return (id);
}
