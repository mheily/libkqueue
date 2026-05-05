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

#ifndef  _KQUEUE_WINDOWS_PLATFORM_H
#define  _KQUEUE_WINDOWS_PLATFORM_H

#include "config.h"

/*
 * Require Windows 10 1803 (build 17134) or later.  This pulls in:
 *   - GetSystemTimePreciseAsFileTime (Win8)
 *   - FileStandardInfo / GetFileInformationByHandleEx (Win8)
 *   - AF_UNIX SOCK_STREAM (Win10 1803+, used by the AF_UNIX smoke
 *     test)
 *   - GetFinalPathNameByHandleW (Vista, baseline)
 * Win10 went GA in 2015; older targets are out of practical use.
 */
#if WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#elif _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <malloc.h>
#include <sys/stat.h>
#include <errno.h>

#include <stdbool.h>

#include "../common/queue.h"

#define _CRT_SECURE_NO_WARNINGS 1
/* The #define doesn't seem to work, but the #pragma does.. */
#ifdef _MSC_VER
# pragma warning( disable : 4996 )
#endif

/*
 * C11 atomic operations.  MSVC needs /experimental:c11atomics
 * (VS17.5+) to enable <stdatomic.h>; the flag is set in the
 * top-level CMakeLists.txt.
 */
#include <stdatomic.h>

#define atomic_inc(p)                 (atomic_fetch_add((p), 1) + 1)
#define atomic_dec(p)                 (atomic_fetch_sub((p), 1) - 1)

/* Compound literals stop the 'expected' values being overwritten. */
#ifndef _MSC_VER
#define atomic_cas(p, oval, nval)     atomic_compare_exchange_strong(p, &(__typeof__(oval)){ oval }, nval)
#define atomic_ptr_cas(p, oval, nval) atomic_compare_exchange_strong(p, (&(uintptr_t){ (uintptr_t)oval }), (uintptr_t)nval)
#else
/* MSVC lacks __typeof__; the only atomic_cas / atomic_ptr_cas
 * callers spell their expected value as a temporary, so a plain
 * unary-& works here. */
#define atomic_cas(p, oval, nval) \
    atomic_compare_exchange_strong((p), &(int){ (int)(oval) }, (int)(nval))
#define atomic_ptr_cas(p, oval, nval) \
    atomic_compare_exchange_strong((p), &(uintptr_t){ (uintptr_t)(oval) }, (uintptr_t)(nval))
#endif
#define atomic_ptr_swap(p, nval)      atomic_exchange(p, (uintptr_t)nval)
#define atomic_ptr_load(p)            atomic_load(p)

/*
 * Drop the kq lock across kevent_wait.  GetQueuedCompletionStatus
 * doesn't touch any libkqueue-managed state, and holding kq_mtx
 * across it would block other threads trying to register or trigger
 * events on the same kq - which is exactly the cross-thread
 * EVFILT_USER wake pattern test_kevent_threading_user_trigger_cross_thread
 * exercises.  Common code re-acquires the lock before copyout.
 */
#define KEVENT_WAIT_DROP_LOCK 1

/*
 * IOCP completion key reserved for close-detect completions.
 * Filter ids (encoded via KQ_FILTER_KEY below) are negative
 * shorts that sign-extend to UINT64_MAX-N when cast through
 * ULONG_PTR; the small positive value here can't collide.
 */
#define KQ_CLOSE_DETECT_KEY ((ULONG_PTR)1)

/*
 * Reserved IOCP completion key for pipe-HANDLE EVFILT_READ
 * (overlapped 0-byte ReadFile).  The completion arrives with
 * overlap = &knote->kn_read.pipe_ov; windows_kevent_copyout
 * recovers the owning knote via CONTAINING_RECORD.
 */
#define KQ_PIPE_READ_KEY ((ULONG_PTR)2)

