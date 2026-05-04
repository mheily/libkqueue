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

#include "../common/private.h"

struct event_buf {
    DWORD       bytes;
    ULONG_PTR   key;
    OVERLAPPED *overlap;
};

/*
 * Per-thread evt event buffer used to ferry data between
 * kevent_wait() and kevent_copyout().
 */
static __thread struct event_buf iocp_buf;

/*
 * The timeout (in ms) the caller passed to kevent().  Stashed by
 * windows_kevent_wait so windows_kevent_copyout's stale-discard
 * drain loop can re-block on the IOCP with the right budget when
 * everything queued so far was a spurious wakeup.  INFINITE means
 * "wait forever", 0 means "non-blocking peek".
 */
static __thread DWORD iocp_wait_timeout_ms;

extern int windows_signal_init(void);
#ifndef NDEBUG
extern dbg_func_t windows_dbg_default(void);
#endif

static atomic_int kq_close_pipe_seq;

/*
 * No-op invalid-parameter handler.  _get_osfhandle on a non-fd
 * ident invokes the CRT IPH which in Debug builds aborts (or pops
 * a dialog under MSVC's debug runtime).  Install this around
 * _get_osfhandle so the call simply returns -1 + EBADF.
 */
static void
windows_iph_noop(const wchar_t *expr, const wchar_t *func,
                 const wchar_t *file, unsigned int line, uintptr_t reserved)
{
  (void) expr; (void) func; (void) file; (void) line; (void) reserved;
}

int
windows_get_descriptor_type(struct knote *kn)
{
  /*
   * kev.ident is a SOCKET for socket knotes and a CRT file
   * descriptor for files/pipes (the test harness adopts HANDLEs
   * via _open_osfhandle).  Detection order:
   *
   *   1. getsockopt(SO_TYPE) - succeeds iff ident is a SOCKET.
   *   2. _get_osfhandle(ident) - succeeds iff ident is a CRT fd.
   *      Wrap with a no-op invalid-parameter handler so a non-fd
   *      ident doesn't trip the CRT debug abort.
   */
  socklen_t slen;
  int       lsock = 0, stype = 0, i;
  HANDLE    h = INVALID_HANDLE_VALUE;

  /* Step 1: SOCKET check via getsockopt. */
  slen = sizeof(stype);
  i = getsockopt(kn->kev.ident, SOL_SOCKET, SO_TYPE,
                 (char *) &stype, &slen);
  if (i == 0) {
    slen = sizeof(lsock);
    i = getsockopt(kn->kev.ident, SOL_SOCKET, SO_ACCEPTCONN,
                   (char *) &lsock, &slen);
    if (i == 0 && lsock)
      kn->kn_flags |= KNFL_SOCKET_PASSIVE;
    if (stype == SOCK_STREAM)
      kn->kn_flags |= KNFL_SOCKET_STREAM;
    return 0;
  }

  /* Step 2: CRT fd via _get_osfhandle, with IPH suppressed. */
  {
    _invalid_parameter_handler oldh =
        _set_thread_local_invalid_parameter_handler(windows_iph_noop);
    h = (HANDLE) _get_osfhandle((int) kn->kev.ident);
    _set_thread_local_invalid_parameter_handler(oldh);
  }

  if (h == INVALID_HANDLE_VALUE)
    return -1;

  switch (GetFileType(h)) {
  case FILE_TYPE_PIPE:
    dbg_printf("ident=%d - classified as pipe", (int) kn->kev.ident);
    kn->kn_flags |= KNFL_PIPE;
    break;
  case FILE_TYPE_DISK:
    dbg_printf("ident=%d - classified as regular file", (int) kn->kev.ident);
    kn->kn_flags |= KNFL_FILE;
    break;
  default:
    /* FILE_TYPE_CHAR (console etc.), FILE_TYPE_REMOTE, FILE_TYPE_UNKNOWN.
     * Treat as a generic file source for now. */
    dbg_printf("ident=%d - unknown GetFileType, treating as file",
               (int) kn->kev.ident);
    kn->kn_flags |= KNFL_FILE;
    break;
  }

  return 0;
}

