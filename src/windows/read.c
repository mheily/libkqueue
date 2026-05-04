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

/*
 * EVFILT_READ on Win32 covers four very different descriptor flavours
 * (regular file, anonymous/named pipe HANDLE, passive listening
 * socket, regular SOCKET) that share almost no syscall surface.  Each
 * flavour has its own create/delete/copyout helper below; the public
 * dispatch entrypoints are thin if-ladders that pick the right one
 * off kn_flags.
 */

/*
 * Per-flavour copyout fill helpers return a value matching the
 * outer evfilt_read_copyout contract:
 *   1  - dst is populated, continue to flag-actions / re-arm
 *   0  - drop this completion (release ref, set dst->filter=0)
 *  -1  - hard error
 */
#define READ_COPYOUT_OK         1
#define READ_COPYOUT_DROP       0   /* release ref, dst->filter=0, return 0 */
#define READ_COPYOUT_ERR       (-1)
#define READ_COPYOUT_FILE_EOF   2   /* dst->filter=0, return 0 (no release) */

static VOID CALLBACK
evfilt_read_callback(void *param, BOOLEAN fired)
{
    WSANETWORKEVENTS events;
    struct kqueue *kq;
    struct knote *kn;
    int rv;

    assert(param);

    if (fired) {
        dbg_puts("called, but event was not triggered(?)");
        return;
    }

    kn = (struct knote *)param;
    kq = kn->kn_kq;
    assert(kq);

    /* Retrieve the socket events and update the knote */
    rv = WSAEnumNetworkEvents(
            (SOCKET) kn->kev.ident,
            kn->kn_handle,
                &events);
    if (rv != 0) {
        dbg_wsalasterror("WSAEnumNetworkEvents");
        return;
    }

    /*
     * Edge-trigger emulation for EV_CLEAR/EV_DISPATCH sockets.
     * WSAEventSelect re-records FD_READ after a partial recv even
     * though no fresh data has arrived; Linux/BSD edge semantics
     * only fire on a 0-or-shrinking-bytes -> grew transition.
     *
     * Compare current FIONREAD against the byte count we last
     * delivered (snapshotted in copyout) and suppress the post if
     * we'd be re-firing without genuinely new data.  FD_CLOSE/
     * FD_ACCEPT bypass this check - those are real edges.
     */
    if (events.lNetworkEvents & FD_CLOSE) {
        atomic_store(&kn->kn_read.eof, 1);
        /*
         * iErrorCode[FD_CLOSE_BIT] carries the close reason: 0 for
         * a graceful FIN, non-zero (typically WSAECONNRESET) for an
         * RST.  Stash for copyout to surface as fflags.
         */
        atomic_store(&kn->kn_read.so_error,
                     events.iErrorCode[FD_CLOSE_BIT]);
    }

    if (kn->kev.flags & (EV_CLEAR | EV_DISPATCH)) {
        unsigned long now_bytes = 0;
        int last;
        bool real_edge = (events.lNetworkEvents & (FD_CLOSE | FD_ACCEPT)) != 0;

        if (!real_edge) {
            if (ioctlsocket(kn->kev.ident, FIONREAD, &now_bytes) != 0)
                now_bytes = 0;
            last = atomic_load(&kn->kn_read.last_data);
            if ((int)now_bytes <= last) {
                /* No fresh data; remember the current floor so a
                 * later genuine arrival above it re-fires. */
                atomic_store(&kn->kn_read.last_data, (int)now_bytes);
                return;
            }
        }
    }

    /*
     * Retain a ref for the queued completion so an EV_DELETE
     * arriving before the dispatcher drains can't free the knote
     * out from under us (same UAF pattern as the synthetic file
     * paths and timer/vnode callbacks).  Released in copyout.
     */
    knote_retain(kn);
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) kn)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return;
    }
}

/*
 * Regular file: report bytes-remaining-to-EOF, matching the Linux
 * read filter.  At EOF discard the event by zeroing dst->filter (the
 * common layer drops events with filter==0) and stop the synthetic
 * re-arm cycle, so a subsequent test_no_kevents on a fully-consumed
 * file sees nothing pending.  A consumer that seeks back and
 * re-enables the knote will pick the new range up via the next
 * knote_create.
 */
