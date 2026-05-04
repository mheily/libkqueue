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
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, (ULONG_PTR) 0,
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
     * Regular files are always considered writable.  We have no
     * direct equivalent of SIOCOUTQ on a Windows file handle, so
     * report the most useful approximation: writable with no known
     * outstanding bytes.
     */
    if (src->kn_flags & KNFL_FILE) {
        dst->data = 0;
    } else {
        /*
         * For sockets, report the available send buffer space.
         * Windows has no exact analogue of Linux SIOCOUTQ, so use
         * SO_SNDBUF as the bound.  This matches the documented
         * semantics: "amount of space remaining in the write buffer".
         */
        int sndbuf = 0;
        int slen = sizeof(sndbuf);
        if (getsockopt((SOCKET)src->kev.ident, SOL_SOCKET, SO_SNDBUF,
                       (char *)&sndbuf, &slen) == 0) {
            dst->data = sndbuf;
        } else {
            dst->data = 0;
        }
    }

    {
        int is_synthetic = src->kn_write.file_synthetic;
        int is_disabled  = (src->kev.flags & EV_DISABLE) != 0;
        struct kqueue *kq = src->kn_kq;

        if (knote_copyout_flag_actions(filt, src) < 0) {
            knote_release(src);
            return -1;
        }

        if (is_synthetic && !is_disabled) {
            int is_deleted = (src->kn_flags & KNFL_KNOTE_DELETED) != 0;
            if (!is_deleted) {
                if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, (ULONG_PTR) 0,
                                                (LPOVERLAPPED) src)) {
                    dbg_lasterror("PostQueuedCompletionStatus()");
                    knote_release(src);
                }
            } else {
                knote_release(src);
            }
        } else {
            knote_release(src);
        }
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
        kn->kn_handle = NULL;
        kn->kn_event_whandle = NULL;
        kn->kn_write.file_synthetic = 1;
        /*
         * Hold a ref for the queued completion so an EV_DELETE
         * arriving before this is drained doesn't free the knote
         * out from under us.  Released in copyout (or the
         * synthetic-post path's own re-post).
         */
        knote_retain(kn);
        if (!PostQueuedCompletionStatus(kn->kn_kq->kq_iocp, 1, (ULONG_PTR) 0,
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

    kn->kn_handle = evt;

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, evt,
        evfilt_write_callback, kn, INFINITE, 0) == 0) {
        dbg_puts("RegisterWaitForSingleObject failed");
        CloseHandle(evt);
        kn->kn_handle = NULL;
        return (-1);
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