int
windows_kqueue_init(struct kqueue *kq)
{
    HANDLE pipe_read = NULL;
    HANDLE pipe_write = NULL;
    BOOL rd_ok;
    char pipe_name[64];

    TAILQ_INIT(&kq->kq_inflight);

    kq->kq_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL,
                                         (ULONG_PTR) 0, 0);
    if (kq->kq_iocp == NULL) {
        dbg_lasterror("CreateIoCompletionPort");
        return (-1);
    }

    /*
     * kqueue() must return a value the consumer can
     * close() without tripping the CRT debug-runtime assert.
     * _open_osfhandle rejects IOCP handles (EBADF) but accepts
     * pipe handles, so we synthesise a closeable fd by giving the
     * consumer the write end of a single-instance named pipe.
     *
     * CreatePipe() can't be used: it makes synchronous pipes,
     * which CreateIoCompletionPort rejects with
     * ERROR_INVALID_PARAMETER.  CreateNamedPipe with
     * FILE_FLAG_OVERLAPPED produces a server (read) side that
     * supports overlapped I/O, then we CreateFile the client
     * (write) side as a normal synchronous handle the consumer
     * can close().
     *
     * On peer-close (consumer's close() drops the last write
     * reference), the queued overlapped 0-byte ReadFile completes
     * with ERROR_BROKEN_PIPE; the completion lands in kq_iocp
     * with KQ_CLOSE_DETECT_KEY, and windows_kevent_copyout
     * surfaces it to any parked kevent() as EBADF.
     */
    snprintf(pipe_name, sizeof(pipe_name),
             "\\\\.\\pipe\\libkqueue-close-%lu-%d",
             (unsigned long) GetCurrentProcessId(),
             (int) atomic_fetch_add(&kq_close_pipe_seq, 1));

    pipe_read = CreateNamedPipeA(pipe_name,
                                 PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                                     FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                                 1, 0, 0, 0, NULL);
    if (pipe_read == INVALID_HANDLE_VALUE) {
        dbg_lasterror("CreateNamedPipe");
        pipe_read = NULL;
        goto err;
    }

    pipe_write = CreateFileA(pipe_name, GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (pipe_write == INVALID_HANDLE_VALUE) {
        dbg_lasterror("CreateFile(close-detect write)");
        pipe_write = NULL;
        goto err;
    }

    kq->kq_id = _open_osfhandle((intptr_t) pipe_write, _O_BINARY);
    if (kq->kq_id < 0) {
        dbg_perror("_open_osfhandle");
        CloseHandle(pipe_write);
        pipe_write = NULL;
        goto err;
    }
    /* CRT now owns pipe_write; do not close it again. */
    pipe_write = NULL;

    /*
     * Disown any stale kq still claiming the same CRT slot.  A
     * previous kq whose consumer close()d the fd without parking a
     * thread in kevent() never had its close-detect dispatch fire,
     * so its kq_id was never reset to -1.  Once _open_osfhandle has
     * recycled that slot, the stale kq's kq_id refers to OUR new
     * pipe_write - and the immediately-following kqueue_free_by_id
     * (in src/common/kqueue.c) would otherwise call _close on it,
     * clearing FOPEN on the new slot and tripping a CRT assertion
     * on the consumer's eventual close(kqfd).
     */
    {
        struct kqueue *iter;
        LIST_FOREACH(iter, &kq_list, kq_entry) {
            if (iter != kq && iter->kq_id == kq->kq_id) {
                dbg_printf("disowning stale kq=%p (kq_id=%d) so "
                           "auto-cleanup of the recycled fd doesn't "
                           "_close our brand-new slot",
                           (void *) iter, iter->kq_id);
                iter->kq_id = -1;
            }
        }
    }

    if (CreateIoCompletionPort(pipe_read, kq->kq_iocp,
                               KQ_CLOSE_DETECT_KEY, 0) == NULL) {
        dbg_lasterror("CreateIoCompletionPort(close-detect)");
        goto err;
    }

    kq->kq_close_read = pipe_read;
    memset(&kq->kq_close_ov, 0, sizeof(kq->kq_close_ov));

    rd_ok = ReadFile(pipe_read, kq->kq_close_buf, sizeof(kq->kq_close_buf),
                     NULL, &kq->kq_close_ov);
    if (!rd_ok && GetLastError() != ERROR_IO_PENDING) {
        dbg_lasterror("ReadFile(close-detect)");
        goto err;
    }
    pipe_read = NULL; /* now owned by kq_close_read + IOCP */

    if (filter_register_all(kq) < 0)
        goto err;

    return (0);

err:
    if (pipe_read != NULL) CloseHandle(pipe_read);
    if (pipe_write != NULL) CloseHandle(pipe_write);
    if (kq->kq_close_read != NULL) {
        CancelIoEx(kq->kq_close_read, &kq->kq_close_ov);
        CloseHandle(kq->kq_close_read);
        kq->kq_close_read = NULL;
    }
    if (kq->kq_id >= 0) {
        _close(kq->kq_id);
        kq->kq_id = -1;
    }
    if (kq->kq_iocp != NULL) {
        CloseHandle(kq->kq_iocp);
        kq->kq_iocp = NULL;
    }
    return (-1);
}