static __inline int
evfilt_read_copyout_file(struct kevent *dst, struct knote *src)
{
    struct _stat64 sb;
    __int64 curpos;
    intptr_t remaining = 0;

    if (_fstat64((int)src->kev.ident, &sb) == 0) {
        curpos = _lseeki64((int)src->kev.ident, 0, SEEK_CUR);
        if (curpos < 0) curpos = 0;
        remaining = (sb.st_size > curpos) ? (intptr_t)(sb.st_size - curpos) : 0;
    }
    if (remaining == 0) {
        src->kn_read.file_synthetic = 0;
        return READ_COPYOUT_FILE_EOF;
    }
    dst->data = remaining;
    return READ_COPYOUT_OK;
}

/*
 * Pipe HANDLE: state is in kn_read.eof if a previous completion already
 * detected the broken-pipe (the post path sets it either via the
 * WSAEnumNetworkEvents-style call or via the synth-failure path in
 * the post-flag-actions block below).  If kn_read.eof is already set, the
 * completion came via the synthetic re-post (key=0, overlap=knote
 * ptr) and we should NOT touch &kn_read.pipe_ov.  Otherwise the completion
 * came from the overlapped ReadFile via KQ_PIPE_READ_KEY and
 * GetOverlappedResult tells us the outcome.
 */
static __inline int
evfilt_read_copyout_pipe(struct kevent *dst, struct knote *src)
{
    unsigned long avail = 0;

    dbg_puts("pipe copyout entered");
    if (atomic_load(&src->kn_read.eof)) {
        dst->flags |= EV_EOF;
    } else {
        DWORD got = 0;
        BOOL  ok  = GetOverlappedResult(src->kn_handle, &src->kn_read.pipe_ov,
                                        &got, FALSE);
        if (!ok) {
            switch (GetLastError()) {
            case ERROR_BROKEN_PIPE:
            case ERROR_HANDLE_EOF:
            case ERROR_PIPE_NOT_CONNECTED:
                atomic_store(&src->kn_read.eof, 1);
                dst->flags |= EV_EOF;
                break;
            case ERROR_OPERATION_ABORTED:
                /* CancelIoEx from delete; discard. */
                return READ_COPYOUT_DROP;
            default:
                dbg_lasterror("GetOverlappedResult(pipe)");
                atomic_store(&src->kn_read.eof, 1);
                dst->flags |= EV_EOF;
                break;
            }
        }
    }
    if (src->kn_handle != NULL &&
        PeekNamedPipe(src->kn_handle, NULL, 0, NULL,
                      (DWORD *) &avail, NULL))
        dst->data = (intptr_t) avail;
    else
        dst->data = 0;
    return READ_COPYOUT_OK;
}

/* TODO: should contain the length of the socket backlog */
static __inline int
evfilt_read_copyout_socket_passive(struct kevent *dst, UNUSED struct knote *src)
{
    dst->data = 1;
    return READ_COPYOUT_OK;
}

static __inline int
evfilt_read_copyout_socket(struct kevent *dst, struct knote *src)
{
    unsigned long bufsize;

    if (ioctlsocket(src->kev.ident, FIONREAD, &bufsize) != 0) {
        dbg_wsalasterror("ioctlsocket");
        return READ_COPYOUT_ERR;
    }
    dst->data = bufsize;

    /*
     * Edge-trigger snapshot for EV_CLEAR/EV_DISPATCH sockets:
     * remember the byte count we just delivered so the
     * WSAEventSelect callback can suppress re-assertions that don't
     * represent fresh data (a partial recv re-records FD_READ on
     * Win32 even though the level didn't transition).
     */
    if (src->kev.flags & (EV_CLEAR | EV_DISPATCH))
        atomic_store(&src->kn_read.last_data, (int)bufsize);

    if (atomic_load(&src->kn_read.eof)) {
        int serr;
        dst->flags |= EV_EOF;
        /*
         * Surface the captured close-reason as fflags (parity with
         * posix/read.c).  0 stays 0 (clean FIN); a non-zero WSA
         * error gets propagated.
         */
        serr = atomic_load(&src->kn_read.so_error);
        if (serr != 0)
            dst->fflags = (unsigned int) serr;
    }
    return READ_COPYOUT_OK;
}

