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

#ifndef  _KQUEUE_POSIX_PLATFORM_EXT_H
#define  _KQUEUE_POSIX_PLATFORM_EXT_H

#include <signal.h>

/* Highest signum (exclusive) for posix/signal.c sentries. */
#ifndef POSIX_SIGNAL_MAX
#  ifdef NSIG
#    define POSIX_SIGNAL_MAX NSIG
#  else
#    define POSIX_SIGNAL_MAX 64
#  endif
#endif

/** Additional members of 'struct eventfd'
 *
 * These should be included in the platform's EVENTFD_PLATFORM_SPECIFIC
 * macro definition if using the POSIX eventfd functions.
 */
#define POSIX_EVENTFD_PLATFORM_SPECIFIC \
    int             ef_wfd

/** Additional members of 'struct knote'
 *
 * These should be included in the platform's KNOTE_PLATFORM_SPECIFIC
 * macro definition if using the POSIX proc filter.
 */
#define POSIX_KNOTE_PROC_PLATFORM_SPECIFIC \
    struct { \
        LIST_ENTRY(knote) kn_proc_waiter; \
        int kn_proc_status; \
    }

/*
 * Per-filter platform state.  Each EVFILT_* that needs any state
 * gets its own struct; they're grouped in union posix_filter_state
 * because a given struct-filter slot only ever serves one filter
 * id.  Filters with no platform state (READ, WRITE, USER, TIMER)
 * don't appear here - READ/WRITE work directly off kq_fds /
 * kq_wfds, USER off the common kf_efd, TIMER off kq_timers.
 *
 * Add a new struct here when a new filter grows per-filter state;
 * adding a member to the union is a no-op for unrelated filters.
 */
struct sig_filter_state;            /* defined in common/evfilt_signal.h */

struct posix_filter_signal {
    struct sig_filter_state *state; /* heap-allocated dispatcher state */
};

union posix_filter_state {
    struct posix_filter_signal  sig;
};

/** Additional members of 'struct filter'
 *
 * These should be included in the platform's FILTER_PLATFORM_SPECIFIC
 * macro definition if using all the POSIX filters.
 */
#define POSIX_FILTER_PLATFORM_SPECIFIC \
    int             kf_pfd;             /* fd dispatcher polls for filter readiness */ \
    union posix_filter_state kf_state   /* per-filter union, indexed by kf_id */

/** Additional members of 'struct kqueue'
 *
 * These should be included in the platform's KQUEUE_PLATFORM_SPECIFIC
 * macro definition.
 */
/*
 * In-flight tracking for KEVENT_WAIT_DROP_LOCK.  Each thread inside
 * kevent() (between kqueue_kevent_enter and kqueue_kevent_exit)
 * stack-allocates a kqueue_kevent_state and links it on this TAILQ.
 * Empty TAILQ + kq_freeing == ready to complete deferred free.
 */
TAILQ_HEAD(posix_kqueue_kevent_state_head, kqueue_kevent_state);

struct posix_timer;
RB_HEAD(posix_timer_tree, posix_timer);

#define POSIX_KQUEUE_PLATFORM_SPECIFIC \
    fd_set          kq_fds;          /* watched-for-read fd set */ \
    fd_set          kq_rfds;         /* read-readable fds after last pselect */ \
    fd_set          kq_wfds;         /* watched-for-write fd set */ \
    fd_set          kq_wrfds;        /* write-ready fds after last pselect */ \
    int             kq_nfds;         /* highest watched fd + 1, for pselect's nfds */ \
    int             kq_wake_wfd;     /* write end of the self-pipe used as kq_id */ \
    int             kq_always_ready; /* count of "always-ready" knotes; non-zero \
                                      * forces pselect to a 0 timeout so file/etc \
                                      * knotes get re-dispatched every wait */ \
    struct posix_kqueue_kevent_state_head kq_inflight; /* kevent() callers in-flight */ \
    struct posix_timer_tree kq_timers   /* EVFILT_TIMER deadlines (RB-tree by next-deadline) */

/** Additional members of 'struct knote'
 *
 */
#define POSIX_KNOTE_PLATFORM_SPECIFIC \
    POSIX_KNOTE_PROC_PLATFORM_SPECIFIC; \
    struct posix_timer *kn_timer

#endif  /* ! _KQUEUE_POSIX_PLATFORM_EXT_H */