void
windows_kqueue_free(struct kqueue *kq)
{
    /*
     * Did the consumer already close()d the kq fd?  We treat
     * either (a) the close-detect IRP having already completed or
     * (b) kq_consumer_closed having been latched by copyout's
     * dispatch as proof - both mean the original kq_id no longer
     * belongs to us, so calling _close on it would either trip the
     * Debug CRT's FOPEN assertion (consumer's _close already
     * cleared FOPEN) or punt the unrelated process-wide slot the
     * runtime has since recycled.
     */
    bool consumer_already_closed = false;
    if (atomic_load(&kq->kq_consumer_closed))
        consumer_already_closed = true;
    else if (kq->kq_close_read != NULL &&
             HasOverlappedIoCompleted(&kq->kq_close_ov))
        consumer_already_closed = true;

    /*
     * Cancel the close-detect ReadFile before closing handles -
     * the cancellation completion (ERROR_OPERATION_ABORTED) will
     * land in the IOCP and get silently dropped by the impending
     * CloseHandle.
     */
    if (kq->kq_close_read != NULL) {
        CancelIoEx(kq->kq_close_read, &kq->kq_close_ov);
        CloseHandle(kq->kq_close_read);
        kq->kq_close_read = NULL;
    }
    /*
     * Only release the CRT slot if the consumer hasn't already
     * close()'d the kq fd.  Two paths:
     *
     *   1. kqueue_free called from windows_kqueue_close_cleanup
     *      (the close-detect dispatch): copyout already set
     *      kq->kq_id = -1, so we skip naturally.
     *
     *   2. kqueue_free called from map_insert auto-cleanup of a
     *      stale kq when a new kqueue() recycles the same fd: the
     *      old kq's close-detect IRP fired but no thread was
     *      parked to dispatch it (e.g. test_kqueue_alloc creates
     *      and immediately close()s without ever waiting on it),
     *      so kq_id is still the (now recycled) fd value.
     *      consumer_already_closed catches that path; without it
     *      _close here would close the NEW kqueue's fresh CRT
     *      slot, leaving the consumer's later close() to trip the
     *      Debug CRT FOPEN assertion.
     */
    if (kq->kq_id >= 0 && !consumer_already_closed) {
        _close(kq->kq_id);
    }
    kq->kq_id = -1;
    if (kq->kq_iocp != NULL) {
        CloseHandle(kq->kq_iocp);
        kq->kq_iocp = NULL;
    }
}

/*
 * Win32 thread-pool callback: runs after the consumer closes
 * the kq fd, freeing the kq for real (map_remove + LIST_REMOVE
 * + windows_kqueue_free + free).  Deferred from
 * windows_kevent_copyout because that runs with the per-kq
 * lock held; common kqueue_free needs to take that lock and
 * destroy it.
 *
 * Any kevent caller that arrives between the close-detect
 * completion and this callback running sees kq_consumer_closed
 * via the windows_kevent_wait short-circuit and returns EBADF
 * without ever entering kqops.kevent_wait, so we can free the
 * kq freely once we get the global kq_mtx.
 */
VOID CALLBACK
windows_kqueue_close_cleanup(PTP_CALLBACK_INSTANCE inst, PVOID ctx)
{
    struct kqueue *kq = (struct kqueue *) ctx;
    struct kqueue *iter;
    bool alive = false;
    (void) inst;

    /*
     * kqueue_free asserts kq_mtx is held; it does the
     * LIST_REMOVE, map_remove, kqops.kqueue_free, per-kq mutex
     * destroy, and free.  Safe to take kq_mtx here because the
     * original kevent() caller has released the per-kq lock by
     * the time this thread runs.
     *
     * Race we have to defend against: process exit fires
     * libkqueue_free between the close-detect post and our
     * dispatch.  libkqueue_free walks kq_list and frees every
     * kq still in it - if it raced us, this kq pointer is
     * already freed memory.  Verify it's still in kq_list under
     * the global lock before dereferencing.
     */
    tracing_mutex_lock(&kq_mtx);
    LIST_FOREACH(iter, &kq_list, kq_entry) {
        if (iter == kq) { alive = true; break; }
    }
    if (alive)
        kqueue_free(kq);
    tracing_mutex_unlock(&kq_mtx);
}

