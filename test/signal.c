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

#include <time.h>

#include "common.h"

void
test_kevent_signal_add(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
}

void
test_kevent_signal_get(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
}

void
test_kevent_signal_get_pending(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* A pending signal should be reported as soon as the event is added */
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
}

void
test_kevent_signal_disable(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_DISABLE, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    test_no_kevents(ctx->kqfd);
}

void
test_kevent_signal_enable(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct timespec ts = { 2, 0 };
    int total = 0;

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ENABLE, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    /*
     * Two fires happened: one in test_kevent_signal_disable while
     * the knote was disabled, one from the kill() above.  Native
     * BSD/macOS hook EVFILT_SIGNAL into the signal-generation path
     * so each fire bumps kev.data atomically before the kernel's
     * per-process pending bitmap coalesces - they always report
     * total=2 in one event.
     *
     * libkqueue's signalfd-backed dispatcher reads from the kernel
     * pending queue, where non-RT signals coalesce.  Whether we
     * see 1 or 2 fires depends on dispatcher scheduling: if it
     * drains the first kill before the second arrives, both are
     * accounted (total=2, possibly across multiple events); if
     * not, the second kill collides with the first in the pending
     * bitmap and signalfd returns one siginfo (total=1).  Both
     * are correct for the platform.
     *
     * Drain in a loop to accumulate any events the dispatcher
     * produces, then assert against the platform's expected lower
     * bound.
     */
    while (total < 2) {
        int rv = kevent_get_timeout(ret, 1, ctx->kqfd, &ts);
        if (rv <= 0)
            break;
        total += ret[0].data;
    }
#ifdef NATIVE_KQUEUE
    if (total != 2)
        errx(1, "expected total=2 fires, got %d", total);
#else
    if (total < 1 || total > 2)
        errx(1, "expected total in [1,2] fires, got %d", total);
#endif

    /* Delete the watch */
    kev.ident  = SIGUSR1;
    kev.filter = EVFILT_SIGNAL;
    kev.flags  = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

void
test_kevent_signal_del(struct test_context *ctx)
{
    struct kevent kev;

    /* Delete the kevent */
    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);

    signal(SIGUSR1, SIG_IGN);
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    test_no_kevents(ctx->kqfd);
}

void
test_kevent_signal_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_ONESHOT, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Send another one and make sure we get no events */
    test_no_kevents(ctx->kqfd);
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    test_no_kevents(ctx->kqfd);
}

void
test_kevent_signal_modify(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, ((void *)-1));

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_kevent_signal_del(ctx);
}

#ifdef EV_DISPATCH
void
test_kevent_signal_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    /* EV_CLEAR should be set internally */
    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_DISPATCH, 0, 0, NULL);

    /* Get one event */
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Generate a pending signal, this should get delivered once the filter is enabled again */
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    test_no_kevents(ctx->kqfd);

    /* Enable the knote, our pending signal should now get delivered */
    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ENABLE | EV_DISPATCH, 0, 0, NULL);

    kev.flags ^= EV_ENABLE;
    kev.flags |= EV_ADD;
    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Remove the knote and ensure the event no longer fires */
    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    test_no_kevents(ctx->kqfd);
}
#endif  /* EV_DISPATCH */

/*
 * Distinct signums on a single kqueue.  Each kill should fire
 * only the matching knote.  Catches dispatcher fan-out bugs that
 * cross-wire signum routing via the wrong sig_link.
 */
