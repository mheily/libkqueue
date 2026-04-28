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

void    solaris_kqueue_free(struct kqueue *);
int     solaris_kqueue_init(struct kqueue *);

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
    union { \
        POSIX_KNOTE_PROC_PLATFORM_SPECIFIC; \
        SOLARIS_KNOTE_VNODE_PLATFORM_SPECIFIC; \
    }

#define FILTER_PLATFORM_SPECIFIC \
    int             kf_pfd; \
    void           *kf_signal_state; /* posix/signal.c per-filter heap state */ \
    POSIX_FILTER_PROC_PLATFORM_SPECIFIC

#endif  /* ! _KQUEUE_SOLARIS_PLATFORM_H */
