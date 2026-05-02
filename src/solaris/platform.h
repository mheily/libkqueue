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

#ifndef  _KQUEUE_SOLARIS_PLATFORM_H
#define  _KQUEUE_SOLARIS_PLATFORM_H

struct filter;

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <stropts.h>
#include <sys/poll.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "../posix/platform_ext.h"

#include "eventfd.h"

/*
 * Solaris/illumos eventfd struct extras.
 *
 * On illumos there is no eventfd(2).  We model "wake the kqueue
 * waiter" as a port_send(3C) into the kqueue's event port, which
 * unblocks port_getn(3C).  efd_events carries the port-source
 * discriminator (see X_PORT_SOURCE_*) so the platform copyout
 * dispatcher can route the wakeup to the right filter.
 */
#define EVENTFD_PLATFORM_SPECIFIC \
    int          efd_events; \
    atomic_uint  efd_raised   /* 0/1; coalesces multiple raises into one wake */

/*
 * C11 atomic operations
 */
#define atomic_inc(p)                 (atomic_fetch_add((p), 1) + 1)
#define atomic_dec(p)                 (atomic_fetch_sub((p), 1) - 1)

/* Compound literal so atomic_compare_exchange_strong's failure-write doesn't clobber the caller's value. */
#define atomic_cas(p, oval, nval)     atomic_compare_exchange_strong(p, &(__typeof__(oval)){ oval }, nval)
#define atomic_ptr_cas(p, oval, nval) atomic_compare_exchange_strong(p, (&(uintptr_t){ (uintptr_t)oval }), (uintptr_t)nval)
#define atomic_ptr_swap(p, nval)      atomic_exchange(p, (uintptr_t)nval)
#define atomic_ptr_load(p)            atomic_load(p)

/*
 * Event ports
 */
#include <port.h>

/*
 * PORT_SOURCE_USER on the main port comes from solaris_eventfd_raise
 * (signal/proc).  portev_events is the originating filter id, used as
 * the filter_lookup key in solaris_kevent_copyout.
 */

/* Convenience macros to access the event port descriptor for the kqueue */
#define kqueue_epoll_fd(kq)     ((kq)->kq_id)
#define filter_epoll_fd(filt)   ((filt)->kf_kqueue->kq_id)

/*
 * Tell common/kevent.c to drop kq->kq_mtx across kevent_wait.  See the
 * deferred-udata-free protocol below for why this is now safe.
 */
#define KEVENT_WAIT_DROP_LOCK   1

void    solaris_kqueue_free(struct kqueue *);
int     solaris_kqueue_init(struct kqueue *);

/*
 * "udata" is passed as port_associate's user pointer; returned via the
 * portev_user field when an event is ready.  It is heap-allocated so it
 * can outlive the knote: a port_getn may return an event whose
 * portev_user points here after another thread has run EV_DELETE and
 * freed the kn.
 *
 * EV_DELETE protocol:
 *   1. Remove the kernel registration (port_dissociate for FD/FILE/
 *      TIMER, close(sub-port) for EVFILT_USER) - no new events queue.
 *   2. Mark ud_stale = true and queue on kq->ud_deferred_free with
 *      the epoch boundary.
 *   3. Free only once every kevent() caller that could have observed
 *      this ud (entry epoch <= boundary) has exited.
 *
 * Copyout checks ud_stale before reading ud_kn (dangling once stale).
 * Filter-pointer port_sends from solaris_eventfd_raise (signal/proc)
 * don't use port_udata - filter pointers live as long as the kqueue.
 */
struct port_udata {
    struct knote             *ud_kn;            //!< Pointer back to the containing knote.
    bool                     ud_stale;          //!< Set true under kq_mtx by EV_DELETE.
                                                ///< Once ud_stale is set, ud_kn is dangling and
                                                ///< copyout must skip dispatch.
    uint64_t                 ud_boundary_epoch; //!< Highest kevent() entry epoch that could
                                                ///< have observed the udata in its TLS buffer.
    TAILQ_ENTRY(port_udata)  ud_deferred_entry; //!< Entry in kq->ud_deferred_free.
                                                ///< Only valid once ud_stale is set.
};

TAILQ_HEAD(port_udata_head, port_udata);

/*
 * Per-kevent() in-flight tracking.
 *
 * Stack-allocated in the common kevent() entry path; linked into the
 * kqueue's kq_inflight list under kq_mtx for the duration of the
 * kevent() call.  The recorded epoch lets the deferred-free sweep
 * tell whether the caller could still hold a stale udata pointer in
 * its TLS port_event_t buffer.
 */
