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
#ifndef  _KQUEUE_LINUX_PLATFORM_H
#define  _KQUEUE_LINUX_PLATFORM_H

struct filter;

#include <sys/syscall.h>
#include <sys/epoll.h>

#include <sys/inotify.h>
#if HAVE_SYS_EVENTFD_H
# include <sys/eventfd.h>
#else
# ifdef SYS_eventfd2
#  define eventfd(a,b) syscall(SYS_eventfd2, (a), (b))
# else
#  define eventfd(a,b) syscall(SYS_eventfd, (a), (b))
# endif

  static inline int eventfd_write(int fd, uint64_t val) {
      if (write(fd, &val, sizeof(val)) < (ssize_t) sizeof(val))
          return (-1);
      else
          return (0);
  }
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#if HAVE_LINUX_LIMITS_H
#  include <linux/limits.h>
#else
#  include <limits.h>
#endif

#if HAVE_SYS_QUEUE_H
# include <sys/queue.h>
#else
# include "../common/queue.h"
#endif

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Check to see if we have SYS_pidfd_open support if we don't,
 * fall back to the POSIX EVFILT_PROC code.
 *
 * Using the build system macro makes it slightly easier to
 * toggle between POSIX/Linux implementations.
 */
#if HAVE_SYS_PIDFD_OPEN
#define KNOTE_PROC_PLATFORM_SPECIFIC  int kn_procfd;
#define FILTER_PROC_PLATFORM_SPECIFIC
#else
#include "../posix/platform_ext.h"
#define KNOTE_PROC_PLATFORM_SPECIFIC  POSIX_KNOTE_PROC_PLATFORM_SPECIFIC
#define FILTER_PROC_PLATFORM_SPECIFIC POSIX_FILTER_PROC_PLATFORM_SPECIFIC
#endif

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
 * Allow us to make arbitrary syscalls
 */
# define _GNU_SOURCE
#if HAVE_LINUX_UNISTD_H
# include <linux/unistd.h>
#elif HAVE_SYSCALL_H
# include <syscall.h>
#endif
#ifndef __ANDROID__
extern long int syscall (long int __sysno, ...);
#endif

/* Workaround for Android */
#ifndef EPOLLONESHOT
# define EPOLLONESHOT (1 << 30)
#endif

/* Convenience macros to access the epoll descriptor for the kqueue */
#define kqueue_epoll_fd(kq)     ((kq)->epollfd)
#define filter_epoll_fd(filt)   ((filt)->kf_kqueue->epollfd)

/*
 * Tell common/kevent.c to drop kq->kq_mtx across kevent_wait.
 *
 * The Linux backend uses a userspace mutex around the epoll
 * syscall.  Holding it across the wait would block any other
 * thread that wants to add or trigger events on this kq, which
 * breaks the cross-thread EVFILT_USER wake pattern.  See @ref
 * kevent in src/common/kevent.c for the consumer side.
 */
#define KEVENT_WAIT_DROP_LOCK   1

/** What type of udata was passed to epoll
 *
 */
enum epoll_udata_type {
    EPOLL_UDATA_KNOTE = 1,           //!< Udata is a pointer to a knote.
    EPOLL_UDATA_FD_STATE,            //!< Udata is a pointer to a fd state structure.
    EPOLL_UDATA_EVENT_FD             //!< Udata is a pointer to an eventfd.
};

struct epoll_udata;

struct epoll_udata *epoll_udata_alloc(enum epoll_udata_type type, void *back);
void                epoll_udata_defer_free(struct kqueue *kq, struct epoll_udata *u);

/** Allocate the kn_udata for a knote
 *
 * The udata is heap-allocated so the udata's lifetime can outlive the
 * knote across the kevent_wait window.  See @ref epoll_udata for the
 * full lifetime model.
 *
 * @param[in] _kn            knote to populate kn_udata on.
 */
#define KN_UDATA_ALLOC(_kn)        ((_kn)->kn_udata = epoll_udata_alloc(EPOLL_UDATA_KNOTE, _kn))

/** Allocate the fds_udata for an fd_state
 *
 * @param[in] _fds           fd_state to populate fds_udata on.
 */
