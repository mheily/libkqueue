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
    if (ctx->cur_test_id)
        free(ctx->cur_test_id);
    ctx->cur_test_id = strdup(func);

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
