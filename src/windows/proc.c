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
 * Win32 has no native EVFILT_PROC.  We synthesise NOTE_EXIT by
 * opening the target process with SYNCHRONIZE rights and parking
 * a thread-pool wait on the process handle, which becomes
 * signaled when the process terminates.  Other NOTE_* fflags
 * (FORK, EXEC, TRACK, CHILD) have no cheap Win32 analogue and
 * are silently ignored, matching what the Linux pidfd backend
 * does for unsupported sub-events.
 */

static VOID CALLBACK
evfilt_proc_callback(void *param, BOOLEAN fired)
{
    struct knote *kn;
    struct kqueue *kq;

    assert(param);
    (void)fired; /* the wait is INFINITE; we only fire on signal */

    kn = (struct knote *)param;

    if (kn->kn_flags & KNFL_KNOTE_DELETED) {
        dbg_puts("knote marked for deletion, skipping event");
        return;
    }

    /*
     * Hold a ref for the queued completion so EV_DELETE arriving
     * after this point doesn't free the knote out from under
     * copyout's later dispatch.  Released in evfilt_proc_copyout.
     */
    knote_retain(kn);

    kq = kn->kn_kq;
    assert(kq);

    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) kn)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return;
    }
}

int
evfilt_proc_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, UNUSED void *ptr)
{
    DWORD exit_code = 0;
    unsigned int status = 0;

    memcpy(dst, &src->kev, sizeof(*dst));

    if (src->kn_handle != NULL &&
        GetExitCodeProcess((HANDLE)src->kn_handle, &exit_code)) {
        /*
         * Mirror waitpid()-style status encoding so the data field
         * matches what BSD/macOS kqueue users expect: high byte =
         * exit code, low byte = 0 for a clean exit.  Win32 has no
         * concept of being killed-by-signal, so we never set the
         * core/signal bits.
         */
        status = (exit_code & 0xff) << 8;
        dst->data = status;
        dst->flags |= EV_EOF;
    } else {
        dbg_lasterror("GetExitCodeProcess()");
        dst->data = 0;
    }

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return -1;
    }
    knote_release(src);                    /* balance callback retain */
    return (1);
}

int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    HANDLE proc;

    if (!(kn->kev.fflags & NOTE_EXIT)) {
        dbg_printf("not monitoring pid=%u as no NOTE_EXIT fflag set",
                   (unsigned int)kn->kev.ident);
        kn->kn_handle = NULL;
        kn->kn_event_whandle = NULL;
        return (0);
    }

    proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, (DWORD)kn->kev.ident);
    if (proc == NULL) {
        dbg_lasterror("OpenProcess()");
        return (-1);
    }

    /*
     * NOTE_EXIT is inherently edge-triggered/one-shot: a process
     * can only exit once.  Set the flags to match what macOS and
     * FreeBSD report, and so the common layer reaps the knote
     * after delivery.
     */
    kn->kev.flags |= EV_ONESHOT;
    kn->kev.flags |= EV_CLEAR;

    kn->kn_handle = proc;

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, proc,
        evfilt_proc_callback, kn, INFINITE, WT_EXECUTEONLYONCE) == 0) {
        dbg_lasterror("RegisterWaitForSingleObject()");
        CloseHandle(proc);
        kn->kn_handle = NULL;
        return (-1);
    }

    return (0);
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kn_event_whandle != NULL) {
        if (!UnregisterWaitEx(kn->kn_event_whandle, INVALID_HANDLE_VALUE)) {
            dbg_lasterror("UnregisterWaitEx()");
            /* fall through; we still want to close the process handle */
        }
        kn->kn_event_whandle = NULL;
    }
    if (kn->kn_handle != NULL) {
        CloseHandle((HANDLE)kn->kn_handle);
        kn->kn_handle = NULL;
    }
    return (0);
}

int
evfilt_proc_knote_modify(struct filter *filt, struct knote *kn,
    const struct kevent *kev)
{
    /*
     * Nothing to do at the Win32 layer; the wait is already
     * registered and the new flags have been merged into kn->kev
     * by the common layer.  If the original create skipped the
     * registration because no NOTE_EXIT was set, redo it now.
     */
    if (kn->kn_handle == NULL && (kev->fflags & NOTE_EXIT))
        return evfilt_proc_knote_create(filt, kn);

    return (0);
}

int
evfilt_proc_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_proc_knote_create(filt, kn);
}

int
evfilt_proc_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_proc_knote_delete(filt, kn);
}

const struct filter evfilt_proc = {
    .kf_id      = EVFILT_PROC,
    .kf_copyout = evfilt_proc_copyout,
    .kn_create  = evfilt_proc_knote_create,
    .kn_modify  = evfilt_proc_knote_modify,
    .kn_delete  = evfilt_proc_knote_delete,
    .kn_enable  = evfilt_proc_knote_enable,
    .kn_disable = evfilt_proc_knote_disable,
};
