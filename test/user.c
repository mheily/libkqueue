/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

static void
test_kevent_user_add_and_delete(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_user_get(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    /* Add the event, and then trigger it */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);

    /* Tear down so later tests that re-register ident=1 start from
     * a clean slate.  BSD does not refresh the flag word on re-EV_ADD,
     * so a residual EV_CLEAR knote leaking out of this test would
     * mask EV_ONESHOT/EV_DISPATCH re-declarations in later tests. */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_user_get_hires(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];
    struct timespec timeo = {
        .tv_sec = 0,
        .tv_nsec = 500000
    };

    test_no_kevents(ctx->kqfd);

    /* Add the event, and then trigger it */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get_hires(ret, NUM_ELEMENTS(ret), ctx->kqfd, &timeo);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_user_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    /*
     * EV_CLEAR explicit.  An earlier comment claimed it was "set
     * automatically by kqueue" but neither BSD nor libkqueue actually
     * does that for EVFILT_USER - the test only passed because of
     * residual EV_CLEAR state leaked from preceding tests in the same
     * run.  Set it explicitly so the test stands alone.
     */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DISABLE, 0, 0, NULL);

    /* Trigger the event, but since it is disabled, nothing will happen. */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_ENABLE, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /*
     * Tear down so later tests that re-register ident=1 start from
     * a clean slate.  Historically this test left the knote alive
     * and later tests relied on that residual state, which masked
     * test-ordering bugs (e.g. re-EV_ADD not merging EV_DISPATCH on
     * macOS).  Keep each test hermetic.
     */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_user_oneshot(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 2, EVFILT_USER, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* EV_ONESHOT auto-deletes on fire so no explicit cleanup needed,
     * but assert we really did see it go.  test_no_kevents is the
     * post-condition. */
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_user_multi_trigger_merged(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];
    int i;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 2, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    for (i = 0; i < 10; i++)
        kevent_add(ctx->kqfd, &tmp, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);

    /* Tear down: BSD does not refresh the flag word on re-EV_ADD, so
     * leaving an EV_CLEAR knote behind would mask EV_ONESHOT in tests
     * run after this one (e.g. across iterations under -n N). */
    kevent_add(ctx->kqfd, &tmp, 2, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

#ifdef EV_DISPATCH
/** Assert that EV_DISPATCH is a durable knote attribute
 *
 *  BSD kqueue treats EV_DISPATCH as sticky - once set at EV_ADD
 *  time it survives every subsequent kevent() operation (bare
 *  NOTE_TRIGGER, EV_DISABLE/EV_ENABLE cycles, EV_ENABLE with
 *  NOTE_TRIGGER) and keeps being reported in the returned
 *  `struct kevent` until EV_DELETE.
 *
 *  Pre-fix libkqueue silently cleared EV_DISPATCH out of the
 *  stored knote on any modify whose src->flags didn't include
 *  EV_DISPATCH, so every second fire returned with the bit clear.
 */
void
test_kevent_user_dispatch_durable(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR | EV_DISPATCH, 0, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;

    /* 1) bare NOTE_TRIGGER - EV_DISPATCH must survive */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_DISPATCH))
        die("EV_DISPATCH lost after bare NOTE_TRIGGER");
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* 2) EV_ENABLE | NOTE_TRIGGER atomic re-arm - must still survive */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_DISPATCH))
        die("EV_DISPATCH lost after EV_ENABLE|NOTE_TRIGGER");
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* 3) explicit EV_DISABLE then EV_ENABLE cycle - must still survive */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DISABLE, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_ENABLE, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_DISPATCH))
        die("EV_DISPATCH lost after EV_DISABLE/EV_ENABLE cycle");
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/** Exercise `EV_ENABLE | NOTE_TRIGGER` in a single kevent()
 *
 *  After an `EV_DISPATCH` knote has fired (and been auto-disabled),
 *  a common idiom is to re-arm and re-signal it atomically with a
 *  single `kevent(EV_ENABLE | NOTE_TRIGGER)` call.  That shape is
 *  used by cross-thread wakeups where the producer doesn't want two
 *  syscalls per signal.
 *
 *  Prior to the EV_ENABLE fallthrough fix, the EV_ENABLE branch in
 *  `kevent_copyin_one` short-circuited and `kn_modify` was never
 *  called, so `NOTE_TRIGGER` was silently dropped and the waiter
 *  never woke.
 */
