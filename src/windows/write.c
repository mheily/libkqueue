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

static VOID CALLBACK
evfilt_write_callback(void *param, BOOLEAN fired)
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

    rv = WSAEnumNetworkEvents((SOCKET) kn->kev.ident,
                              kn->kn_handle,
                              &events);
    if (rv != 0) {
        dbg_wsalasterror("WSAEnumNetworkEvents");
        return;
    }

    /* Retain across the IOCP queue so EV_DELETE can't free us first. */
    knote_retain(kn);
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) param)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return;
    }
}

int
evfilt_write_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr)
{
    bool           is_synthetic;
    struct kqueue *kq;

    /*
     * Stale completion - re-post raced with EV_DELETE.  Must
     * check before any ident-based syscall (getsockopt below);
     * the underlying fd may already have been close()d and
     * recycled to a different descriptor type.
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    /*
     * EV_DISABLE'd write knote: a callback posted before disable
     * arrived is still queued; drop it.
     */
    if (src->kev.flags & EV_DISABLE) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    memcpy(dst, &src->kev, sizeof(*dst));

    /*
     * Fill dst->data and (for pipes) dst->flags |= EV_EOF based on
     * the descriptor type.  Each branch is its own little story;
     * keeping them flat here is clearer than nesting.
     */
    if (src->kn_flags & KNFL_PIPE) {
        /*
         * Pipe writer: probe for peer-gone with a 0-byte
         * WriteFile.  Win32 has no FD_CLOSE-style edge for pipes,
         * so detect it lazily on each drain.  Once latched,
         * surface EV_EOF on every subsequent delivery (BSD
         * level-trigger) and let the synthetic re-post path keep
         * firing.
         */
        DWORD out_buf = 0;
        if (!src->kn_write.pipe_eof && src->kn_handle != NULL) {
            DWORD wrote = 0;
            if (!WriteFile(src->kn_handle, "", 0, &wrote, NULL)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
                    err == ERROR_PIPE_NOT_CONNECTED)
                    src->kn_write.pipe_eof = 1;
            }
        }
        if (src->kn_write.pipe_eof)
            dst->flags |= EV_EOF;
        /*
         * BSD filt_pipewrite reports kn_data = pipe_buffer.size -
         * pipe_buffer.cnt (free outbound buffer slack).  Win32
         * has no direct outstanding-bytes query; GetNamedPipeInfo
         * returns the allocated buffer size, the right upper
         * bound when nothing is in flight.
         */
        if (src->kn_handle != NULL &&
            GetNamedPipeInfo(src->kn_handle, NULL, &out_buf, NULL, NULL))
            dst->data = (intptr_t) out_buf;
        else
            dst->data = 0;
    } else if (src->kn_flags & KNFL_FILE) {
        /*
         * Regular files are always considered writable.  No
         * Windows analogue of SIOCOUTQ so report 0 (writable, no
         * known outstanding bytes).
         */
        dst->data = 0;
    } else {
        /*
         * Sockets: report the available send buffer space.  Use
         * SO_SNDBUF as the upper bound; Win32 has no exact
         * analogue of Linux SIOCOUTQ.
         */
        int sndbuf = 0;
        int slen = sizeof(sndbuf);
        if (getsockopt((SOCKET)src->kev.ident, SOL_SOCKET, SO_SNDBUF,
                       (char *)&sndbuf, &slen) == 0)
            dst->data = sndbuf;
        else
            dst->data = 0;
    }

    is_synthetic = src->kn_write.file_synthetic;
    kq           = src->kn_kq;

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return (-1);
    }

    /*
     * flag_actions may have run EV_ONESHOT and set
     * KNFL_KNOTE_DELETED.  Sockets and post-delete knotes have
     * nothing to re-arm; just release the completion's ref.
     */
    if (!is_synthetic || (src->kn_flags & KNFL_KNOTE_DELETED)) {
        knote_release(src);
        return (1);
    }

    /*
     * Synthetic file / pipe writer: re-post for the next drain.
     * The post's ref hands off into the queued entry.
     */
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1,
                                    KQ_FILTER_KEY(src->kev.filter),
                                    (LPOVERLAPPED) src)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(src);
    }
    return (1);
}