/*
 * Wake every thread parked in GetQueuedCompletionStatus on this kq
 * so kqueue_free's deferred teardown can complete.  Each parked
 * waiter dequeues the synthetic completion, returns to the common
 * kevent loop, takes the kq lock, observes kq_freeing and a non-
 * empty kq_inflight, exits without re-blocking.  The last caller
 * out runs kqueue_complete_deferred_free.
 *
 * One PostQueuedCompletionStatus per inflight caller is enough -
 * each completion only wakes one GQCS - so we post the count by
 * walking kq_inflight under the per-kq lock.  A spurious extra
 * wake (post to a kq with no waiters) is harmless because copyout
 * is robust to discarded completions.
 */
static void
windows_kqueue_interrupt(struct kqueue *kq)
{
    struct kqueue_kevent_state *state;
    if (kq->kq_iocp == NULL)
        return;
    TAILQ_FOREACH(state, &kq->kq_inflight, entry) {
        PostQueuedCompletionStatus(kq->kq_iocp, 0, (ULONG_PTR) 0, NULL);
    }
}

int
windows_kevent_wait(struct kqueue *kq, int no, const struct timespec *timeout)
{
    int retval;
    DWORD       timeout_ms;
    BOOL        success;

    /*
     * kq fd was closed by the consumer (and the close-detect
     * completion already drained on some thread).  Surface
     * EBADF so the caller doesn't park indefinitely.  Set in
     * windows_kevent_copyout's close-detect path.
     */
    if (atomic_load(&kq->kq_consumer_closed)) {
        errno = EBADF;
        return (-1);
    }

    if (timeout == NULL) {
        timeout_ms = INFINITE;
    } else if ( timeout->tv_sec == 0 && timeout->tv_nsec < 1000000 ) {
        /* do we need to try high precision timing? */
        // TODO: This is currently not possible on windows!
        timeout_ms = 0;
    } else {  /* Convert timeout to milliseconds */
        timeout_ms = 0;
        if (timeout->tv_sec > 0)
            timeout_ms += ((DWORD)timeout->tv_sec) * 1000;
        if (timeout->tv_nsec > 0)
            timeout_ms += timeout->tv_nsec / 1000000;
    }

    dbg_printf("timeout=%u ms - waiting for events", (unsigned int) timeout_ms);
#if 0
    if(timeout_ms <= 0)
        dbg_puts("Woop, not waiting !?");
#endif
    iocp_wait_timeout_ms = timeout_ms;
    memset(&iocp_buf, 0, sizeof(iocp_buf));
    success = GetQueuedCompletionStatus(kq->kq_iocp,
            &iocp_buf.bytes, &iocp_buf.key, &iocp_buf.overlap,
            timeout_ms);
    /*
     * Re-check kq_consumer_closed / kq_freeing after the wait
     * returns.  Either another thread already ran the close-
     * detect dispatch (and scheduled the deferred cleanup), or
     * kqueue_free has flipped kq_freeing and called
     * windows_kqueue_interrupt to wake every parked waiter.
     * Surface EBADF so the common kevent loop unwinds via out:
     * and the last-caller-out completes the deferred free,
     * instead of letting copyout dispatch a phantom completion
     * against a kq the rest of the world has already torn down.
     */
    if (atomic_load(&kq->kq_consumer_closed) || kq->kq_freeing) {
        errno = EBADF;
        return (-1);
    }
    if (success) {
        return (1);
    }
    /*
     * GetQueuedCompletionStatus returns FALSE in two distinct
     * shapes (per MSDN):
     *
     *   (a) No completion was dequeued: lpOverlapped == NULL.  The
     *       only meaningful case here is WAIT_TIMEOUT; anything
     *       else is a real error on the IOCP itself.
     *   (b) A completion WAS dequeued for a failed I/O:
     *       lpOverlapped != NULL, GetLastError() is the I/O error.
     *       Notably the close-detect ReadFile returns
     *       ERROR_BROKEN_PIPE this way when the consumer closes
     *       the kq fd, and we MUST surface this as a regular wait
     *       success so the dispatcher can route the completion.
     */
    if (iocp_buf.overlap != NULL) {
        dbg_printf("GetQueuedCompletionStatus dequeued failed-I/O completion "
                   "(GLE=%lu, key=%llu) - dispatching",
                   (unsigned long) GetLastError(),
                   (unsigned long long) iocp_buf.key);
        return (1);
    }
    {
        DWORD gle = GetLastError();
        if (gle == WAIT_TIMEOUT) {
            dbg_puts("no events within the given timeout");
            return (0);
        }
        /*
         * The kqueue's IOCP was closed while we were parked.
         * That happens when one thread closes the kq fd while
         * other threads are still in kevent() on it; the close-
         * detect dispatch eventually CloseHandle's kq_iocp and
         * GQCS returns ERROR_ABANDONED_WAIT_0 to every waiter.
         * Surface EBADF so the caller can distinguish 'kq went
         * away' from a transient IOCP error and unwind cleanly,
         * matching what the test threading_close_multi waiters
         * expect.
         */
        if (gle == ERROR_ABANDONED_WAIT_0 || gle == ERROR_INVALID_HANDLE) {
            errno = EBADF;
            return (-1);
        }
        dbg_lasterror("GetQueuedCompletionStatus");
        errno = EIO;
        return (-1);
    }
}