void
test_kevent_user_dispatch_enable_and_trigger_atomic(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    /* Add an EV_DISPATCH knote and fire it once so it auto-disables */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR | EV_DISPATCH, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Auto-disabled after the dispatch fire */
    test_no_kevents(ctx->kqfd);

    /*
     * Re-arm the auto-disabled knote and signal it in a single
     * kevent() call.  Consumers (e.g. cross-thread wakeup code paths)
     * rely on this to avoid two syscalls per signal.
     *
     * Pre-fix libkqueue took the EV_ENABLE branch and short-circuited
     * before kn_modify ran, dropping NOTE_TRIGGER on the floor and
     * leaving the waiter stuck here.
     */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);

    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

void
test_kevent_user_dispatch(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);

    /* Add the event, and then trigger it */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR | EV_DISPATCH, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    /*
     * Retrieve one event.  EV_DISPATCH is a durable knote attribute:
     * BSD kqueue reports it in every returned kevent until EV_DELETE,
     * so expect it preserved here.
     */
    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Confirm that the knote is disabled automatically */
    test_no_kevents(ctx->kqfd);

    /* Re-enable the kevent */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_ENABLE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    /* Trigger the event */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* Delete the watch */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}
#endif     /* EV_DISPATCH */

struct trigger_args {
    int         kqfd;
    uintptr_t   ident;
};

static void *
trigger_user_event_thread(void *arg)
{
    struct trigger_args *ta = arg;
    struct kevent tmp;

    /* Trigger the user event from this thread */
    kevent_add(ta->kqfd, &tmp, ta->ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    return NULL;
}

static void
test_kevent_user_trigger_from_thread(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    pthread_t th;
    struct trigger_args args;

    test_no_kevents(ctx->kqfd);

    /* Use a distinct ident to avoid confusion with other tests */
    args.kqfd = ctx->kqfd;
    args.ident = 3;

    /* Add the event, then trigger it from another thread */
    kevent_add(ctx->kqfd, &kev, args.ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    if (pthread_create(&th, NULL, trigger_user_event_thread, &args) != 0)
        die("failed creating thread");

    if (pthread_join(th, NULL) != 0)
        die("pthread_join failed");

    /* Prepare expected event (mask out control bits) */
    kev.fflags &= ~NOTE_FFCTRLMASK;
    kev.fflags &= ~NOTE_TRIGGER;

    /* Fetch and compare */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);
}

/*
 * Flag-behaviour tests.
 */

/*
 * EV_DISABLE drops pending events on EVFILT_USER: a NOTE_TRIGGER
 * issued before EV_DISABLE must not be delivered after the disable.
 */
static void
test_kevent_user_disable_drains(struct test_context *ctx)
{
    struct kevent kev, tmp;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    /* Disable BEFORE draining the trigger. */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DISABLE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * EV_DELETE drops pending events on EVFILT_USER: a NOTE_TRIGGER
 * issued before EV_DELETE must not surface after.  Also exercises
 * the freed-knote-on-pending-list class of UAF (the analogous
 * EVFILT_PROC bug we fixed in posix/proc.c).
 */
static void
test_kevent_user_delete_drains(struct test_context *ctx)
{
    struct kevent kev, tmp;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * EV_RECEIPT is sticky on BSD: a knote registered with the bit reports
 * it on every subsequent event until EV_DELETE.  Verifies the
 * preserve-EV_RECEIPT branch in EVFILT_USER's kn_modify isn't wiped
 * by a bare NOTE_TRIGGER.
 */
static void
test_kevent_user_receipt_preserved(struct test_context *ctx)
{
    struct kevent kev, tmp, receipt, ret[1];

    test_no_kevents(ctx->kqfd);

    /* EV_RECEIPT on registration: the kevent() call returns immediately
     * with EV_ERROR | EV_RECEIPT as the receipt confirmation. */
    EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_RECEIPT | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, &receipt, 1, NULL) != 1)
        die("expected EV_RECEIPT confirmation");

    /* Bare NOTE_TRIGGER modify; must not clobber EV_RECEIPT. */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_RECEIPT))
        die("EV_RECEIPT lost after bare NOTE_TRIGGER, flags=0x%x", ret[0].flags);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * BSD overwrites kn->kev.udata on every successful modify, even on a
 * bare NOTE_TRIGGER with udata=NULL.  Verifies the udata-clobber line
 * in common/kevent.c (line ~303) actually behaves that way end-to-end.
 */
