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
    /*
     * Win32 CI VMs have notoriously coarse timer scheduling
     * (15ms default tick), Sleep() returns can lag by tens of
     * ms, and Release builds occasionally land the first fire
     * past the 2x-period mark on shared runners.  Use a 3 *
     * period window so two ticks are comfortably in the past
     * before we drain.  Tests are gated on data >= 2, so a
     * generous wait is the safest knob.
     */
#ifdef _WIN32
    usleep(3000 * 1000);
#else
    usleep(1200 * 1000);  /* 1200ms - 2 full periods + slack */
#endif

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
    /*
     * Bounds scale with the wait window: POSIX waits 1200ms (~2-3
     * fires); Win32 waits 3000ms so more fires accumulate (~5-6).
     */
#ifdef _WIN32
    if (ret[0].data < 2 || ret[0].data > 8)
        die("periodic_modify: expected accumulated data in [2,8], got %ld",
            (long) ret[0].data);
#else
    if (ret[0].data < 2 || ret[0].data > 4)
        die("periodic_modify: expected accumulated data in [2,4], got %ld",
            (long) ret[0].data);
#endif

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

#ifdef NOTE_ABSOLUTE
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
#endif /* NOTE_ABSOLUTE */

/*
 * Flag-behaviour tests.
 *
 * EV_DISABLE/EV_DELETE pending-drain tests aren't included here:
 * the timer event is kernel-buffered, so detecting "fired but not
 * yet drained" requires either nanosleep (banned per project sync
 * rules) or a multi-threaded harness exploiting the
 * KEVENT_WAIT_DROP_LOCK race window.
 */

/*
 * BSD overwrites kn->kev.udata on every modify, including the timer
 * re-arm via EV_ADD.
 */
static void
test_kevent_timer_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };
    int             marker  = 0xab;

    kevent_add(ctx->kqfd, &kev, 80, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, 0, 60L * 60L * 1000L, &marker);
    /* Modify to a short period with udata=NULL. */
    kevent_add(ctx->kqfd, &kev, 80, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, 0, 50, NULL);

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected timer fire");
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);
}

/*
 * EV_RECEIPT is sticky on BSD: a knote registered with the bit keeps
 * it across modifies and reports it in every subsequent event until
 * EV_DELETE.  Catches the regression where a filter's kn_modify did
 * `kn->kev.flags = kev->flags | EV_CLEAR` and clobbered EV_RECEIPT.
 */
static void
test_kevent_timer_modify_preserves_ev_receipt(struct test_context *ctx)
{
    struct kevent   kev, receipt, ret[1];
    struct timespec timeout = { 1, 0 };

    /* Register with EV_RECEIPT and a 1h deadline so the only event we
     * see comes from the modify-driven fire below, not the original. */
    EV_SET(&kev, 77, EVFILT_TIMER,
           EV_ADD | EV_RECEIPT | EV_ONESHOT, 0, 60L * 60L * 1000L, NULL);
    if (kevent(ctx->kqfd, &kev, 1, &receipt, 1, NULL) != 1)
        die("expected immediate EV_RECEIPT confirmation");

    /* Modify to fire in 100ms, no EV_RECEIPT on the change. */
    EV_SET(&kev, 77, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 100, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) != 0)
        die("kevent modify failed");

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected timer to fire after modify");
    if (!(ret[0].flags & EV_RECEIPT))
        die("EV_RECEIPT was clobbered by kn_modify, flags=0x%x", ret[0].flags);
}

/*
 * EV_DELETE on a never-added ident must fail with ENOENT.
 */
static void
test_kevent_timer_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 999, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added timer should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));
}

/*
 * udata set on EV_ADD round-trips through delivery.
 */
static void
test_kevent_timer_udata_preserved(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };
    void           *marker  = (void *) 0xDEADBEEFUL;

    EV_SET(&kev, 70, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected timer fire");
    if (ret[0].udata != marker)
        die("udata not preserved: got %p, expected %p", ret[0].udata, marker);
}

/*
 * Re-EV_ADD replaces the fflags wholesale, not OR.  Register with
 * NOTE_NSECONDS, modify with NOTE_SECONDS - the timer should run
 * with NOTE_SECONDS semantics, not the union.
 */