/*
 * IOCP completion key for synthetic posts that target a specific
 * filter: encodes the kev.filter value (a negative short) into the
 * sign-extended high bits of the ULONG_PTR.  windows_kevent_copyout
 * extracts it and dispatches to the matching kqops filter slot.
 *
 * Used by:
 *   - per-knote synthetic posts (overlap = knote pointer; the filter
 *     that owns the knote drains it on dispatch).
 *   - eventfd doorbell posts (overlap = NULL; filter drains its own
 *     pending state without touching a knote).
 *
 * Posts with key == 0 and overlap == NULL are treated as a bare
 * "wake the parked GQCS" sentinel (windows_kqueue_interrupt) - no
 * filter id, no knote, just bring the waiter out of the kernel.
 */
#define KQ_FILTER_KEY(_fid) ((ULONG_PTR)(LONG_PTR)(short)(_fid))

/*
 * Per-kevent() in-flight tracking.  Stack-allocated in the common
 * kevent() entry path; linked into kq->kq_inflight under the per-kq
 * lock for the duration of the call.  kqueue_free defers its
 * teardown if any callers are still in flight at close time, so a
 * waiter parked inside kqops.kevent_wait (with the lock dropped via
 * KEVENT_WAIT_DROP_LOCK) doesn't return into a freed kq when it
 * tries to re-acquire kq_lock.
 */
struct kqueue_kevent_state {
    TAILQ_ENTRY(kqueue_kevent_state) entry;
};
TAILQ_HEAD(kqueue_kevent_state_head, kqueue_kevent_state);

void windows_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state);
void windows_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state);

#define kqueue_kevent_enter(_kq, _state) windows_kevent_enter((_kq), (_state))
#define kqueue_kevent_exit(_kq, _state)  windows_kevent_exit((_kq), (_state))

/*
 * Additional members of struct kqueue.
 *
 * Close detection: kqueue() hands the consumer the write end of
 * an anonymous pipe (adopted as a CRT fd via _open_osfhandle, so
 * the value is releasable with close()).  We keep the read end
 * here, associate it with kq_iocp, and issue an overlapped
 * 0-byte ReadFile.  The consumer's close() drops the only
 * reference to the write handle; the pipe's read side then
 * completes with ERROR_BROKEN_PIPE and the completion lands in
 * kq_iocp with KQ_CLOSE_DETECT_KEY - waking any parked
 * kevent_wait and giving us the chance to surface EBADF cleanly.
 */
#define KQUEUE_PLATFORM_SPECIFIC \
    HANDLE kq_iocp; \
    struct filter *kq_filt_ref[EVFILT_SYSCOUNT]; \
    size_t kq_filt_count; \
    HANDLE kq_close_read; \
    OVERLAPPED kq_close_ov; \
    char kq_close_buf[1]; \
    /* Set by windows_kevent_copyout when the close-detect    */ \
    /* completion fires (consumer's close(kqfd)).  Subsequent */ \
    /* kevent_wait returns EBADF immediately, and a thread-   */ \
    /* pool work item is scheduled to free the kq.            */ \
    atomic_int kq_consumer_closed; \
    /* Monotonic counter bumped at the start of every            */ \
    /* windows_kevent_copyout call.  Per-knote                   */ \
    /* kn_dispatch_seq is set to this on dispatch so the same    */ \
    /* batch's IOCP drain can recognise (and discard) duplicate  */ \
    /* level-trigger self-posts for a knote already delivered.   */ \
    unsigned long kq_dispatch_seq; \
    /* Inflight tracking under KEVENT_WAIT_DROP_LOCK.  Always */ \
    /* empty on Windows: kqueue_kevent_enter/exit are the     */ \
    /* common no-op stubs, so kqueue_free's TAILQ_EMPTY check */ \
    /* trivially passes and we go straight to teardown.       */ \
    struct kqueue_kevent_state_head kq_inflight

/*
 * Additional members of struct filter
 */
/*
#define FILTER_PLATFORM_SPECIFIC \
    HANDLE kf_event_handle
*/