void
test_kevent_signal_multi_signum(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    signal(SIGUSR2, SIG_IGN);

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, SIGUSR2, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

    /* kill SIGUSR1 -> only the SIGUSR1 knote should fire */
    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");
    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 1, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* kill SIGUSR2 -> only the SIGUSR2 knote should fire */
    if (kill(getpid(), SIGUSR2) < 0)
        die("kill");
    EV_SET(&kev, SIGUSR2, EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 1, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* clean up both */
    kev.flags = EV_DELETE;
    kev.ident = SIGUSR1;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kev.ident = SIGUSR2;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

/*
 * Two kqueues each watching the same signum.  A single kill must
 * be observed by both - on libkqueue's signalfd backend this
 * exercises the global-dispatcher fan-out that replaced the old
 * per-knote signalfd model (where two signalfds would race for
 * the kernel's dequeue_signal and the loser saw nothing).
 *
 * Skipped on the POSIX backend: the select(2)-based dispatcher
 * does not fan a single signal out to multiple kqueues yet.
 */
void
test_kevent_signal_multi_kqueue(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int kqfd2;

    if ((kqfd2 = kqueue()) < 0)
        die("kqueue");

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    kevent_add(kqfd2,     &kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags |= EV_CLEAR;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    kevent_get(ret, NUM_ELEMENTS(ret), kqfd2, 1);
    kevent_cmp(&kev, ret);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_rv_cmp(0, kevent(kqfd2,     &kev, 1, NULL, 0, NULL));
    close(kqfd2);
}

/*
 * RT signals queue per-fire (vs the per-process pending-bit
 * coalescing for non-RT), so kev.data should reflect actual
 * delivery counts once the dispatcher has drained.
 *
 * Late-register variant: register kqueue A, queue one fire,
 * register kqueue B, queue another fire.  A should observe
 * both fires (count=2 across one or more events); B should
 * observe only the second fire (count=1).  Confirms that B
 * doesn't retroactively see fires that landed before its
 * registration was visible to the dispatcher.
 *
 * The dispatcher races our kevent_get(), so a single fire may
 * arrive split across multiple events - drain in a loop until
 * the running total hits the expected count.
 */
#ifdef SIGRTMIN
static void
rt_drain(int kqfd, int sig, int expected)
{
    struct kevent ret[1];
    struct timespec ts = { 2, 0 };
    int total = 0;

    while (total < expected) {
        int rv = kevent_get_timeout(ret, 1, kqfd, &ts);
        if (rv <= 0)
            break;
        if (ret[0].ident != (uintptr_t) sig || ret[0].filter != EVFILT_SIGNAL)
            errx(1, "unexpected event: ident=%lu filter=%d",
                 (unsigned long) ret[0].ident, ret[0].filter);
        total += ret[0].data;
    }

    if (total != expected)
        errx(1, "kqfd=%d signal=%d expected %d fires, got %d",
             kqfd, sig, expected, total);
}

void
test_kevent_signal_rt_late_register(struct test_context *ctx)
{
    struct kevent kev;
    union sigval sv;
    sigset_t mask;
    int kqfd2;
    int sig = SIGRTMIN + 4;

    memset(&sv, 0, sizeof(sv));

    /*
     * Block the signal process-wide.  libkqueue's signalfd backend
     * already does this via catch_signal, but native BSD/macOS
     * EVFILT_SIGNAL fires the kqueue knote alongside normal signal
     * delivery - which for an RT signal with no handler installed
     * runs the default action (terminate the process) before the
     * test can read the kevent.  Blocking keeps the signal pending
     * so EVFILT_SIGNAL can fire without the disposition running.
     */
    sigemptyset(&mask);
    sigaddset(&mask, sig);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        die("sigprocmask");

    if ((kqfd2 = kqueue()) < 0)
        die("kqueue");

    /* A registers, then fire 1 lands.  Drain A to confirm fire 1
     * was observed - this also synchronizes us with the dispatcher,
     * since kevent_get blocks until the dispatcher has fanned the
     * fire onto A's pending list.  Without this drain, kn_create
     * for B and sig_dispatch_handle for fire 1 could win the
     * sigtbl_mtx race in either order, and B might see fire 1. */
    kevent_add(ctx->kqfd, &kev, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (sigqueue(getpid(), sig, sv) < 0)
        die("sigqueue");
    rt_drain(ctx->kqfd, sig, 1);

    /* Now register B with fire 1 already drained, then fire 2. */
    kevent_add(kqfd2, &kev, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (sigqueue(getpid(), sig, sv) < 0)
        die("sigqueue");

    /* A sees fire 2, B sees fire 2.  Cumulatively across the test
     * A observed 2 fires and B observed 1, with B never having
     * seen fire 1. */
    rt_drain(ctx->kqfd, sig, 1);
    rt_drain(kqfd2,     sig, 1);

    kev.ident  = sig;
    kev.filter = EVFILT_SIGNAL;
    kev.flags  = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_rv_cmp(0, kevent(kqfd2,     &kev, 1, NULL, 0, NULL));
    close(kqfd2);
}
#endif

/*
 * Flag-behaviour tests.
 *
 * EV_DISABLE/EV_DELETE pending-drain tests aren't included here:
 * the dispatcher thread populates sfs_pending asynchronously after
 * kill(), and there's no in-API barrier that says "dispatcher has
 * processed signal X" without also draining it.  Determinism would
 * need a test-only hook into the dispatcher.
 */

/*
 * BSD overwrites kn->kev.udata on every modify.  Verifies the signal
 * filter's modify (which only updates flags) still lets common code
 * clobber udata.
 */
void
test_kevent_signal_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           marker = 0xab;

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, &marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) != 0)
        die("kevent add failed");

    /* Re-EV_ADD with udata=NULL. */
    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) != 0)
        die("kevent modify failed");

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

/*
 * EV_RECEIPT is sticky on BSD: a knote registered with the bit
 * reports it on every subsequent event until EV_DELETE.  Verifies
 * the (kn->kev.flags & EV_RECEIPT) preserve term in the signal
 * filter's kn_modify isn't dropped.
 */
void
test_kevent_signal_receipt_preserved(struct test_context *ctx)
{
    struct kevent kev, receipt, ret[1];

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_RECEIPT, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, &receipt, 1, NULL) != 1)
        die("expected EV_RECEIPT confirmation");

    /* Re-EV_ADD modify without EV_RECEIPT - sticky bit must survive. */
    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) != 0)
        die("kevent modify failed");

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_RECEIPT))
        die("EV_RECEIPT lost across modify, flags=0x%x", ret[0].flags);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