int
evfilt_read_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, UNUSED void *ptr)
{
    int rv;

    /*
     * Stale completion: callback or synthetic re-post raced with
     * EV_DELETE.  Must check before any ident-based syscall (e.g.
     * ioctlsocket below); the underlying fd may already have been
     * close()d and recycled to a different kind of descriptor by
     * the test harness (sockets recycled to a regular file, etc.),
     * turning the syscall into spurious failure.
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    /*
     * EV_DISABLE'd knote: the consumer asked us to suppress
     * delivery until EV_ENABLE.  A completion may already have been
     * posted by the WSAEventSelect callback before disable_knote
     * tore the wait down; drop it cleanly.
     */
    if (src->kev.flags & EV_DISABLE) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    memcpy(dst, &src->kev, sizeof(*dst));

    if (src->kn_flags & KNFL_FILE)
        rv = evfilt_read_copyout_file(dst, src);
    else if (src->kn_flags & KNFL_PIPE)
        rv = evfilt_read_copyout_pipe(dst, src);
    else if (src->kn_flags & KNFL_SOCKET_PASSIVE)
        rv = evfilt_read_copyout_socket_passive(dst, src);
    else
        rv = evfilt_read_copyout_socket(dst, src);

    if (rv == READ_COPYOUT_DROP) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }
    if (rv == READ_COPYOUT_FILE_EOF) {
        dst->filter = 0;
        return (0);
    }
    if (rv == READ_COPYOUT_ERR)
        return (-1);

    /*
     * Snapshot before flag_actions: EV_ONESHOT inside that helper
     * does knote_delete + knote_release.  The post took one ref, so
     * 2->1 = still alive when we return - but a third
     * EV_ONESHOT-from-some-other-path could free.  Be defensive and
     * don't read src->* fields after flag_actions; use locals.
     */
    {
        bool is_synthetic = src->kn_read.file_synthetic;
        bool is_pipe = (src->kn_flags & KNFL_PIPE) != 0;
        bool is_disabled = (src->kev.flags & EV_DISABLE) != 0;
        bool is_deleted = (src->kn_flags & KNFL_KNOTE_DELETED) != 0;

        /*
         * Once a socket peer has closed, FD_CLOSE only fires once
         * on Win32; the auto-reset event won't re-trigger.  For
         * level-triggered (no EV_CLEAR/EV_DISPATCH) sockets the
         * consumer expects EV_EOF to keep firing, so re-arm
         * synthetically while the knote remains armed.
         */
        int eof_relevel = (dst->flags & EV_EOF) &&
                          !(src->kev.flags & (EV_CLEAR | EV_DISPATCH));
        struct kqueue *kq = src->kn_kq;

        if (knote_copyout_flag_actions(filt, src) < 0) {
            knote_release(src);
            return -1;
        }

        /*
         * Synthetic file sources re-arm by re-posting; the post's
         * ref hands off into the queued entry.
         * Pipes re-arm by issuing a fresh overlapped ReadFile - the
         * IOCP delivers the next completion when data arrives or
         * the writer closes (level-triggered EOF replays).  Sockets
         * just release - the next FD_READ the WSAEventSelect
         * callback handles will retain again.
         */
        if (is_pipe && !is_disabled) {
            if (is_deleted || src->kn_handle == NULL) {
                knote_release(src);
            } else if (atomic_load(&src->kn_read.eof)) {
                /*
                 * Pipe is broken: don't keep issuing overlapped
                 * ReadFile against a dead handle (every retry
                 * fails synchronously anyway), and don't leave an
                 * OVERLAPPED association the CRT thinks is pending
                 * - that hangs _close on the user fd.  Re-post via
                 * knote-pointer + key=0 like the synthetic-file
                 * path; the dispatcher routes it back into
                 * evfilt_read_copyout which sees kn_read.eof and
                 * delivers another EV_EOF.
                 */
                if (!PostQueuedCompletionStatus(kq->kq_iocp, 1,
                                                KQ_FILTER_KEY(src->kev.filter),
                                                (LPOVERLAPPED) src)) {
                    dbg_lasterror("PostQueuedCompletionStatus(pipe-eof relevel)");
                    knote_release(src);
                }
            } else {
                memset(&src->kn_read.pipe_ov, 0, sizeof(src->kn_read.pipe_ov));
                if (!ReadFile(src->kn_handle, src->kn_read.pipe_buf, 0, NULL,
                              &src->kn_read.pipe_ov)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        /*
                         * ReadFile failed synchronously (peer just
                         * closed) - latch EOF and re-post via the
                         * knote-pointer path so the next wait
                         * delivers cleanly without a phantom
                         * overlapped on the handle.
                         */
                        atomic_store(&src->kn_read.eof, 1);
                        if (!PostQueuedCompletionStatus(kq->kq_iocp, 1,
                                                        KQ_FILTER_KEY(src->kev.filter),
                                                        (LPOVERLAPPED) src))
                            knote_release(src);
                    }
                    /* ERROR_IO_PENDING: completion will arrive via IOCP. */
                } /* synchronous success: completion will arrive via IOCP. */
            }
        } else if ((is_synthetic || eof_relevel) && !is_disabled) {
            bool is_deleted = (src->kn_flags & KNFL_KNOTE_DELETED) != 0;
            if (!is_deleted) {
                if (!PostQueuedCompletionStatus(kq->kq_iocp, 1,
                                                KQ_FILTER_KEY(src->kev.filter),
                                                (LPOVERLAPPED) src)) {
                    dbg_lasterror("PostQueuedCompletionStatus()");
                    knote_release(src);
                }
                /* re-armed: ref handed off to the queued entry */
            } else {
                knote_release(src);
            }
        } else {
            knote_release(src);
        }
    }

    return (1);
}

