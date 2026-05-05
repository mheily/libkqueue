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
#include <limits.h>

#if defined(__linux__) && (defined(__GLIBC__) && !defined(__UCLIBC__))
#  include <execinfo.h>
#endif

#include "common.h"

static int testnum = 1;
static int error_flag = 1;

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
                printf("%d: SKIP %s -- %s\n",
                       ctx->test->ut_num, tc->name, reason);
                ctx->test->ut_num++;
                continue;
            }
        }

        snprintf(display, sizeof(display), "%s()\t%s",
                 tc->name, tc->desc ? tc->desc : "");
        watchdog_heartbeat(tc->name);
        test_begin(ctx, display);
        errno = 0;
        tc->func(ctx);
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