int
windows_kevent_copyout(struct kqueue *kq, int nready,
        struct kevent *eventlist, int nevents)
{
    struct filter *filt;
    struct knote* kn;
    int rv;
    int produced = 0;
    unsigned long batch_seq;

    /*
     * Bump the per-kqueue dispatch sequence.  Each knote we
     * successfully dispatch in this call has its kn_dispatch_seq
     * stamped with batch_seq; the drain leg below uses this to
     * recognise duplicate level-trigger self-posts for a knote we
     * already delivered (e.g. the EV_EOF re-level for sockets and
     * pipes) and discard them instead of stacking the same event
     * many times into the caller's eventlist.
     */
    if (++kq->kq_dispatch_seq == 0)
        kq->kq_dispatch_seq = 1;
    batch_seq = kq->kq_dispatch_seq;

    /*
     * Drain stale completions in a loop.  Synthetic re-posts and
     * thread-pool callbacks can race with EV_DELETE such that a
     * knote pointer hits the IOCP just after the knote was
     * KNFL_KNOTE_DELETED'd; the per-filter copyouts recognise that
     * and discard with dst->filter = 0.  We must consume those
     * silently and look at the next IOCP entry rather than return
     * a fake "no events" up to the caller, who's blocked in
     * kevent() expecting a real wakeup.
     *
     * After a successful dispatch we also drain the IOCP queue
     * non-blocking so a single kevent() call can deliver every
     * completion that was already ready.  BSD kqueue returns up
     * to nevents in one call; pipe_eof_multi (and any consumer
     * watching multiple fds that wake together) needs that.
     */
    for (;;) {
        short fid;

        eventlist->filter = 0;

        /*
         * Close-detect (kernel-posted via the kq_close_read pipe):
         * consumer's close(kqfd) dropped the last write reference.
         * Schedule the deferred free on the thread pool and return
         * EBADF so the kevent() caller unwinds without dispatching
         * anything from this batch.
         */
        if (iocp_buf.key == KQ_CLOSE_DETECT_KEY) {
            dbg_puts("close-detect completion - kq fd was closed");
            atomic_store(&kq->kq_consumer_closed, 1);
            /*
             * Leave kq->kq_id alone: kqueue_free's map_remove
             * needs the original index to clear the slot, and
             * windows_kqueue_free's "should I _close this fd?"
             * decision is gated on kq_consumer_closed below.
             * (Setting kq_id = -1 here was what stranded the kq
             * pointer in kqmap[idx] - map_remove with idx == -1
             * is a no-op, so a subsequent kqueue() that recycled
             * the same fd resolved kqueue_lookup back to a
             * pointer the cleanup callback was about to free.)
             */
            if (!TrySubmitThreadpoolCallback(windows_kqueue_close_cleanup,
                                              kq, NULL)) {
                dbg_lasterror("TrySubmitThreadpoolCallback");
                /*
                 * Cleanup couldn't be scheduled; the kq leaks
                 * until process exit's libkqueue_free walks
                 * kq_list.  Bounded, not catastrophic.
                 */
            }
            errno = EBADF;
            return -1;
        }

        /*
         * Bare wake-up sentinel posted by windows_kqueue_interrupt
         * (key == 0, overlap == NULL).  Nothing to dispatch; let
         * the drain loop pick up the next entry, or return 0 if
         * the queue is empty.
         */
        if (iocp_buf.key == 0 && iocp_buf.overlap == NULL)
            goto drain_more;

        /*
         * Recover the owning knote and the filter id.  Two shapes
         * land here:
         *
         *   - KQ_PIPE_READ_KEY: kernel-posted overlapped read on a
         *     pipe HANDLE.  overlap = &knote->kn_read.pipe_ov; the
         *     knote sits at a known offset from it and the filter
         *     id is read from the recovered knote.
         *
         *   - KQ_FILTER_KEY(fid): synthetic post (filter callback
         *     re-arming a knote, or eventfd doorbell waking a
         *     filter).  Key encodes the filter id; overlap is the
         *     knote pointer, or NULL for the doorbell case where
         *     the filter drains its own pending state without a
         *     specific knote.
         */
        if (iocp_buf.key == KQ_PIPE_READ_KEY) {
            kn  = (struct knote *)((char *) iocp_buf.overlap -
                                   offsetof(struct knote, kn_read.pipe_ov));
            fid = kn->kev.filter;
        } else {
            fid = (short)(LONG_PTR) iocp_buf.key;
            kn  = (struct knote *) iocp_buf.overlap;   /* NULL for doorbell */
        }

        /*
         * Dedup duplicate level-trigger self-posts for a knote
         * already delivered in this batch (e.g. the EV_EOF re-
         * level for sockets and pipes).  Re-queue for the next
         * kevent() call rather than stacking the same event into
         * the caller's eventlist.
         */
        if (kn != NULL && produced > 0 && kn->kn_dispatch_seq == batch_seq) {
            dbg_puts("drain: duplicate level-trigger for already-"
                     "delivered knote, re-queueing for next kevent()");
            PostQueuedCompletionStatus(kq->kq_iocp, 1,
                                       KQ_FILTER_KEY(kn->kev.filter),
                                       (LPOVERLAPPED) kn);
            return produced;
        }

        if (filter_lookup(&filt, kq, fid) < 0) {
            dbg_printf("completion with unsupported filter id %d", fid);
            goto drain_more;
        }

        /*
         * Stamp the dispatch seq BEFORE kf_copyout: the filter may
         * release the knote (EV_ONESHOT runs knote_delete +
         * knote_release inside flag_actions), so any read of kn
         * after the call is a use-after-free.  asan caught exactly
         * that on test_kevent_threading_close_multi.
         */
        if (kn != NULL)
            kn->kn_dispatch_seq = batch_seq;

        rv = filt->kf_copyout(eventlist, nevents, filt, kn, &iocp_buf);
        if (unlikely(rv < 0)) {
            dbg_puts("knote_copyout failed");
            abort();
        }
        if (rv > 0 && eventlist->filter != 0) {
            eventlist += rv;
            nevents -= rv;
            produced += rv;
            if (nevents <= 0)
                return produced;
        }

drain_more:
        /*
         * Drain additional completions: stale-knote discards
         * (re-block with the original budget so we don't return
         * a phantom "0 events" to a caller that was expecting a
         * real wakeup) or successful dispatches that we want to
         * stack into the same eventlist.  When we already
         * produced at least one event we use timeout=0 so the
         * call returns promptly with whatever was already ready,
         * matching BSD batch semantics.
         */
        {
            DWORD wait_ms = (produced > 0) ? 0 : iocp_wait_timeout_ms;
            dbg_printf("draining IOCP queue timeout=%u, produced=%d",
                       (unsigned int) wait_ms, produced);
            memset(&iocp_buf, 0, sizeof(iocp_buf));
            if (!GetQueuedCompletionStatus(kq->kq_iocp,
                    &iocp_buf.bytes, &iocp_buf.key, &iocp_buf.overlap,
                    wait_ms)) {
                if (GetLastError() == WAIT_TIMEOUT)
                    return produced;
                if (iocp_buf.overlap == NULL) {
                    dbg_lasterror("GetQueuedCompletionStatus(drain)");
                    return produced;
                }
                /*
                 * Failed-I/O completion (e.g. ERROR_BROKEN_PIPE on a
                 * pipe ReadFile): this is a real event we still need
                 * to dispatch, fall through to the loop body.
                 */
            }
        }
    }
}