/*
 * Regular files: synthesise a "level-triggered, always readable"
 * source.  Post one completion now and let evfilt_read_copyout
 * re-post on each drain while the knote remains armed.  No
 * WSAEventSelect/wait registration; that machinery is socket-only on
 * Win32.
 */
static __inline int
evfilt_read_knote_create_file(struct knote *kn)
{
    kn->kn_handle = NULL;
    kn->kn_event_whandle = NULL;
    kn->kn_read.file_synthetic = 1;
    /* See evfilt_write_knote_create for the retain/release rationale. */
    knote_retain(kn);
    if (!PostQueuedCompletionStatus(kn->kn_kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) kn)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return (-1);
    }
    return (0);
}

/*
 * Pipe HANDLE: WSAEventSelect doesn't apply.  Attach the pipe to
 * kq_iocp under KQ_PIPE_READ_KEY and issue a 0-byte overlapped
 * ReadFile.  When the writer sends data the read completes with
 * bytes-read=0 (we asked for 0); when the writer closes its end, the
 * read completes with ERROR_BROKEN_PIPE.  Either way the IOCP
 * delivers the completion, the dispatcher recovers the knote via
 * CONTAINING_RECORD on kn_read.pipe_ov, and copyout drains.
 */
static __inline int
evfilt_read_knote_create_pipe(struct knote *kn)
{
    HANDLE ph = (HANDLE) _get_osfhandle((int) kn->kev.ident);

    if (ph == INVALID_HANDLE_VALUE) {
        dbg_lasterror("_get_osfhandle on pipe");
        return (-1);
    }
    dbg_printf("pipe knote: ident=%d ph=%p", (int) kn->kev.ident, ph);
    /*
     * IOCP association is permanent for the life of the HANDLE -
     * Windows has no Disassociate API.  After an EV_DELETE+EV_ADD
     * churn (threading_read_delete_race iterates this 200 times)
     * the handle is already bound to kq_iocp under KQ_PIPE_READ_KEY,
     * and a fresh CreateIoCompletionPort returns NULL with
     * ERROR_INVALID_PARAMETER.  Since we always bind pipes with the
     * same key, treat that as 'already associated the way we
     * wanted' and proceed.
     */
    if (CreateIoCompletionPort(ph, kn->kn_kq->kq_iocp,
                               KQ_PIPE_READ_KEY, 0) == NULL) {
        DWORD err = GetLastError();
        if (err != ERROR_INVALID_PARAMETER) {
            dbg_lasterror("CreateIoCompletionPort(pipe)");
            return (-1);
        }
        dbg_puts("pipe knote: handle already associated with kq_iocp - reusing");
    } else {
        dbg_puts("pipe knote: associated with kq_iocp under KQ_PIPE_READ_KEY");
    }
    kn->kn_handle = ph;
    kn->kn_event_whandle = NULL;
    memset(&kn->kn_read.pipe_ov, 0, sizeof(kn->kn_read.pipe_ov));
    knote_retain(kn);
    /*
     * 0-byte overlapped read: peek-style pend that wakes on
     * data-available or pipe-broken WITHOUT consuming any bytes
     * from the pipe.  The earlier comment in this file claimed
     * 0-byte reads return synchronously and defeat the IOCP wake;
     * that is true for synchronous handles but FILE_FLAG_OVERLAPPED
     * + lpOverlapped non-NULL pends in the IOCP just like the
     * 1-byte form did.  Doing 1-byte reads consumed the consumer's
     * data, which broke threading_read_delete_race - the test's
     * own read() blocked because libkqueue had already stolen the
     * single byte the writer pushed in.
     */
    if (!ReadFile(ph, kn->kn_read.pipe_buf, 0, NULL, &kn->kn_read.pipe_ov)) {
        DWORD err = GetLastError();
        dbg_printf("pipe ReadFile returned FALSE, GLE=%lu", (unsigned long) err);
        if (err != ERROR_IO_PENDING) {
            dbg_lasterror("ReadFile(pipe overlapped)");
            /*
             * On synchronous failure the IOCP doesn't get a
             * completion - balance the retain inline.
             */
            knote_release(kn);
            return (-1);
        }
    } else {
        dbg_puts("pipe ReadFile returned TRUE (synchronous completion)");
    }
    return (0);
}