#define FDS_UDATA_ALLOC(_fds)      ((_fds)->fds_udata = epoll_udata_alloc(EPOLL_UDATA_FD_STATE, _fds))

/** Allocate the efd_udata for an eventfd
 *
 * @param[in] _efd           eventfd to populate efd_udata on.
 */
#define EVENTFD_UDATA_ALLOC(_efd)  ((_efd)->efd_udata = epoll_udata_alloc(EPOLL_UDATA_EVENT_FD, _efd))

/** Immediately free a knote's udata and null the field
 *
 * Use only on paths where the udata was allocated but never
 * successfully registered with the kernel (e.g. kn_create failure
 * after KN_UDATA_ALLOC but before/at a failing epoll_ctl(EPOLL_CTL_ADD)).
 * For the normal teardown path (kn_delete after a live registration)
 * use `KN_UDATA_DEFER_FREE` instead so any in-flight epoll_wait
 * results pointing at the freed udata remain safe to dereference.
 *
 * @param[in] _kn            knote whose udata should be freed.
 *                           kn_udata is set to NULL on return.
 */
#define KN_UDATA_FREE(_kn)        do { \
    free((_kn)->kn_udata); \
    (_kn)->kn_udata = NULL; \
} while (0)

/** Immediately free an eventfd's udata and null the field.  See KN_UDATA_FREE.
 *
 * @param[in] _efd           eventfd whose efd_udata should be freed.
 */
#define EVENTFD_UDATA_FREE(_efd)  do { \
    free((_efd)->efd_udata); \
    (_efd)->efd_udata = NULL; \
} while (0)

/** Defer-free a knote's udata and null the field
 *
 * Use on the normal teardown path (kn_delete after a live
 * EPOLL_CTL_ADD).  Queues the udata on the kqueue's deferred-free
 * list so that any concurrent epoll_wait whose TLS buffer still
 * carries the udata's address as data.ptr can safely dereference
 * the udata for the duration of the concurrent kevent() call.  The
 * actual free runs from the matching kevent_exit's sweep once no
 * in-flight caller could still observe the udata.
 *
 * @param[in] _kq            kqueue (used to find the deferred list).
 * @param[in] _kn            knote whose udata should be deferred.
 *                           kn_udata is set to NULL on return.
 */
#define KN_UDATA_DEFER_FREE(_kq, _kn) do { \
    epoll_udata_defer_free((_kq), (_kn)->kn_udata); \
    (_kn)->kn_udata = NULL; \
} while (0)

/** Defer-free an fd_state's udata and null the field.  See KN_UDATA_DEFER_FREE.
 *
 * @param[in] _kq            kqueue.
 * @param[in] _fds           fd_state whose fds_udata should be deferred.
 */
#define FDS_UDATA_DEFER_FREE(_kq, _fds) do { \
    epoll_udata_defer_free((_kq), (_fds)->fds_udata); \
    (_fds)->fds_udata = NULL; \
} while (0)

/** Defer-free an eventfd's udata and null the field.  See KN_UDATA_DEFER_FREE.
 *
 * @param[in] _kq            kqueue.
 * @param[in] _efd           eventfd whose efd_udata should be deferred.
 */
#define EVENTFD_UDATA_DEFER_FREE(_kq, _efd) do { \
    epoll_udata_defer_free((_kq), (_efd)->efd_udata); \
    (_efd)->efd_udata = NULL; \
} while (0)

/** Macro for building a temporary kn epoll_event
 *
 * @param[in] _events        One or more event flags EPOLLIN, EPOLLOUT etc...
 * @param[in] _kn            to associate with the event.  Is used to
 *                           determine the correct filter to notify
 *                           if the event fires.
 */
#define EPOLL_EV_KN(_events, _kn) &(struct epoll_event){ .events = _events, .data = { .ptr = (_kn)->kn_udata }}

/** Macro for building a temporary fds epoll_event
 *
 * @param[in] _events        One or more event flags EPOLLIN, EPOLLOUT etc...
 * @param[in] _fds           File descriptor state to associate with the
 *                           event. Is used during event demuxing to inform
 *                           multiple filters of read/write events on the fd.
 */
#define EPOLL_EV_FDS(_events, _fds) &(struct epoll_event){ .events = _events, .data = { .ptr = (_fds)->fds_udata }}

