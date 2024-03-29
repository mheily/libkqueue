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

    kevent_add(ctx->kqfd, &kev, SIGUSR1, EVFILT_SIGNAL, EV_ENABLE, 0, 0, NULL);

    if (kill(getpid(), SIGUSR1) < 0)
        die("kill");

    kev.flags = EV_ADD | EV_CLEAR;
#if LIBKQUEUE
    kev.data = 1; /* WORKAROUND */
#else
    kev.data = 2; // one extra time from test_kevent_signal_disable()
#endif
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Delete the watch */
    kev.flags = EV_DELETE;
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

void
test_evfilt_signal(struct test_context *ctx)
{
    signal(SIGUSR1, SIG_IGN);

    test(kevent_signal_add, ctx);
    test(kevent_signal_del, ctx);
    test(kevent_signal_get, ctx);
    test(kevent_signal_get_pending, ctx);
    test(kevent_signal_disable, ctx);
    test(kevent_signal_enable, ctx);
    test(kevent_signal_oneshot, ctx);
    test(kevent_signal_modify, ctx);
#ifdef EV_DISPATCH
    test(kevent_signal_dispatch, ctx);
#endif
}
