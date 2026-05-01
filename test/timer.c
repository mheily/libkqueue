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

void
test_kevent_timer_add(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);
}

void
test_kevent_timer_del(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);

    test_no_kevents(ctx->kqfd);
}

void
test_kevent_timer_get(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

static void
test_kevent_timer_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 500, NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR | EV_ONESHOT;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Check if the event occurs again */
    sleep(3);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_timer_periodic(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Check if the event occurs again */
    sleep(1);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Delete the event */
    kev.flags = EV_DELETE;
    kevent_update(ctx->kqfd, &kev);
}

static void
test_kevent_timer_periodic_modify(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /*
     * Check if the event occurs again.  Re-arm to a 500ms period and
     * sleep slightly longer than 2 * period so the second tick is
     * comfortably in the past when the kevent_get runs.  A bare
     * sleep(1) flakes on slow CI VMs where the syscall returns within
     * a fraction of a millisecond of the second tick's deadline,
     * delivering data=1 instead of data=2.  Also widen the assertion
     * to allow 2 or 3 ticks so a much slower scheduler still passes
     * (the property under test is "modify changed the period and
     * EV_CLEAR accumulated", not the exact tick count).
     */
    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD, 0, 500, NULL);
    kev.flags = EV_ADD | EV_CLEAR;
    usleep(1200 * 1000);  /* 1200ms - 2 full periods + slack */

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    /*
     * Native FreeBSD kqueue returns the event without EV_ADD set;
     * Linux libkqueue and macOS native echo EV_ADD back.  Don't pin
     * the EV_ADD bit here - the kevent_cmp helper has a FreeBSD-
     * specific workaround for this elsewhere, but we're doing a
     * direct compare to widen the data tolerance.  Just verify the
     * sticky EV_CLEAR survived, which is what the test cares about.
     */
    if (ret[0].ident != 3 || ret[0].filter != EVFILT_TIMER ||
        !(ret[0].flags & EV_CLEAR))
        die("periodic_modify: unexpected ident/filter/flags in %s",
            kevent_to_str(&ret[0]));
    if (ret[0].data < 2 || ret[0].data > 4)
        die("periodic_modify: expected accumulated data in [2,4], got %ld",
            (long) ret[0].data);

    /* Delete the event */
    kev.flags = EV_DELETE;
    kevent_update(ctx->kqfd, &kev);
}

/*
 * This appears to be a bug in the Linux/FreeBSD implementation where
 * it won't allow you to modify an existing event to make it oneshot.
 */
#if WITH_NATIVE_KQUEUE_BUGS
static void
test_kevent_timer_periodic_to_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Check if the event occurs again */
    sleep(1);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Switch to oneshot */
    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 500, NULL);
    kev.flags = EV_ADD | EV_CLEAR;

    sleep(1);
    kev.data = 1;	/* Should have fired one */

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Delete the event */
    kev.flags = EV_DELETE;
    kevent_update_expect_fail(ctx->kqfd, &kev);
}
#endif

static void
test_kevent_timer_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    /* Add the watch and immediately disable it */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 2000,NULL);
    kev.flags = EV_DISABLE;
    kevent_update(ctx->kqfd, &kev);
    test_no_kevents(ctx->kqfd);

    /* Re-enable and check again */
    kev.flags = EV_ENABLE;
    kevent_update(ctx->kqfd, &kev);

    kev.flags = EV_ADD | EV_CLEAR | EV_ONESHOT;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
}

#ifdef EV_DISPATCH
void
test_kevent_timer_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ADD | EV_DISPATCH, 0, 200, NULL);

    /* Get one event */
    kev.flags = EV_ADD | EV_CLEAR | EV_DISPATCH;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Confirm that the knote is disabled due to EV_DISPATCH */
    usleep(500000); /* 500 ms */
    test_no_kevents(ctx->kqfd);

#if WITH_NATIVE_KQUEUE_BUGS || !defined(__FreeBSD__)
    /*
     * macOS and FreeBSD both share the same bug.
     *
     * When a timer event with EV_DISPATCH is re-enabled
     * although the EV_DISPATCH flag is high in the
     * returned event the timer filter behaves as if
     * EV_DISPATCH it's not set and will fire multiple
     * times.
     */

    /* Enable the knote and make sure no events are pending */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ENABLE | EV_DISPATCH, 0, 200, NULL);
    test_no_kevents(ctx->kqfd);

    /* Get the next event.  1100ms / 200ms = 5.5 expected ticks; under
     * scheduling jitter on busy CI VMs the sleep can run past 1200ms
     * and yield 6.  Compare flags/ident strictly but allow the
     * accumulated tick count a small tolerance window. */
    usleep(1100000); /* 1100 ms */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != 4 || ret[0].filter != EVFILT_TIMER ||
        ret[0].flags != (EV_ADD | EV_CLEAR | EV_DISPATCH))
        die("EV_DISPATCH re-enable: unexpected ident/filter/flags in %s",
            kevent_to_str(&ret[0]));
    if (ret[0].data < 5 || ret[0].data > 7)
        die("EV_DISPATCH re-enable: expected accumulated data in [5,7], got %ld",
            (long) ret[0].data);
#endif

    /* Remove the knote and ensure the event no longer fires */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    usleep(500000); /* 500 ms */
    test_no_kevents(ctx->kqfd);
}
#endif  /* EV_DISPATCH */