/*
 * EV_DELETE on a never-registered signum returns ENOENT.
 */
static void
test_kevent_signal_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, SIGUSR2, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on non-existent signal knote should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));
}

/*
 * udata round-trips through delivery.
 */
static void
test_kevent_signal_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    void         *marker = (void *) 0xC0FFEEUL;

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGUSR1) < 0) die("kill");
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p expected %p", ret[0].udata, marker);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
}

/*
 * libkqueue rejects signum 0 and out-of-range signums on EV_ADD.
 * Native BSD/macOS don't validate ident at registration time -
 * a knote with a bogus signum registers cleanly and never fires
 * (kill() can't generate the signal), so the test would surface
 * a non-bug there.  Gate to libkqueue (POSIX/Linux backends).
 */
#if !defined(NATIVE_KQUEUE)
static void
test_kevent_signal_invalid_signum_rejected(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, 0, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("signum 0 should reject");
    if (errno != EINVAL)
        die("signum 0: expected EINVAL, got %d (%s)", errno, strerror(errno));

    EV_SET(&kev, 9999, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("signum 9999 should reject");
    if (errno != EINVAL)
        die("signum 9999: expected EINVAL, got %d (%s)", errno, strerror(errno));
}
#endif

/*
 * EV_CLEAR resets kev.data to 0 after delivery: a second drain
 * with no further fires must return 0.
 */
static void
test_kevent_signal_ev_clear_resets_data(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGUSR1) < 0) die("kill");
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* No further fires; EV_CLEAR must zero the accumulator. */
    test_no_kevents(ctx->kqfd);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
}

/*
 * pthread_kill(self) must fire the knote: BSD's signal-generation
 * path posts to the proc-level klist regardless of the source
 * thread.
 */
#include <pthread.h>
static void
test_kevent_signal_pthread_kill_self(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (pthread_kill(pthread_self(), SIGUSR1) != 0)
        die("pthread_kill");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != SIGUSR1)
        die("expected SIGUSR1, got %llu", (unsigned long long) ret[0].ident);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
}

/*
 * Signal blocked via sigprocmask still fires the knote: BSD posts
 * the knote in the signal-generation path before mask check.  The
 * blocked signal stays pending in the proc bitmap, but EVFILT_SIGNAL
 * is independent.
 */
static void
test_kevent_signal_fires_while_blocked(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    sigset_t      block, oldmask;

    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0) die("sigmask");

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGUSR1) < 0) die("kill");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != SIGUSR1)
        die("blocked-signal: knote didn't fire");

    /* Unmask before drain to let any pending delivery complete. */
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
}