/** Macro for building a temporary eventfd epoll_event
 *
 * @param[in] _events        One or more event flags EPOLLIN, EPOLLOUT etc...
 * @param[in] _efd           eventfd to build event for.
 */
#define EPOLL_EV_EVENTFD(_events, _efd) &(struct epoll_event){ .events = _events, .data = { .ptr = (_efd)->efd_udata }}

/** Macro for building a temporary epoll_event
 *
 * @param[in] _events        One or more event flags EPOLLIN, EPOLLOUT etc...
 */
#define EPOLL_EV(_events) &(struct epoll_event){ .events = _events }

/** Common structure passed as the epoll data.ptr
 *
 * The kernel returns the udata pointer via `epoll_event::data.ptr`
 * whenever an associated registration becomes ready.  The udata is
 * heap-allocated independently of the containing knote / fd_state /
 * eventfd so the udata's lifetime can outlive the containing object
 * across the kevent_wait window: a thread may hold a stale data.ptr
 * in the thread's TLS `epoll_events[]` buffer between epoll_wait
 * returning and kevent_copyout running, even after another thread
 * has run EV_DELETE on the registration and freed the knote.
 *
 * Once EV_DELETE runs, epoll_ctl(EPOLL_CTL_DEL) stops the kernel
 * queuing new events for the udata, but pre-existing ready events
 * may still surface in some other thread's TLS buffer.  EV_DELETE
 * marks the udata stale, queues the udata on the kqueue's deferred-
 * free list with the current epoch as the boundary, and the deferred
 * udata is reclaimed only once every kevent() caller that could
 * have observed the udata (every caller whose entry epoch is <= the
 * boundary) has exited.
 *
 * Copyout always checks ud_stale before dereferencing ud_kn /
 * ud_fds / ud_efd - the back-pointers are dangling once stale is set.
 */
struct epoll_udata {
    union {
        struct knote        *ud_kn;     //!< Pointer back to the containing knote.
        struct fd_state     *ud_fds;    //!< Pointer back to the containing fd_state.
        struct eventfd      *ud_efd;    //!< Pointer back to the containing eventfd.
    };
    enum epoll_udata_type   ud_type;    //!< Which union member is live.
    bool                    ud_stale;   //!< Set true under kq_mtx by EV_DELETE.
                                        ///< Once ud_stale is set, the back-pointer is
                                        ///< dangling and copyout must skip dispatch.
    uint64_t                ud_boundary_epoch; //!< Highest kevent() entry epoch that could
                                        ///< have observed the udata in its TLS buffer.
                                        ///< Free is gated on every in-flight caller with
                                        ///< epoch <= ud_boundary_epoch having exited.
    TAILQ_ENTRY(epoll_udata) ud_deferred_entry; //!< Entry in kq->ud_deferred_free.
                                        ///< Only valid once ud_stale is set.
};

TAILQ_HEAD(epoll_udata_head, epoll_udata);

/** Per-kevent() in-flight tracking
 *
 * Stack-allocated in the common kevent() entry path; linked into the
 * kqueue's kq_inflight list under kq_mtx for the duration of the
 * kevent() call.  The recorded epoch lets the deferred-free sweep
 * tell whether the caller could still hold a stale udata pointer in
 * the caller's TLS epoll_events buffer.
 */
struct kqueue_kevent_state {
    TAILQ_ENTRY(kqueue_kevent_state) entry; //!< Entry in kq->kq_inflight.
    uint64_t                         epoch; //!< Assigned at kevent_enter time.
};

TAILQ_HEAD(kqueue_kevent_state_head, kqueue_kevent_state);

/** Holds cross-filter information for file descriptors
 *
 * Epoll will not allow the same file descriptor to be inserted
 * twice into the same event loop.
 *
 * To make epoll work reliably, we need to mux and demux the events
 * created by the filters interested in read/write events.
 * This structure records the read and write knotes.
 *
 * Unfortunately this has the side effect of not allowing flags like
 * EPOLLET
 *
 * We use the fact that fds_read/fds_write are nonnull to determine
 * if the FD has already been registered for a particular event when
 * we get a request to update epoll.
 */