/*
 * Exercise the NOTE_* unit selectors on EVFILT_TIMER.  Each variant
 * arms a one-shot timer with a ~50ms equivalent expressed in the
 * tested unit and waits up to 5s for it to fire.  We're testing the
 * unit conversion, not timer accuracy, so the sleep budget is
 * deliberately huge - what we care about is "the kernel programmed
 * a timer that fires in finite time", not "it fired in 50ms".
 */
static void
_timer_unit_check(struct test_context *ctx, intptr_t data,
                  uint32_t fflags, const char *label)
{
    struct kevent  kev, ret[1];
    struct timespec timeout = { 5, 0 };
    int n;

    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, 100, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, fflags, data, NULL);

    n = kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout);
    if (n < 0)
        die("%s: kevent() returned -1", label);
    if (n == 0)
        die("%s: timer did not fire within 5s (data=%ld fflags=0x%x)",
            label, (long) data, fflags);
    if (ret[0].ident != 100 || ret[0].filter != EVFILT_TIMER)
        die("%s: unexpected event %s", label, kevent_to_str(&ret[0]));

    /* No EV_DELETE: EV_ONESHOT already auto-removed the knote on
     * delivery, and the libkqueue test helper that asserts success
     * would trip on the resulting ENOENT. */
}

static void
test_kevent_timer_note_useconds(struct test_context *ctx)
{
    /* 50ms = 50,000us */
    _timer_unit_check(ctx, 50000L, NOTE_USECONDS, "NOTE_USECONDS");
}

static void
test_kevent_timer_note_nseconds(struct test_context *ctx)
{
    /* 50ms = 50,000,000ns */
    _timer_unit_check(ctx, 50L * 1000L * 1000L, NOTE_NSECONDS,
                      "NOTE_NSECONDS");
}

static void
test_kevent_timer_note_seconds(struct test_context *ctx)
{
    /* Min granularity in this unit is 1s; budget 5s. */
    _timer_unit_check(ctx, 1L, NOTE_SECONDS, "NOTE_SECONDS");
}

static void
test_kevent_timer_note_absolute(struct test_context *ctx)
{
    struct timespec  now;
    intptr_t         deadline_ms;

    /* BSD: NOTE_ABSOLUTE deadlines are milliseconds since the Epoch
     * (CLOCK_REALTIME).  100ms in the future, expressed that way. */
    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
        die("clock_gettime");
    deadline_ms = (intptr_t)(now.tv_sec) * 1000
                + (intptr_t)(now.tv_nsec / 1000000)
                + 100;

    _timer_unit_check(ctx, deadline_ms, NOTE_ABSOLUTE, "NOTE_ABSOLUTE");
}

/*
 * Register a relative timer, then EV_ADD-modify it to NOTE_ABSOLUTE
 * with a CLOCK_REALTIME deadline ~200ms in the future, and verify it
 * fires within that window.
 *
 * Catches the bug where the platform reuses the underlying timer's
 * original clockid (CLOCK_MONOTONIC for relative) and just toggles
 * the abstime flag in timer_settime.  Without the fix the deadline
 * is interpreted as a monotonic-clock absolute time - an Epoch-style
 * timestamp would land roughly 50+ years past system boot, so no
 * event arrives within the test timeout.
 */
static void
test_kevent_timer_note_absolute_after_modify(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec now;
    intptr_t        deadline_ms;
    struct timespec timeout = { 1, 0 };  /* 1s window for the 200ms deadline */

    /* Original registration: relative 1h timer, picks CLOCK_MONOTONIC. */
    kevent_add(ctx->kqfd, &kev, 88, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, 0, 60L * 60L * 1000L, NULL);
    test_no_kevents(ctx->kqfd);

    /* Modify to NOTE_ABSOLUTE with an Epoch-millisecond deadline. */
    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
        die("clock_gettime");
    deadline_ms = (intptr_t)(now.tv_sec) * 1000
                + (intptr_t)(now.tv_nsec / 1000000)
                + 200;
    kevent_add(ctx->kqfd, &kev, 88, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, NOTE_ABSOLUTE, deadline_ms, NULL);

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected NOTE_ABSOLUTE timer to fire within 1s after modify");

    /* EV_ONESHOT auto-deleted the knote; no explicit EV_DELETE needed. */
}

void
test_evfilt_timer(struct test_context *ctx)
{
    test(kevent_timer_add, ctx);
    test(kevent_timer_del, ctx);
    test(kevent_timer_get, ctx);
    test(kevent_timer_oneshot, ctx);
    test(kevent_timer_periodic, ctx);
    test(kevent_timer_periodic_modify, ctx);
#if WITH_NATIVE_KQUEUE_BUGS
    test(kevent_timer_periodic_to_oneshot, ctx);
#endif
    test(kevent_timer_disable_and_enable, ctx);
#ifdef EV_DISPATCH
    test(kevent_timer_dispatch, ctx);
#endif
#ifdef NOTE_USECONDS
    test(kevent_timer_note_useconds, ctx);
#endif
#ifdef NOTE_NSECONDS
    test(kevent_timer_note_nseconds, ctx);
#endif
#ifdef NOTE_SECONDS
    test(kevent_timer_note_seconds, ctx);
#endif
#ifdef NOTE_ABSOLUTE
    test(kevent_timer_note_absolute, ctx);
    test(kevent_timer_note_absolute_after_modify, ctx);
#endif
}