struct kqueue_kevent_state {
    TAILQ_ENTRY(kqueue_kevent_state) entry;     //!< Entry in kq->kq_inflight.
    uint64_t                         epoch;     //!< Assigned at kevent_enter time.
};

TAILQ_HEAD(kqueue_kevent_state_head, kqueue_kevent_state);

struct port_udata *port_udata_alloc(struct knote *kn);
void port_udata_defer_free(struct kqueue *kq, struct port_udata *u);

void solaris_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state);
void solaris_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state);

#define kqueue_kevent_enter(_kq, _state) solaris_kevent_enter((_kq), (_state))
#define kqueue_kevent_exit(_kq, _state)  solaris_kevent_exit((_kq), (_state))

/** Allocate the kn_udata for a knote.  See @ref port_udata. */
#define KN_UDATA_ALLOC(_kn) ((_kn)->kn_udata = port_udata_alloc(_kn))

/*
 * Immediately free a knote's udata and null the field.
 *
 * Use only on paths where the udata was allocated but never
 * successfully passed to port_associate (e.g. kn_create failure).
 * For the normal teardown path use KN_UDATA_DEFER_FREE.
 */
#define KN_UDATA_FREE(_kn) do { \
    free((_kn)->kn_udata); \
    (_kn)->kn_udata = NULL; \
} while (0)

/** Defer-free a knote's udata and null the field.  See @ref port_udata. */
#define KN_UDATA_DEFER_FREE(_kq, _kn) do { \
    port_udata_defer_free((_kq), (_kn)->kn_udata); \
    (_kn)->kn_udata = NULL; \
} while (0)

/*
 * Data structures
 */
struct event_buf {
    port_event_t pe;
    TAILQ_ENTRY(event_buf) entries;
};

/*
 * Per-knote state for EVFILT_TIMER.  See solaris/timer.c.
 */
#define SOLARIS_KNOTE_TIMER_PLATFORM_SPECIFIC \
    timer_t kn_timerid

/*
 * Per-knote state for EVFILT_USER.
 *
 * kn_user_ctr coalesces NOTE_TRIGGERs: only the 0->1 transition calls
 * port_send; the accumulated count drains on copyout.
 * kn_user_subport is a dedicated port(5) fd that proxies EVFILT_USER
 * wakeups into the platform event loop.  See solaris/user.c.
 */
#define SOLARIS_KNOTE_USER_PLATFORM_SPECIFIC \
    struct { \
        atomic_uint kn_user_ctr; \
        int         kn_user_subport; \
    }

/*
 * Per-knote state for EVFILT_VNODE / PORT_SOURCE_FILE.  kn_vnode_path
 * is heap-owned by the knote (fobj.fo_name aliases it).  See
 * solaris/vnode.c for the protocol.
 */
#define SOLARIS_KNOTE_VNODE_PLATFORM_SPECIFIC \
    struct { \
        struct file_obj kn_vnode_fobj; \
        char           *kn_vnode_path; \
        unsigned int    kn_vnode_events; \
        nlink_t         kn_vnode_nlink; \
        off_t           kn_vnode_size; \
    }

#define KNOTE_PLATFORM_SPECIFIC \
    struct port_udata *kn_udata; /* live for all filter types; remaining fields are per-type and share a union */ \
    union { \
        SOLARIS_KNOTE_TIMER_PLATFORM_SPECIFIC; \
        SOLARIS_KNOTE_USER_PLATFORM_SPECIFIC; \
        POSIX_KNOTE_PROC_PLATFORM_SPECIFIC; \
        SOLARIS_KNOTE_VNODE_PLATFORM_SPECIFIC; \
    }

/*
 * Per-filter platform state.  See union posix_filter_state in
 * src/posix/platform_ext.h for the layout - filters that need
 * platform-specific fields get one named struct member each.
 */
#define FILTER_PLATFORM_SPECIFIC \
    union posix_filter_state kf_state

/*
 * Per-kqueue deferred-free bookkeeping.
 *
 * kq_next_epoch + kq_inflight + ud_deferred_free implement the
 * deferred-free scheme that keeps a port_udata alive across the
 * kevent_wait window.  See @ref port_udata for the full protocol.
 */
#define KQUEUE_PLATFORM_SPECIFIC \
    uint64_t        kq_next_epoch;                    /* Monotonic kevent() entry counter; rebased at UINT64_MAX. */ \
    struct kqueue_kevent_state_head kq_inflight;      /* In-flight kevent() callers; head = oldest. */ \
    struct port_udata_head          ud_deferred_free  /* Stale udatas; head = smallest boundary. */

#endif  /* ! _KQUEUE_SOLARIS_PLATFORM_H */