struct fd_state {
    RB_ENTRY(fd_state)    fds_entries;    //!< Entry in the RB tree.
    int                   fds_fd;         //!< File descriptor this entry relates to.
    struct knote          *fds_read;      //!< Knote that should be informed of read events.
    struct knote          *fds_write;     //!< Knote that should be informed of write events.
    struct epoll_udata    *fds_udata;     //!< Heap-allocated demux header registered with
                                          ///< epoll via data.ptr.  Lifecycled separately from
                                          ///< the fd_state itself.
};

/** Additional members of struct eventfd
 *
 */
#define EVENTFD_PLATFORM_SPECIFIC \
    struct epoll_udata    *efd_udata

/** Additional members of struct knote
 *
 */
#define KNOTE_PLATFORM_SPECIFIC \
    int kn_epollfd;                       /* A copy of filter->epoll_fd */ \
    int kn_registered;                    /* Is FD registered with epoll */ \
    struct fd_state        *kn_fds;       /* File descriptor's registration state */ \
    int epoll_events;                     /* Which events this file descriptor is registered for */ \
    union { \
        int kn_timerfd; \
        int kn_signalfd; \
        int kn_eventfd; \
        struct { \
            nlink_t         nlink;  \
            off_t           size;   \
            int             inotifyfd; \
        } kn_vnode; \
        KNOTE_PROC_PLATFORM_SPECIFIC; \
    }; \
    struct epoll_udata    *kn_udata      /* Heap-allocated demux header.  The udata's lifecycle
                                            is independent of the knote so the udata can outlive
                                            EV_DELETE across the kevent_wait window. */

/** Additional members of struct filter
 *
 */
#define FILTER_PLATFORM_SPECIFIC \
    FILTER_PROC_PLATFORM_SPECIFIC

/** Additional members of struct kqueue
 *
 * kq_next_epoch + kq_inflight + ud_deferred_free implement the
 * deferred-free scheme that keeps an epoll_udata alive across the
 * kevent_wait window.  See @ref epoll_udata for the full protocol.
 */
#define KQUEUE_PLATFORM_SPECIFIC \
    int epollfd;                          /* Main epoll FD */ \
    int pipefd[2];                        /* FD for pipe that catches close */ \
    RB_HEAD(fd_st, fd_state) kq_fd_st;    /* EVFILT_READ/EVFILT_WRITE fd state */ \
    struct epoll_event kq_plist[MAX_KEVENT]; \
    size_t kq_nplist; \
    uint64_t kq_next_epoch;               /* Monotonic counter; incremented on every kevent() entry. */ \
                                          /* Rebased toward 0 by linux_kqueue_epoch_rebase if it */ \
                                          /* approaches UINT64_MAX (centuries away in practice). */ \
    struct kqueue_kevent_state_head kq_inflight; /* Callers currently inside kevent().  Tail-inserted, */ \
                                          /* so head = oldest = lowest still-active epoch. */ \
    struct epoll_udata_head ud_deferred_free /* Stale udatas waiting for safe reclamation.  Tail- */ \
                                          /* inserted, so head = smallest boundary epoch. */

int     linux_knote_copyout(struct kevent *, struct knote *);

void    linux_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state);
void    linux_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state);

/* Common-code-callable wrappers, consumed by common/kevent.c */
#define kqueue_kevent_enter(_kq, _state) linux_kevent_enter((_kq), (_state))
#define kqueue_kevent_exit(_kq, _state)  linux_kevent_exit((_kq), (_state))

/* utility functions */

int     linux_get_descriptor_type(struct knote *);
int     linux_fd_to_path(char *, size_t, int);

/* epoll-related functions */

char const *  epoll_op_dump(int);
char const *  epoll_event_flags_dump(const struct epoll_event *);
char const *  epoll_event_dump(const struct epoll_event *);

int     epoll_fd_state(struct fd_state **, struct knote *, bool);
int     epoll_fd_state_init(struct fd_state **, struct knote *, int);
void    epoll_fd_state_free(struct fd_state **, struct knote *, int);

bool    epoll_fd_registered(struct filter *filt, struct knote *kn);
int     epoll_update(int op, struct filter *filt, struct knote *kn, int ev, bool delete);
#endif  /* ! _KQUEUE_LINUX_PLATFORM_H */
