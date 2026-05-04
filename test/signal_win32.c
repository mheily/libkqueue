/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

/*
 * Win32 EVFILT_SIGNAL smoke tests.  POSIX test/signal.c uses
 * sigaction(SIGUSR1) + kill(SIGUSR1) which the named-event bridge
 * does not interoperate with at the OS layer; this file exercises
 * the same code paths via the libkqueue-supplied kq_kill / kq_raise
 * shims that SetEvent() the matching named event.
 */

#include "common.h"

#ifdef _WIN32

#include <kqueue/signal.h>
#include <signal.h>
#include <windows.h>

static void
test_kevent_signal_add_and_raise(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL,
               EV_ADD | EV_CLEAR, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    if (kq_raise(SIGUSR1) != 0)
        die("kq_raise(SIGUSR1)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].filter != EVFILT_SIGNAL)
        die("expected EVFILT_SIGNAL, got %d", (int) ret[0].filter);
    if (ret[0].ident != SIGUSR1)
        die("expected ident=%d, got %d",
            (int) SIGUSR1, (int) ret[0].ident);
    if (ret[0].data < 1)
        die("expected data >= 1 (delivery count), got %lld",
            (long long) ret[0].data);

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL,
               EV_DELETE, 0, 0, NULL);
}

static void
test_kevent_signal_coalesce(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           i;

    kevent_add(ctx->kqfd, &kev, SIGUSR2, EVFILT_SIGNAL,
               EV_ADD | EV_CLEAR, 0, 0, NULL);

    /*
     * Auto-reset event semantics: N SetEvents while no Wait is
     * outstanding coalesce to one wake.  The data field carries
     * the per-knote fire_count which can be any value >= 1
     * depending on RegisterWait timing.
     */
    for (i = 0; i < 4; i++)
        if (kq_raise(SIGUSR2) != 0) die("kq_raise(SIGUSR2)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != SIGUSR2)
        die("expected ident=SIGUSR2");
    if (ret[0].data < 1)
        die("expected coalesced delivery count >= 1");

    kevent_add(ctx->kqfd, &kev, SIGUSR2, EVFILT_SIGNAL,
               EV_DELETE, 0, 0, NULL);
}

static void
test_kevent_signal_no_listener(struct test_context *ctx)
{
    /*
     * kq_kill / kq_raise to a signum nobody EV_ADD'd surfaces as
     * ESRCH (no such target), matching POSIX kill() behaviour
     * when the named target does not exist.
     */
    (void) ctx;
    /*
     * Use a high signum no other test will subscribe to so this
     * test order-independent.
     */
    int orig_errno = errno;
    errno = 0;
    if (kq_raise(63) == 0)
        die("kq_raise(unsubscribed) unexpectedly succeeded");
    if (errno != ESRCH)
        die("expected ESRCH, got errno=%d", errno);
    errno = orig_errno;
}

static void
test_kevent_signal_console_ctrl_bridge(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, SIGINT, EVFILT_SIGNAL,
               EV_ADD | EV_CLEAR, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    /*
     * GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) targets every
     * process attached to the current console.  CI runs without an
     * attached console for the test process, so this can fail with
     * ERROR_INVALID_HANDLE - skip cleanly in that case rather than
     * failing the whole suite.  When a console IS attached, the
     * handler bridge installed by windows_signal_init lights up
     * the SIGINT named event and this delivers as expected.
     */
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
        DWORD err = GetLastError();
        printf(" -- GenerateConsoleCtrlEvent skipped (gle=%lu)\n",
               (unsigned long) err);
        kevent_add(ctx->kqfd, &kev, SIGINT, EVFILT_SIGNAL,
                   EV_DELETE, 0, 0, NULL);
        return;
    }
    /* Brief wait for the handler thread to dispatch. */
    Sleep(50);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].filter != EVFILT_SIGNAL || ret[0].ident != SIGINT)
        die("expected EVFILT_SIGNAL ident=SIGINT");

    kevent_add(ctx->kqfd, &kev, SIGINT, EVFILT_SIGNAL,
               EV_DELETE, 0, 0, NULL);
}

void
test_evfilt_signal(struct test_context *ctx)
{
    test(kevent_signal_add_and_raise, ctx);
    test(kevent_signal_coalesce, ctx);
    test(kevent_signal_no_listener, ctx);
    test(kevent_signal_console_ctrl_bridge, ctx);
}

#endif /* _WIN32 */