/*
 * Inflight tracking for KEVENT_WAIT_DROP_LOCK.  The common kevent()
 * entry path stack-allocates a struct kqueue_kevent_state and calls
 * these around the wait/copyout block; kqueue_free observes a non-
 * empty kq_inflight under kq_mtx + per-kq lock and defers its
 * teardown until the last waiter exits.  Without this, a thread
 * parked inside windows_kevent_wait (with the lock dropped) would
 * return into freed memory when it called kqueue_lock(kq) on the
 * other side - which is exactly what asan caught on
 * threading_close_multi.
 */
void
windows_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    tracing_mutex_assert_state(&kq->kq_mtx, MTX_LOCKED);
    TAILQ_INSERT_TAIL(&kq->kq_inflight, state, entry);
}

void
windows_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    tracing_mutex_assert_state(&kq->kq_mtx, MTX_LOCKED);
    TAILQ_REMOVE(&kq->kq_inflight, state, entry);
}

int
windows_filter_init(struct kqueue *kq, struct filter *kf)
{

    kq->kq_filt_ref[kq->kq_filt_count] = (struct filter *) kf;
    kq->kq_filt_count++;

    return (0);
}

void
windows_filter_free(struct kqueue *kq, struct filter *kf)
{

}

static void
windows_libkqueue_init(void)
{
    WSADATA wsaData;

    /*
     * Bring up the global kq_mtx CRITICAL_SECTION before common
     * code can lock it.  POSIX backends rely on
     * PTHREAD_MUTEX_INITIALIZER static init; Win32's
     * CRITICAL_SECTION needs a runtime InitializeCriticalSection,
     * which tracing_mutex_init wraps.
     */
    tracing_mutex_init(&kq_mtx, NULL);

    /*
     * Winsock startup must succeed for any socket-shaped filter
     * (EVFILT_READ / EVFILT_WRITE on AF_INET, FD_CLOSE detection
     * via WSAEventSelect, etc.) to function.  Used to live inside
     * libkqueue_init's KQUEUE_DEBUG branch in common code, which
     * meant Release builds without the env var never started
     * Winsock at all.
     */
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
        abort();

#if !defined(NDEBUG) && !defined(__GNUC__)
    /*
     * MSVC Debug CRT heap surveillance: opt in only when the user
     * set KQUEUE_DEBUG=1, mirroring the original common-code
     * behaviour.  -Werror'd builds without __GNUC__ pull in
     * <crtdbg.h> via private.h.
     */
    {
        char *s = getenv("KQUEUE_DEBUG");
        if (s && *s && *s != '0') {
            int tmpFlag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
            tmpFlag |= _CRTDBG_CHECK_ALWAYS_DF;
            _CrtSetDbgFlag(tmpFlag);
        }
    }
#endif

    /*
     * Hook CTRL_C / CTRL_BREAK / window-close / logoff / shutdown
     * so an external signal-shaped event reaches any kqueue
     * watcher EVFILT_SIGNAL'd on the matching signum.  Failures
     * are non-fatal: the rest of the library is still usable;
     * just no console-control bridge.
     */
    (void) windows_signal_init();
}

const struct kqueue_vtable kqops = {
    .libkqueue_init     = windows_libkqueue_init,
#ifndef NDEBUG
    .libkqueue_dbg_default = windows_dbg_default,
#endif

    .kqueue_init        = windows_kqueue_init,
    .kqueue_free        = windows_kqueue_free,
    .kqueue_interrupt   = windows_kqueue_interrupt,
    .kevent_wait        = windows_kevent_wait,
    .kevent_copyout     = windows_kevent_copyout,
    .filter_init        = windows_filter_init,
    .filter_free        = windows_filter_free,
    .eventfd_register   = windows_eventfd_register,
    .eventfd_unregister = windows_eventfd_unregister,
    .eventfd_init       = windows_eventfd_init,
    .eventfd_close      = windows_eventfd_close,
    .eventfd_raise      = windows_eventfd_raise,
    .eventfd_lower      = windows_eventfd_lower,
    .eventfd_descriptor = windows_eventfd_descriptor,
};