#if defined(NOTE_NSECONDS) && defined(NOTE_SECONDS)
static void
test_kevent_timer_modify_replaces_fflags(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };

    /* Register with NOTE_NSECONDS for 1h.  Past EV_ADD must hold the
     * unit until the next modify replaces it. */
    EV_SET(&kev, 71, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_NSECONDS, 60L * 60L * 1000000000L, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* Modify with bare ms data and no NOTE_NSECONDS.  If kn_modify
     * ORed flags the timer would still run as nanoseconds and miss
     * the 1s window. */
    EV_SET(&kev, 71, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 100, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent modify");

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("modify_replaces_fflags: timer didn't fire in 1s window "
            "(NOTE_NSECONDS likely leaked through)");
}
#endif

/*
 * EV_DISABLE drops pending tick: arm a fast oneshot, busy-wait
 * past the deadline, then disable; nothing should be delivered.
 */
static void
test_kevent_timer_disable_drains(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 72, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);
    usleep(150 * 1000);  /* well past 50ms */

    kevent_add(ctx->kqfd, &kev, 72, EVFILT_TIMER, EV_DISABLE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 72, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

/*
 * EV_DELETE drops the pending tick (knote-list cleanup contract).
 */
static void
test_kevent_timer_delete_drains(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 73, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);
    usleep(150 * 1000);

    kevent_add(ctx->kqfd, &kev, 73, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * Same ident in two kqueues - independent timer state.
 */
static void
test_kevent_timer_multi_kqueue(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };
    int             kq2;

    if ((kq2 = kqueue()) < 0) die("kqueue(2)");

    kevent_add(ctx->kqfd, &kev, 74, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);
    kevent_add(kq2,        &kev, 74, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("kq1 timer didn't fire");
    if (kevent(kq2, NULL, 0, ret, 1, &timeout) != 1)
        die("kq2 timer didn't fire");

    close(kq2);
}

/*
 * Huge interval (NOTE_NSECONDS data near INT64_MAX): the kernel
 * (FreeBSD) clamps via SBT_MAX on LP64; libkqueue must either
 * accept and clamp or reject with EINVAL.  No silent overflow into
 * an immediate-fire.
 */
#ifdef NOTE_NSECONDS
static void
test_kevent_timer_huge_interval(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec poll = { 0, 100 * 1000 * 1000 };  /* 100ms */
    intptr_t        huge = INTPTR_MAX / 2;  /* well past any real timer */

    EV_SET(&kev, 760, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_NSECONDS, huge, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
        if (errno == EINVAL) {
            /* Acceptable: kernel rejected the overflow. */
            return;
        }
        die("kevent(huge interval) failed unexpectedly: %d", errno);
    }

    /*
     * Registered: must NOT fire within 100ms (huge interval is
     * effectively infinite).  Fire-immediate would indicate
     * silent overflow into negative.
     */
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &poll) > 0)
        die("huge-interval timer fired immediately - overflow bug");

    EV_SET(&kev, 760, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
}
#endif

/*
 * Negative interval contract diverges:
 *  - FreeBSD's filt_timervalidate + libkqueue POSIX/Linux: EINVAL.
 *  - OpenBSD/NetBSD: silently accept, fire immediately or busy-loop.
 *  - macOS: behaviour varies.
 * Test the strict-rejection contract on backends that pin to it.
 */
static void
test_kevent_timer_negative_interval_rejected(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 75, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, -1, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("negative interval should reject");
    if (errno != EINVAL)
        die("expected EINVAL, got %d (%s)", errno, strerror(errno));
}

#ifdef NOTE_ABSOLUTE
/*
 * NOTE_ABSOLUTE deadline already in the past must fire immediately.
 */
static void
test_kevent_timer_note_absolute_past(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec now;
    intptr_t        deadline_ms;
    struct timespec timeout = { 1, 0 };

    if (clock_gettime(CLOCK_REALTIME, &now) < 0) die("clock_gettime");
    deadline_ms = (intptr_t)(now.tv_sec) * 1000
                + (intptr_t)(now.tv_nsec / 1000000)
                - 5000;  /* 5s in the past */

    EV_SET(&kev, 76, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_ABSOLUTE, deadline_ms, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("past deadline didn't fire immediately");
}
#endif

/*
 * Overrun count: 50ms periodic, sleep 500ms (~10 ticks), one drain
 * must report data accumulated >> 1.  Exercises kernel-side tick
 * accumulation rather than per-tick wakeup.
 */
static void
test_kevent_timer_overrun_count(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };

    EV_SET(&kev, 78, EVFILT_TIMER, EV_ADD, 0, 50, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    usleep(500 * 1000);

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("periodic timer didn't fire");
    if (ret[0].data < 8)
        die("expected >=8 accumulated ticks, got %ld", (long) ret[0].data);

    kevent_add(ctx->kqfd, &kev, 78, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

/*
 * EV_CLEAR (the kernel sets it implicitly on EVFILT_TIMER) must
 * reset kev.data to 0 after delivery: a second drain over an idle
 * gap must return nothing.
 */
static void
test_kevent_timer_ev_clear_resets_data(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    struct timespec timeout = { 1, 0 };
    struct timespec brief   = { 0, 50 * 1000000 };  /* 50ms */

    /* 50ms periodic.  Drain once, then re-drain quickly without
     * waiting another period; should see no second event. */
    EV_SET(&kev, 79, EVFILT_TIMER, EV_ADD, 0, 50, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("first drain: timer didn't fire");
    /* Drain immediately again (might still be a tick window so allow
     * a 50us idle).  EV_CLEAR semantics: data zeroed; the only way
     * we'd see another event is if a second tick already accumulated.
     * The data on this re-drain (if it fires) must reset, not carry
     * the previous value. */
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &brief) == 1) {
        if (ret[0].data > 2)
            die("EV_CLEAR didn't reset data, got %ld", (long) ret[0].data);
    }

    kevent_add(ctx->kqfd, &kev, 79, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

void
test_evfilt_timer(struct test_context *ctx)
{
    test(kevent_timer_add, ctx);
    test(kevent_timer_del, ctx);
    test(kevent_timer_del_nonexistent, ctx);
    test(kevent_timer_get, ctx);
    test(kevent_timer_udata_preserved, ctx);
    test(kevent_timer_disable_drains, ctx);
    test(kevent_timer_delete_drains, ctx);
    test(kevent_timer_multi_kqueue, ctx);
    /*
     * Negative interval contract: FreeBSD and libkqueue reject;
     * OpenBSD/NetBSD/macOS don't validate.  Run only where the
     * EINVAL contract holds.
     */
#if !defined(NATIVE_KQUEUE) || defined(__FreeBSD__)
    test(kevent_timer_negative_interval_rejected, ctx);
#endif
    /*
     * Huge NOTE_NSECONDS interval: FreeBSD's filt_timervalidate
     * conversion path fires immediately for INTPTR_MAX/2 ns
     * (suspected sbintime conversion overflow); audit flagged
     * "Only LP64 has the SBT_MAX clamp; on 32-bit the data << 32
     * overflows silently".  libkqueue handles it sanely.  Gate
     * to libkqueue until the kernel-side overflow is verified
     * fixed across BSDs.
     */
#if defined(NOTE_NSECONDS) && !defined(NATIVE_KQUEUE)
    test(kevent_timer_huge_interval, ctx);
#endif
    test(kevent_timer_overrun_count, ctx);
    test(kevent_timer_ev_clear_resets_data, ctx);
#if defined(NOTE_NSECONDS) && defined(NOTE_SECONDS)
    test(kevent_timer_modify_replaces_fflags, ctx);
#endif
#ifdef NOTE_ABSOLUTE
    test(kevent_timer_note_absolute_past, ctx);
#endif
    test(kevent_timer_oneshot, ctx);
    test(kevent_timer_periodic, ctx);
    test(kevent_timer_periodic_modify, ctx);
#if WITH_NATIVE_KQUEUE_BUGS
    test(kevent_timer_periodic_to_oneshot, ctx);
#endif
    test(kevent_timer_disable_and_enable, ctx);
/*
 * NetBSD doesn't disable the timer knote when EV_DISPATCH fires;
 * OpenBSD delivers accumulated ticks immediately on re-enable.
 * Both deviate enough that the test can't pass on either platform.
 */
#if defined(EV_DISPATCH) && !defined(__NetBSD__) && !defined(__OpenBSD__)
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
#if defined(NOTE_ABSOLUTE) && !defined(__APPLE__)
    test(kevent_timer_note_absolute, ctx);
    test(kevent_timer_note_absolute_after_modify, ctx);
#endif
/*
 * NetBSD kern_event.c filt_timermodify does kn->kn_flags = kev->flags,
 * replacing the flags word completely (github.com/NetBSD/src
 * sys/kern/kern_event.c:1536).  EV_RECEIPT set on the original EV_ADD
 * is lost when a subsequent modify omits it.  OpenBSD's filt_timermodify
 * calls knote_assign(), which only updates sfflags/sdata/udata and
 * leaves kn_flags intact (github.com/openbsd/src
 * sys/kern/kern_event.c:746,2340), so EV_RECEIPT survives there.
 */
#if !defined(__NetBSD__)
    test(kevent_timer_modify_preserves_ev_receipt, ctx);
#endif
    test(kevent_timer_modify_clobbers_udata, ctx);
}