/*
 * EVFILT_SIGNAL fires on SIGCONT just like any normal signal.
 * BSD's KNOTE() runs in the signal-generation path before the
 * stop/cont special-case, so even signals the kernel handles
 * specially still notify kqueue.
 */
static void
test_kevent_signal_sigcont(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    signal(SIGCONT, SIG_IGN);

    EV_SET(&kev, SIGCONT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGCONT) < 0) die("kill(SIGCONT)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != SIGCONT)
        die("expected SIGCONT, got %llu", (unsigned long long) ret[0].ident);
    if (ret[0].data < 1)
        die("expected data >= 1, got %lld", (long long) ret[0].data);

    EV_SET(&kev, SIGCONT, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
    signal(SIGCONT, SIG_DFL);
}

/*
 * EV_DISABLE drops a pending signal-fire (drains).  Distinct from
 * the existing test_kevent_signal_disable which just tests that
 * the knote suppresses delivery while in the disabled state.
 */
static void
test_kevent_signal_disable_drains(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGUSR1) < 0) die("kill");

    /* Disable before drain. */
    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DISABLE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent disable");
    test_no_kevents(ctx->kqfd);

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
}

/*
 * EV_DELETE drops a pending fire.
 */
static void
test_kevent_signal_delete_drains(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (kill(getpid(), SIGUSR1) < 0) die("kill");

    EV_SET(&kev, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent delete");
    test_no_kevents(ctx->kqfd);
}

void
test_evfilt_signal(struct test_context *ctx)
{
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);

    test(kevent_signal_add, ctx);
    test(kevent_signal_del, ctx);
    test(kevent_signal_del_nonexistent, ctx);
    test(kevent_signal_udata_preserved, ctx);
#if !defined(NATIVE_KQUEUE)
    test(kevent_signal_invalid_signum_rejected, ctx);
#endif
    test(kevent_signal_ev_clear_resets_data, ctx);
    /*
     * pthread_kill(self) hangs on:
     *  - Linux backend (signalfd-based dispatch may not see
     *    per-thread-targeted signals if the calling thread doesn't
     *    have the signal masked)
     *  - macOS native kqueue (psignal_internal apparently doesn't
     *    fire EVFILT_SIGNAL on pthread_kill-routed signals; test
     *    deadlocks waiting for an event that never arrives)
     * Only FreeBSD's tdksignal -> tdsendsignal path posts to the
     * proc-level klist regardless of source thread.
     */
#if !defined(LIBKQUEUE_BACKEND_LINUX) && !defined(__APPLE__)
    test(kevent_signal_pthread_kill_self, ctx);
#endif
    test(kevent_signal_fires_while_blocked, ctx);
    test(kevent_signal_disable_drains, ctx);
    test(kevent_signal_delete_drains, ctx);
    test(kevent_signal_sigcont, ctx);
    test(kevent_signal_get, ctx);
    test(kevent_signal_get_pending, ctx);
    test(kevent_signal_disable, ctx);
    test(kevent_signal_enable, ctx);
    test(kevent_signal_oneshot, ctx);
    test(kevent_signal_modify, ctx);
/*
 * NetBSD native kqueue doesn't hold signals while a knote is
 * EV_DISPATCH-disabled; the second kill() fires through immediately
 * rather than being buffered for re-enable.
 */
#if defined(EV_DISPATCH) && !defined(__NetBSD__)
    test(kevent_signal_dispatch, ctx);
#endif
    test(kevent_signal_multi_kqueue, ctx);
    test(kevent_signal_multi_signum, ctx);
    test(kevent_signal_receipt_preserved, ctx);
    test(kevent_signal_modify_clobbers_udata, ctx);
    /*
     * RT-signal queueing: the kernel queues N copies of the same
     * RT signum and the test expects the knote to fire N times.
     * The POSIX backend's sigaction + self-pipe layer collapses
     * pending signals to a single fire (one byte per signum gets
     * written to the self-pipe regardless of arrival count), so
     * queueing semantics aren't there yet.  Skip until the
     * dispatcher learns to count.
     */
#ifdef SIGRTMIN
    test(kevent_signal_rt_late_register, ctx);
#endif
}
