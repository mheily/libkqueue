/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
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

#ifndef  _KQUEUE_POSIX_PLATFORM_H
#define  _KQUEUE_POSIX_PLATFORM_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../common/queue.h"

/*
 * Per-caller in-flight tracker for KEVENT_WAIT_DROP_LOCK.  Each
 * kevent() caller stack-allocates one of these, links it on
 * kq->kq_inflight under kq_mtx in kqueue_kevent_enter, and unlinks
 * in kqueue_kevent_exit.  Defined here (not in private.h) because
 * the TAILQ macros in platform_ext.h's KQUEUE_PLATFORM_SPECIFIC
 * need the tag visible.
 */
struct kqueue_kevent_state {
    TAILQ_ENTRY(kqueue_kevent_state) entry;
};

#include "platform_ext.h"

void    posix_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state);
void    posix_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state);
#define kqueue_kevent_enter(_kq, _state) posix_kevent_enter((_kq), (_state))
#define kqueue_kevent_exit(_kq, _state)  posix_kevent_exit((_kq), (_state))

#define KEVENT_WAIT_DROP_LOCK 1

#define EVENTFD_PLATFORM_SPECIFIC	POSIX_EVENTFD_PLATFORM_SPECIFIC
#define KNOTE_PROC_PLATFORM_SPECIFIC	POSIX_KNOTE_PROC_PLATFORM_SPECIFIC
#define PROC_PLATFORM_SPECIFIC		POSIX_PROC_PLATFORM_SPECIFIC
#define FILTER_PLATFORM_SPECIFIC	POSIX_FILTER_PLATFORM_SPECIFIC
#define KQUEUE_PLATFORM_SPECIFIC	POSIX_KQUEUE_PLATFORM_SPECIFIC
#define KNOTE_PLATFORM_SPECIFIC		POSIX_KNOTE_PLATFORM_SPECIFIC

/*
 * C11 atomic operations
 */
#define atomic_inc(p)                 (atomic_fetch_add((p), 1) + 1)
#define atomic_dec(p)                 (atomic_fetch_sub((p), 1) - 1)

/* We use compound literals here to stop the 'expected' values from being overwritten */
#define atomic_cas(p, oval, nval)     atomic_compare_exchange_strong(p, &(__typeof__(oval)){ oval }, nval)
#define atomic_ptr_cas(p, oval, nval) atomic_compare_exchange_strong(p, (&(uintptr_t){ (uintptr_t)oval }), (uintptr_t)nval)
#define atomic_ptr_swap(p, nval)      atomic_exchange(p, (uintptr_t)nval)
#define atomic_ptr_load(p)            atomic_load(p)

void    posix_kqueue_free(struct kqueue *);
int     posix_kqueue_init(struct kqueue *);

int     posix_kevent_wait(struct kqueue *, int, const struct timespec *);
int     posix_kevent_copyout(struct kqueue *, int, struct kevent *, int);

int     posix_eventfd_register(struct kqueue *, struct eventfd *);
void    posix_eventfd_unregister(struct kqueue *, struct eventfd *);

void    posix_wake_kqueue(struct kqueue *kq);

long    posix_timer_min_deadline_ns(struct kqueue *kq);
void    posix_timer_check(struct kqueue *kq);

#endif  /* ! _KQUEUE_POSIX_PLATFORM_H */