static void
test_kevent_user_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];
    int           marker = 0xabc;

    test_no_kevents(ctx->kqfd);

    /* Register with non-NULL udata. */
    kevent_add(ctx->kqfd, &kev, 1, EVFILT_USER,
               EV_ADD | EV_CLEAR, 0, 0, &marker);

    /* Bare NOTE_TRIGGER passing udata=NULL must overwrite. */
    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);

    kevent_add(ctx->kqfd, &tmp, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * EV_DELETE on a never-registered ident returns ENOENT.
 */
static void
test_kevent_user_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 999, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added knote should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));
}

/*
 * udata round-trips through delivery.
 */
static void
test_kevent_user_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];
    void         *marker = (void *) 0xBEEFDEADUL;

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 50, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* TRIGGER without overriding udata.  BSD's modify clobbers
     * udata on every modify, but the kevent_add helper passes NULL,
     * so use EV_SET directly with the same marker to avoid clobber. */
    EV_SET(&tmp, 50, EVFILT_USER, 0, NOTE_TRIGGER, 0, marker);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("kevent trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p expected %p", ret[0].udata, marker);

    EV_SET(&tmp, 50, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * Two kqueues with the same EVFILT_USER ident: independent state.
 * Trigger on kq1 must not surface on kq2.
 */
static void
test_kevent_user_multi_kqueue(struct test_context *ctx)
{
    struct kevent kev, tmp;
    int           kq2;

    if ((kq2 = kqueue()) < 0) die("kqueue(2)");

    EV_SET(&kev, 51, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent kq1");
    EV_SET(&kev, 51, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kq2, &kev, 1, NULL, 0, NULL) < 0) die("kevent kq2");

    /* Trigger only on kq1. */
    EV_SET(&tmp, 51, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    /* kq1 fires. */
    {
        struct kevent ret[1];
        kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    }
    /* kq2 must NOT fire (state per-kqueue). */
    {
        struct timespec poll = { 0, 50 * 1000000 };
        struct kevent   ret[1];
        if (kevent(kq2, NULL, 0, ret, 1, &poll) != 0)
            die("kq2 fired but trigger only went to kq1");
    }

    EV_SET(&tmp, 51, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
    close(kq2);
}

/*
 * NOTE_FFCOPY: writer's low fflags REPLACE the stored fflags.
 */
static void
test_kevent_user_note_ffcopy(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 60, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_FFCOPY | 0xAA, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* Trigger with FFCOPY|0xBB|TRIGGER: stored fflags becomes 0xBB. */
    EV_SET(&tmp, 60, EVFILT_USER, 0, NOTE_FFCOPY | 0xBB | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != 0xBB)
        die("NOTE_FFCOPY: expected fflags 0xBB, got 0x%x",
            ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 60, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * NOTE_FFAND: writer's low fflags ANDed into stored fflags.
 */
static void
test_kevent_user_note_ffand(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    /* Seed via FFCOPY 0x0F. */
    EV_SET(&kev, 61, EVFILT_USER, EV_ADD | EV_CLEAR,
           NOTE_FFCOPY | 0x0F, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent seed");

    /* AND with 0x0C and TRIGGER: stored becomes 0x0F & 0x0C = 0x0C. */
    EV_SET(&tmp, 61, EVFILT_USER, 0, NOTE_FFAND | 0x0C | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != 0x0C)
        die("NOTE_FFAND: expected 0x0C, got 0x%x",
            ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 61, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * NOTE_FFOR: writer's low fflags ORed into stored fflags.
 */
static void
test_kevent_user_note_ffor(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 62, EVFILT_USER, EV_ADD | EV_CLEAR,
           NOTE_FFCOPY | 0x01, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent seed");

    /* OR with 0x06 and TRIGGER: stored becomes 0x01 | 0x06 = 0x07. */
    EV_SET(&tmp, 62, EVFILT_USER, 0, NOTE_FFOR | 0x06 | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != 0x07)
        die("NOTE_FFOR: expected 0x07, got 0x%x",
            ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 62, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * NOTE_FFNOP: writer's low fflags ignored, stored unchanged.
 */
static void
test_kevent_user_note_ffnop(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 63, EVFILT_USER, EV_ADD | EV_CLEAR,
           NOTE_FFCOPY | 0x42, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent seed");

    /* FFNOP|0xFF|TRIGGER: 0xFF must be ignored, stored stays 0x42. */
    EV_SET(&tmp, 63, EVFILT_USER, 0, NOTE_FFNOP | 0xFF | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != 0x42)
        die("NOTE_FFNOP: expected 0x42, got 0x%x",
            ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 63, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * NOTE_FFLAGSMASK enforcement: writer's high bits (incl. NOTE_TRIGGER
 * itself) must not leak into stored fflags via FFCOPY.
 */
static void
test_kevent_user_note_fflagsmask(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 64, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* Try to copy 0x01ffffff: low 24 bits (0xffffff) survive,
     * NOTE_TRIGGER (bit 24) and high control bits do not. */
    EV_SET(&tmp, 64, EVFILT_USER, 0,
           NOTE_FFCOPY | 0x01ffffff | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & ~NOTE_FFLAGSMASK) != 0)
        die("NOTE_FFLAGSMASK leak: high bits in delivered fflags 0x%x",
            ret[0].fflags);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != NOTE_FFLAGSMASK)
        die("expected delivered low bits 0x%x, got 0x%x",
            NOTE_FFLAGSMASK, ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 64, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

/*
 * Multi-trigger fflags accumulation: three FFOR triggers between
 * drains deliver the unioned bits in a single coalesced event.
 */
static void
test_kevent_user_multi_trigger_fflags_or(struct test_context *ctx)
{
    struct kevent kev, tmp, ret[1];

    test_no_kevents(ctx->kqfd);
    EV_SET(&kev, 65, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* Three OR triggers: 0x1, 0x2, 0x4. */
    EV_SET(&tmp, 65, EVFILT_USER, 0, NOTE_FFOR | 0x1 | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger 1");
    EV_SET(&tmp, 65, EVFILT_USER, 0, NOTE_FFOR | 0x2 | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger 2");
    EV_SET(&tmp, 65, EVFILT_USER, 0, NOTE_FFOR | 0x4 | NOTE_TRIGGER, 0, NULL);
    if (kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL) < 0) die("trigger 3");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if ((ret[0].fflags & NOTE_FFLAGSMASK) != 0x07)
        die("multi-trigger OR accumulation: expected 0x07, got 0x%x",
            ret[0].fflags & NOTE_FFLAGSMASK);

    EV_SET(&tmp, 65, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &tmp, 1, NULL, 0, NULL);
}

void
test_evfilt_user(struct test_context *ctx)
{
    test(kevent_user_add_and_delete, ctx);
    test(kevent_user_get, ctx);
    test(kevent_user_get_hires, ctx);
    test(kevent_user_disable_and_enable, ctx);
    /* Flag-behaviour group */
    test(kevent_user_disable_drains, ctx);
    test(kevent_user_delete_drains, ctx);
    test(kevent_user_receipt_preserved, ctx);
    test(kevent_user_modify_clobbers_udata, ctx);
    test(kevent_user_oneshot, ctx);
    test(kevent_user_multi_trigger_merged, ctx);
    test(kevent_user_del_nonexistent, ctx);
    test(kevent_user_udata_preserved, ctx);
    test(kevent_user_multi_kqueue, ctx);
    test(kevent_user_note_ffcopy, ctx);
    test(kevent_user_note_ffand, ctx);
    test(kevent_user_note_ffor, ctx);
    test(kevent_user_note_ffnop, ctx);
    test(kevent_user_note_fflagsmask, ctx);
    test(kevent_user_multi_trigger_fflags_or, ctx);
#ifdef EV_DISPATCH
    test(kevent_user_dispatch, ctx);
    test(kevent_user_dispatch_durable, ctx);
    test(kevent_user_dispatch_enable_and_trigger_atomic, ctx);
#endif
    test(kevent_user_trigger_from_thread, ctx);
    /* TODO: try different fflags operations */
}