int
evfilt_write_knote_create(struct filter *filt, struct knote *kn)
{
    HANDLE evt;
    int rv;

    if (windows_get_descriptor_type(kn) < 0)
        return (-1);

    /*
     * For regular files and named-pipe HANDLEs, writes never
     * block in any meaningful 'wait until writable' sense on
     * Win32 (files are always writable; pipes drain to the OS
     * buffer synchronously and the buffer is large enough that
     * single-byte test churn never observes back-pressure).
     * Signal writable once via a synthetic IOCP completion, then
     * level-trigger re-post via the synthetic-file path in
     * copyout's post-flag-actions until the consumer EV_CLEARs
     * or EV_DELETEs.
     */
    if (kn->kn_flags & (KNFL_FILE | KNFL_PIPE)) {
        /*
         * Stash the pipe HANDLE so the EOF probe in copyout can
         * issue a 0-byte WriteFile against it; files don't need
         * one (always writable, no peer to disappear).
         */
        if (kn->kn_flags & KNFL_PIPE)
            kn->kn_handle = (HANDLE) _get_osfhandle((int) kn->kev.ident);
        else
            kn->kn_handle = NULL;
        kn->kn_event_whandle = NULL;
        kn->kn_write.file_synthetic = 1;
        kn->kn_write.pipe_eof = 0;
        /*
         * Hold a ref for the queued completion so an EV_DELETE
         * arriving before this is drained doesn't free the knote
         * out from under us.  Released in copyout (or the
         * synthetic-post path's own re-post).
         */
        knote_retain(kn);
        if (!PostQueuedCompletionStatus(kn->kn_kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                        (LPOVERLAPPED) kn)) {
            dbg_lasterror("PostQueuedCompletionStatus()");
            knote_release(kn);
            return (-1);
        }
        return (0);
    }

    evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (evt == NULL) {
        dbg_lasterror("CreateEvent()");
        return (-1);
    }

    rv = WSAEventSelect((SOCKET) kn->kev.ident,
                        evt,
                        FD_WRITE | FD_CONNECT | FD_CLOSE);
    if (rv != 0) {
        dbg_wsalasterror("WSAEventSelect()");
        CloseHandle(evt);
        return (-1);
    }

    /*
     * Same shape as the read filter: WSAEventSelect on a freshly
     * connected socket signals the auto-reset event for the
     * initial-writability state, so RegisterWaitForSingleObject
     * would fire its callback immediately and the consumer would
     * see double deliveries (the callback's post + our synth-post
     * below).  Drain the latched network record via
     * WSAEnumNetworkEvents (this also resets the event) and
     * ResetEvent for belt-and-braces; the wait then only fires on
     * genuinely fresh FD_WRITE / FD_CLOSE transitions.
     */
    {
        WSANETWORKEVENTS evs;
        (void) WSAEnumNetworkEvents((SOCKET) kn->kev.ident, evt, &evs);
        ResetEvent(evt);
    }

    kn->kn_handle = evt;

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, evt,
        evfilt_write_callback, kn, INFINITE, 0) == 0) {
        dbg_puts("RegisterWaitForSingleObject failed");
        CloseHandle(evt);
        kn->kn_handle = NULL;
        return (-1);
    }

    /*
     * WSAEventSelect only returns edges, it doesn't fire continuously,
     * so we need to determine the current write status of the event.
     */
    {
        fd_set         wfds;
        struct timeval tv = { 0, 0 };
        FD_ZERO(&wfds);
        FD_SET((SOCKET) kn->kev.ident, &wfds);
        if (select(0, NULL, &wfds, NULL, &tv) > 0) {
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
evfilt_write_knote_delete(struct filter *filt, struct knote *kn)
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
evfilt_write_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * No native modify on Windows; tear down and re-arm.  Common
     * code does not pre-merge kev into kn->kev (filter's job).
     * EV_RECEIPT is sticky on BSD; preserve across modify.
     */
    if (evfilt_write_knote_delete(filt, kn) < 0)
        return (-1);

    kn->kev.fflags = kev->fflags;
    kn->kev.data   = kev->data;

    return evfilt_write_knote_create(filt, kn);
}

int
evfilt_write_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_write_knote_create(filt, kn);
}

int
evfilt_write_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_write_knote_delete(filt, kn);
}

const struct filter evfilt_write = {
    .kf_id      = EVFILT_WRITE,
    .kf_copyout = evfilt_write_copyout,
    .kn_create  = evfilt_write_knote_create,
    .kn_modify  = evfilt_write_knote_modify,
    .kn_delete  = evfilt_write_knote_delete,
    .kn_enable  = evfilt_write_knote_enable,
    .kn_disable = evfilt_write_knote_disable,
};