/*
 * Per-eventfd state.  The "eventfd" abstraction is a generic
 * cross-thread doorbell into the kqueue's wait loop; on Linux it
 * maps to a real eventfd(2), on Solaris to a port_send into the
 * kqueue's event port, and on Windows to a PostQueuedCompletionStatus
 * into kq->kq_iocp with the originating filter id carried in the
 * completion key.
 *
 * efd_filter_id is set at init time from filt->kf_id and used as
 * the IOCP key so windows_kevent_copyout can route the wakeup
 * back to the originating filter via filter_lookup().
 *
 * efd_raised coalesces N raises before a drain into a single IOCP
 * entry, matching the level-triggered eventfd counter semantics
 * common code expects.  Cleared by eventfd_lower().
 */
#define EVENTFD_PLATFORM_SPECIFIC \
    int        efd_filter_id; \
    atomic_int efd_raised

/*
 * Per-filter knote platform state.  Filter-exclusive fields go in
 * named structs referenced from the union below; multi-filter
 * fields (kn_event_whandle / kn_handle / kn_fire_count /
 * kn_dispatch_seq) stay outside.
 */
struct windows_knote_read {
    /* EV_CLEAR/EV_DISPATCH edge-trigger: tracks last reported
     * FIONREAD byte count so the WSAEventSelect callback can
     * suppress re-assertions that don't represent fresh data. */
    atomic_int      last_data;
    /* Set when WSAEnumNetworkEvents observes FD_CLOSE so
     * copyout can OR EV_EOF into the delivered event flags. */
    atomic_int      eof;
    /* Captured iErrorCode[FD_CLOSE_BIT] on the FD_CLOSE that
     * latched eof above.  Surfaced as fflags in copyout so a
     * TCP RST shows up as the actual WSAECONNRESET (parity
     * with posix/read.c SO_ERROR path). */
    atomic_int      so_error;
    /* KNFL_PIPE: 0-byte overlapped ReadFile is attached to
     * kq_iocp; the OVERLAPPED hangs off the knote and the
     * dispatcher recovers the knote via CONTAINING_RECORD-
     * style pointer arithmetic. */
    OVERLAPPED      pipe_ov;
    /* KNFL_FILE: marks the knote as a synthetic level-
     * triggered source so copyout can re-post a completion
     * when the knote remains armed. */
    int             file_synthetic;
    char            pipe_buf[1];
};

struct windows_knote_write {
    /* KNFL_FILE: see windows_knote_read.file_synthetic. */
    int             file_synthetic;
    /*
     * KNFL_PIPE: latched once a 0-byte WriteFile probe sees the
     * reader gone (ERROR_BROKEN_PIPE / ERROR_NO_DATA).  Used to
     * stop the synthetic re-post cycle and surface EV_EOF on every
     * subsequent drain (BSD level-trigger semantic).
     */
    int             pipe_eof;
};

/*
 * Additional members for struct knote
 */
#define KNOTE_PLATFORM_SPECIFIC \
    HANDLE                     kn_event_whandle; \
    void                       *kn_handle; \
    /* Generic fire-count for filters that need to report */     \
    /* accumulated occurrences in copyout (e.g. EVFILT_TIMER). */\
    atomic_int                 kn_fire_count; \
    /* Last value of kq_dispatch_seq at which this knote was   */\
    /* delivered.  Used by the IOCP drain in copyout to avoid  */\
    /* stacking duplicate level-trigger EV_EOF events for the  */\
    /* same knote in a single kevent() call.                   */\
    unsigned long              kn_dispatch_seq; \
    union { \
        struct windows_knote_read  kn_read; \
        struct windows_knote_write kn_write; \
    }

/*
 * Some datatype forward declarations
 */
struct filter;
struct kqueue;
struct knote;

/*
 * Hooks and prototypes
 */
int     windows_kqueue_init(struct kqueue *);
void    windows_kqueue_free(struct kqueue *);
VOID    CALLBACK windows_kqueue_close_cleanup(PTP_CALLBACK_INSTANCE inst, PVOID ctx);
int     windows_kevent_wait(struct kqueue *, int, const struct timespec *);
int     windows_kevent_copyout(struct kqueue *, int, struct kevent *, int);
int     windows_filter_init(struct kqueue *, struct filter *);
void    windows_filter_free(struct kqueue *, struct filter *);
int     windows_get_descriptor_type(struct knote *);