static __inline int
evfilt_read_knote_create_socket(struct knote *kn)
{
    HANDLE evt;
    int rv;

    /* Create an auto-reset event object */
    evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (evt == NULL) {
        dbg_lasterror("CreateEvent()");
        return (-1);
    }

    rv = WSAEventSelect(
                (SOCKET) kn->kev.ident,
                evt,
                FD_READ | FD_ACCEPT | FD_CLOSE);
    if (rv != 0) {
        dbg_wsalasterror("WSAEventSelect()");
        CloseHandle(evt);
        return (-1);
    }

    /*
     * WSAEventSelect on a socket with already-pending FD_READ /
     * FD_ACCEPT / FD_CLOSE state may or may not auto-set the event
     * depending on Win32 SKU and timing - empirically it fires for
     * EV_ENABLE re-arm of a previously-created watch but not always
     * for a fresh EV_ADD.  Reset the event unconditionally so the
     * wait registration sees a known cleared edge, and synthesise
     * the wakeup ourselves below if there's data buffered.  That
     * way the EV_ADD path doesn't accidentally double-fire while
     * EV_ENABLE / EV_DISPATCH re-arm still works.
     */
    ResetEvent(evt);

    kn->kn_handle = evt;
    atomic_store(&kn->kn_read.last_data, 0);

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, evt,
        evfilt_read_callback, kn, INFINITE, 0) == 0) {
        dbg_puts("RegisterWaitForSingleObject failed");
        CloseHandle(evt);
        return (-1);
    }

    /*
     * Level-triggered fire-on-enable: if the socket already has
     * data buffered when we (re)arm the watch, post one completion
     * explicitly.  WSAEventSelect's auto-reset event will only fire
     * once a fresh FD_READ is recorded, which doesn't happen if no
     * recv has occurred since the prior delivery, so the consumer
     * would otherwise miss the EV_DISPATCH / EV_ENABLE re-arm
     * wakeup.  Skipped for passive listeners: FD_ACCEPT carries no
     * FIONREAD signal.
     */
    {
        unsigned long pending = 0;
        if (!(kn->kn_flags & KNFL_SOCKET_PASSIVE) &&
            ioctlsocket((SOCKET)kn->kev.ident, FIONREAD, &pending) == 0 &&
            pending > 0) {
            knote_retain(kn);
            if (!PostQueuedCompletionStatus(kn->kn_kq->kq_iocp, 1,
                                            KQ_FILTER_KEY(kn->kev.filter),
                                            (LPOVERLAPPED) kn)) {
                dbg_lasterror("PostQueuedCompletionStatus()");
                knote_release(kn);
            }
        }
    }

    return (0);
}

int
evfilt_read_knote_create(UNUSED struct filter *filt, struct knote *kn)
{
    if (windows_get_descriptor_type(kn) < 0)
        return (-1);

    if (kn->kn_flags & KNFL_FILE)
        return evfilt_read_knote_create_file(kn);
    if (kn->kn_flags & KNFL_PIPE)
        return evfilt_read_knote_create_pipe(kn);
    return evfilt_read_knote_create_socket(kn);
}

