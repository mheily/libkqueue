/*
 * Copyright (c) 2025 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

struct trigger_args {
    int         kqfd;
    uintptr_t   ident;
};

static void *
close_kqueue(void *arg)
{
	/* Sleep until we're fairly sure the other thread is waiting */
	sleep(1);

    /* Close the waiting kqueue from this thread */
    if (close(*((int *)arg)) != 0)
		die("close failed");

    return NULL;
}

static void
test_kevent_threading_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    pthread_t th;
	int kqfd = kqueue();

    /* Add the event, then trigger it from another thread */
    kevent_add(kqfd, &kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);

    if (pthread_create(&th, NULL, close_kqueue, &kqfd) != 0)
        die("failed creating thread");

	/* Wait for event (should be interrupted by close) */
	if (kevent(kqfd, NULL, 0, ret, 1, NULL) == -1) {
		if (errno != EBADF)
			die("kevent failed with the wrong errno");
	} else {
		die("kevent did not fail");
	}

	/* Subsequent calls should also fail with EBADF */
	if (kevent(kqfd, NULL, 0, ret, 1, NULL) == -1) {
		if (errno != EBADF)
			die("kevent failed with the wrong errno (second call)");
	} else {
		die("kevent did not fail (second call)");
	}

    if (pthread_join(th, NULL) != 0)
        die("pthread_join failed");
}

void
test_threading(struct test_context *ctx)
{
#ifdef NATIVE_KQUEUE
	test(kevent_threading_close, ctx);
#endif
}