int     windows_eventfd_init(struct eventfd *efd, struct filter *filt);
void    windows_eventfd_close(struct eventfd *efd);
int     windows_eventfd_raise(struct eventfd *efd);
int     windows_eventfd_lower(struct eventfd *efd);
int     windows_eventfd_descriptor(struct eventfd *efd);
int     windows_eventfd_register(struct kqueue *kq, struct eventfd *efd);
void    windows_eventfd_unregister(struct kqueue *kq, struct eventfd *efd);

/*
 * GCC-compatible branch prediction macros
 */
#ifdef __GNUC__
# define likely(x)       __builtin_expect((x), 1)
# define unlikely(x)     __builtin_expect((x), 0)
#else
# define likely(x) (x)
# define unlikely(x) (x)
#endif

#ifdef _MSC_VER
/* Function visibility macros */
#define VISIBLE __declspec(dllexport)
#define HIDDEN
#endif

#if !defined(__func__) && !defined(__GNUC__)
#define __func__ __FUNCDNAME__
#endif

#define snprintf _snprintf
#define ssize_t  SSIZE_T
#define sleep(x) Sleep((x) * 1000)
#define inline __inline

/* For POSIX compatibility when compiling, not for actual use */
typedef int socklen_t;
typedef int nlink_t;
typedef int timer_t;
typedef int sigset_t;
#if HAVE_SYS_TYPES_H != 1
typedef int pid_t;
#endif

/*
 * pthread_t isn't typedef'd here: the test suite's win32 shim
 * (test/win32_compat.h) defines it as HANDLE so the threading
 * tests link against real Win32 threads.  No library source
 * uses pthread_t.
 */

#ifndef __GNUC__
# define __thread    __declspec(thread)
#endif

/* Emulation of pthreads mutex functionality */
#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_PROCESS_PRIVATE 2
typedef CRITICAL_SECTION           pthread_mutex_t;
typedef CRITICAL_SECTION           pthread_spinlock_t;
typedef CRITICAL_SECTION           pthread_rwlock_t;

#define EnterCriticalSection(x)    EnterCriticalSection ((x))
#define pthread_mutex_lock         EnterCriticalSection
#define pthread_mutex_unlock       LeaveCriticalSection
#define pthread_mutex_init(x,y)    InitializeCriticalSection((x))
#define pthread_spin_lock          EnterCriticalSection
#define pthread_spin_unlock        LeaveCriticalSection
#define pthread_spin_init(x,y)     InitializeCriticalSection((x))
#define pthread_mutex_init(x,y)    InitializeCriticalSection((x))
#define pthread_mutex_destroy(x)
#define pthread_rwlock_rdlock      EnterCriticalSection
#define pthread_rwlock_wrlock      EnterCriticalSection
#define pthread_rwlock_unlock      LeaveCriticalSection
#define pthread_rwlock_init(x,y)   InitializeCriticalSection((x))

/*
 * pthread_once -> Win32 INIT_ONCE.  Unifies the one-shot init
 * barrier the common code uses around libkqueue_init() so kqueue()
 * doesn't have to branch on _WIN32.  POSIX guarantees "On return
 * from pthread_once, init_routine shall have completed" with
 * memory-ordering visibility; InitOnceExecuteOnce gives us the
 * same semantics natively (concurrent callers block on the still-
 * running init and observe its writes after return).
 */
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static BOOL CALLBACK
_kq_pthread_once_thunk(PINIT_ONCE init_once, PVOID arg, PVOID *ctx)
{
    void (*fn)(void) = (void (*)(void)) arg;
    (void) init_once; (void) ctx;
    fn();
    return TRUE;
}

static __inline int
pthread_once(pthread_once_t *once, void (*fn)(void))
{
    return InitOnceExecuteOnce(once, _kq_pthread_once_thunk,
                               (PVOID)(uintptr_t) fn, NULL) ? 0 : -1;
}

#endif  /* ! _KQUEUE_WINDOWS_PLATFORM_H */