/*
 * Synthetic file source: no Win32 wait registration to tear down,
 * just clear the synthetic flag so any IOCP entry that was already in
 * flight gets discarded by copyout's KNFL_KNOTE_DELETED check (set by
 * the common layer around this call).
 */
static __inline int
evfilt_read_knote_delete_file(struct knote *kn)
{
    kn->kn_read.file_synthetic = 0;
    return (0);
}

/*
 * Pipe HANDLE: cancel the pending overlapped ReadFile.  We do NOT
 * wait on GetOverlappedResult here.  The HANDLE is associated with
 * kq_iocp and the OVERLAPPED has no hEvent, so
 * GetOverlappedResult(bWait=TRUE) on an IOCP-bound handle can block
 * indefinitely (MSDN, GetOverlappedResult Remarks).  The kernel I/O
 * packet still completes promptly with STATUS_CANCELLED and is queued
 * to the IOCP, which copyout picks up later and discards via
 * KNFL_KNOTE_DELETED.  CRT _close / CloseHandle on the fd is safe at
 * that point because the in-kernel I/O has already finished, even if
 * the completion has not yet been dequeued.
 */
static __inline int
evfilt_read_knote_delete_pipe(struct knote *kn)
{
    if (kn->kn_handle != NULL)
        (void) CancelIoEx(kn->kn_handle, &kn->kn_read.pipe_ov);
    kn->kn_handle = NULL;
    return (0);
}

static __inline int
evfilt_read_knote_delete_socket(struct knote *kn)
{
    if (kn->kn_handle == NULL || kn->kn_event_whandle == NULL)
        return (0);

    if (!UnregisterWaitEx(kn->kn_event_whandle, INVALID_HANDLE_VALUE)) {
        dbg_lasterror("UnregisterWait()");
        return (-1);
    }
    if (!WSACloseEvent(kn->kn_handle)) {
        dbg_wsalasterror("WSACloseEvent()");
        return (-1);
    }

    kn->kn_handle = NULL;
    kn->kn_event_whandle = NULL;
    return (0);
}

int
evfilt_read_knote_delete(UNUSED struct filter *filt, struct knote *kn)
{
    if (kn->kn_read.file_synthetic)
        return evfilt_read_knote_delete_file(kn);
    if (kn->kn_flags & KNFL_PIPE)
        return evfilt_read_knote_delete_pipe(kn);
    return evfilt_read_knote_delete_socket(kn);
}

int
evfilt_read_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * Pipe HANDLE modify: don't tear down + re-attach the pipe to
     * kq_iocp.  CreateIoCompletionPort on an already-associated
     * handle fails with ERROR_INVALID_PARAMETER, and the overlapped
     * ReadFile is still pending and still wired to the right knote,
     * so a stop+start cycle has nothing to gain.  Just refresh the
     * dynamic kev fields and return.
     */
    if (kn->kn_flags & KNFL_PIPE) {
        kn->kev.fflags = kev->fflags;
        kn->kev.data   = kev->data;
        return (0);
    }

    /*
     * No native modify on Win32; tear down the WSAEventSelect/wait
     * pair and re-create.  BSD treats kev.flags on sockets as sticky
     * from initial EV_ADD (EV_CLEAR / EV_DISPATCH / EV_RECEIPT do
     * not change on modify).  Common code does not pre-merge kev so
     * update fflags / data here, leave flags alone.
     */
    if (evfilt_read_knote_delete(filt, kn) < 0)
        return (-1);

    kn->kev.fflags = kev->fflags;
    kn->kev.data   = kev->data;

    return evfilt_read_knote_create(filt, kn);
}

int
evfilt_read_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_read_knote_create(filt, kn);
}

int
evfilt_read_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_read_knote_delete(filt, kn);
}

const struct filter evfilt_read = {
    .kf_id      = EVFILT_READ,
    .kf_copyout = evfilt_read_copyout,
    .kn_create  = evfilt_read_knote_create,
    .kn_modify  = evfilt_read_knote_modify,
    .kn_delete  = evfilt_read_knote_delete,
    .kn_enable  = evfilt_read_knote_enable,
    .kn_disable = evfilt_read_knote_disable,
};
