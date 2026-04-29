/*
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

/* We use compound literals here to stop the 'expected' values from being overwritten */
#define atomic_cas(p, oval, nval)     atomic_compare_exchange_strong(p, &(__typeof__(oval)){ oval }, nval)
#define atomic_ptr_cas(p, oval, nval) atomic_compare_exchange_strong(p, (&(uintptr_t){ (uintptr_t)oval }), (uintptr_t)nval)
#define atomic_ptr_swap(p, nval)      atomic_exchange(p, (uintptr_t)nval)
#define atomic_ptr_load(p)            atomic_load(p)

/*
 * Event ports
 */
#include <port.h>

/*
 * For PORT_SOURCE_USER wakeups (signal + user filter), portev_events
 * carries the originating filter id (EVFILT_*).  solaris_kevent_copyout
 * does filter_lookup(kq, evt->portev_events) to dispatch.  No parallel
 * X_PORT_SOURCE_* constant set required.
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

/** Common structure passed as port_associate's user pointer (and as
 * port_send's user argument for EVFILT_USER).
 *
 * The kernel returns this pointer via `port_event_t::portev_user`
 * whenever an associated registration becomes ready.  The udata is
 * heap-allocated independently of the containing knote so its
 * lifetime can outlive the knote across the kevent_wait window: a
 * thread may hold a stale portev_user in its TLS port_event_t buffer
 * (the per-thread `evbuf[]` in solaris_kevent_wait) between port_getn
 * returning and solaris_kevent_copyout running, even after another
 * thread has run EV_DELETE on the registration and freed the knote.
 *
 * Once EV_DELETE runs, port_dissociate stops the kernel queuing new
 * events for the udata, but pre-existing ready events may still
 * surface in some other thread's TLS buffer.  EV_DELETE marks the
 * udata stale, queues it on kq->ud_deferred_free with the current
 * epoch as a boundary, and the deferred udata is reclaimed only
 * once every kevent() caller that could have observed the udata
 * (every caller whose entry epoch is <= the boundary) has exited.
 *
 * Copyout always checks ud_stale before dereferencing ud_kn - the
 * back-pointer is dangling once stale is set.
 *
 * Filter-pointer port_sends (EVFILT_SIGNAL via solaris_eventfd_raise)
 * don't go through port_udata: filter pointers live as long as the
 * containing kqueue, no indirection required.
 */
struct port_udata {
    struct knote            *ud_kn;             //!< Pointer back to the containing knote.
    bool                     ud_stale;          //!< Set true under kq_mtx by EV_DELETE.
                                                ///< Once ud_stale is set, ud_kn is dangling and
                                                ///< copyout must skip dispatch.
    uint64_t                 ud_boundary_epoch; //!< Highest kevent() entry epoch that could
                                                ///< have observed the udata in its TLS buffer.
    TAILQ_ENTRY(port_udata)  ud_deferred_entry; //!< Entry in kq->ud_deferred_free.
                                                ///< Only valid once ud_stale is set.
};

TAILQ_HEAD(port_udata_head, port_udata);

/** Per-kevent() in-flight tracking.
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
void               port_udata_defer_free(struct kqueue *kq, struct port_udata *u);

void    solaris_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state);
void    solaris_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state);

#define kqueue_kevent_enter(_kq, _state) solaris_kevent_enter((_kq), (_state))
#define kqueue_kevent_exit(_kq, _state)  solaris_kevent_exit((_kq), (_state))

/** Allocate the kn_udata for a knote.
 *
 * The udata is heap-allocated so its lifetime can outlive the knote
 * across the kevent_wait window.  See @ref port_udata for the full
 * lifetime model.
 */
#define KN_UDATA_ALLOC(_kn) ((_kn)->kn_udata = port_udata_alloc(_kn))

/** Immediately free a knote's udata and null the field.
 *
 * Use only on paths where the udata was allocated but never
 * successfully passed to port_associate (e.g. kn_create failure).
 * For the normal teardown path use KN_UDATA_DEFER_FREE.
 */
#define KN_UDATA_FREE(_kn)        do { \
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
 * Per-(knote, EVFILT_VNODE) state for the PORT_SOURCE_FILE watcher.
 * The fobj.fo_name pointer references kn_vnode_path, which is
 * heap-allocated and owned by the knote for the lifetime of the
 * association; we cache the FILE_* mask in kn_vnode_events for
 * re-arm after the kernel auto-dissociates on event delivery.
 */
#define SOLARIS_KNOTE_VNODE_PLATFORM_SPECIFIC \
    struct { \
        struct file_obj kn_vnode_fobj; \
        char           *kn_vnode_path; \
        unsigned int    kn_vnode_events; \
    }

#define KNOTE_PLATFORM_SPECIFIC \
    timer_t         kn_timerid; \
    atomic_uint     kn_user_ctr;     /* EVFILT_USER trigger counter; \
                                      * 0->1 transitions port_send.  \
                                      * fetch_add by producer, \
                                      * exchange-to-zero by copyout. */ \
    struct port_udata *kn_udata;     /* Heap-allocated demux header.  Lifecycle \
                                      * is independent of the knote so the \
                                      * udata can outlive EV_DELETE across \
                                      * the kevent_wait window. */ \
    union { \
        POSIX_KNOTE_PROC_PLATFORM_SPECIFIC; \
        SOLARIS_KNOTE_VNODE_PLATFORM_SPECIFIC; \
    }

#define FILTER_PLATFORM_SPECIFIC \
    int             kf_pfd; \
    void           *kf_signal_state; /* posix/signal.c per-filter heap state */ \
    POSIX_FILTER_PROC_PLATFORM_SPECIFIC

/** Per-kqueue deferred-free bookkeeping.
 *
 * kq_next_epoch + kq_inflight + ud_deferred_free implement the
 * deferred-free scheme that keeps a port_udata alive across the
 * kevent_wait window.  See @ref port_udata for the full protocol.
 */
#define KQUEUE_PLATFORM_SPECIFIC \
    uint64_t        kq_next_epoch;                    /* Monotonic counter; bumped on every */ \
                                                      /* kevent() entry.  Rebased toward 0 by */ \
                                                      /* solaris_kqueue_epoch_rebase if it */ \
                                                      /* approaches UINT64_MAX. */ \
    struct kqueue_kevent_state_head kq_inflight;      /* Callers currently inside kevent(). */ \
                                                      /* Tail-inserted, head = oldest = lowest */ \
                                                      /* still-active epoch. */ \
    struct port_udata_head          ud_deferred_free  /* Stale udatas waiting for safe */ \
                                                      /* reclamation.  Tail-inserted, head = */ \
                                                      /* smallest boundary epoch. */

#endif  /* ! _KQUEUE_SOLARIS_PLATFORM_H */
