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

/* Convert milliseconds into negative increments of 100-nanoseconds */
static void
convert_msec_to_filetime(LARGE_INTEGER *dst, intptr_t src)
{
    dst->QuadPart = -((int64_t) src * 1000 * 10);
}

/*
 * Translate kev.data into milliseconds based on the unit flag in
 * kev.fflags.  BSD/Linux semantics: NOTE_USECONDS / NOTE_NSECONDS /
 * NOTE_SECONDS select the unit; absent any of those, data is
 * already milliseconds.  Win32 SetWaitableTimer + WaitForSingleObject
 * are millisecond-granular, so sub-millisecond requests round up to
 * 1ms.  NOTE_MSECONDS and NOTE_ABSOLUTE are handled at the call site.
 */
#define NOTE_TIMER_UNIT_MASK (NOTE_USECONDS | NOTE_NSECONDS | NOTE_SECONDS)

static int64_t
convert_timedata_to_msec(intptr_t data, unsigned int fflags)
{
    int64_t ms;

    switch (fflags & NOTE_TIMER_UNIT_MASK) {
    case NOTE_USECONDS:
        ms = ((int64_t) data + 999) / 1000;
        break;
    case NOTE_NSECONDS:
        ms = ((int64_t) data + 999999) / 1000000;
        break;
    case NOTE_SECONDS:
        ms = (int64_t) data * 1000;
        break;
    default:
        ms = (int64_t) data;
        break;
    }
    if (ms < 1) ms = 1;     /* Win32 timers are ms-granular. */
    return ms;
}

static int
ktimer_delete(struct filter *filt, struct knote *kn)
{

    if (kn->kn_handle == NULL || kn->kn_event_whandle == NULL)
        return (0);

    if(!UnregisterWaitEx(kn->kn_event_whandle, INVALID_HANDLE_VALUE)) {
        dbg_lasterror("UnregisterWait()");
        return (-1);
    }

    if (!CancelWaitableTimer(kn->kn_handle)) {
        dbg_lasterror("CancelWaitableTimer()");
        return (-1);
    }
    if (!CloseHandle(kn->kn_handle)) {
        dbg_lasterror("CloseHandle()");
        return (-1);
    }

    if( !(kn->kev.flags & EV_ONESHOT) )
        knote_release(kn);

    kn->kn_handle = NULL;
    return (0);
}

static VOID CALLBACK evfilt_timer_callback(void* param, BOOLEAN fired){
    struct knote* kn;
    struct kqueue* kq;
    int prev;

    if(fired){
        dbg_puts("called, but timer did not fire - this case should never be reached");
        return;
    }

    assert(param);
    kn = (struct knote*)param;

    if(kn->kn_flags & KNFL_KNOTE_DELETED) {
        dbg_puts("knote marked for deletion, skipping event");
        return;
    } else {
        kq = kn->kn_kq;
        assert(kq);

        /*
         * Bump the per-knote fire counter; copyout drains it into
         * kev.data so periodic timers report the accumulated number
         * of expirations between drains, matching Linux/BSD.
         *
         * Only post a completion on the 0->1 transition - subsequent
         * fires before the consumer drains coalesce into the same
         * IOCP entry, mirroring how timerfd reads return all pending
         * expirations in one go.
         */
        prev = atomic_fetch_add(&kn->kn_fire_count, 1);
        if (prev != 0)
            return;

        /*
         * Retain a ref for the queued completion so an EV_DELETE
         * arriving before the dispatcher drains doesn't free the
         * knote out from under us.  Released in copyout (or in
         * the dispatch-discard path if the knote was already
         * KNFL_KNOTE_DELETED'd by the time we drain).
         */
        knote_retain(kn);
        if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter), (LPOVERLAPPED) kn)) {
            dbg_lasterror("PostQueuedCompletionStatus()");
            knote_release(kn);
            (void) atomic_exchange(&kn->kn_fire_count, 0);
            return;
        }
    }
    if(kn->kev.flags & EV_ONESHOT) {
        struct filter* filt;
        if( filter_lookup(&filt, kq, kn->kev.filter) )
            dbg_perror("filter_lookup()");
        knote_release(kn);
    }
}

int
evfilt_timer_init(struct filter *filt)
{
    return (0);
}

void
evfilt_timer_destroy(struct filter *filt)
{
}

int
evfilt_timer_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote* src, void* ptr)
{
    int count;

    /*
     * Stale completion left from a callback that posted just
     * before EV_DELETE landed.  Discard, balance the post's ref,
     * and let the dispatcher drain the next entry.
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        dst->filter = 0;
        knote_release(src);
        return (0);
    }

    /*
     * EV_DISABLE'd timer: a fire posted before the disable arrived
     * is still queued; drop it.
     */
    if (src->kev.flags & EV_DISABLE) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    memcpy(dst, &src->kev, sizeof(struct kevent));

    /*
     * Drain the fire counter atomically: report what we observed
     * and reset to zero so any further callbacks accumulate fresh.
     * If a fire raced past our drain we'll see it on the next
     * callback's 0->1 transition and post a new completion.
     */
    count = atomic_exchange(&src->kn_fire_count, 0);
    if (count <= 0) count = 1;
    dst->data = count;

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return -1;
    }

    /* Balance the ref the callback's post took. */
    knote_release(src);
    return (1);
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    HANDLE th;
    LARGE_INTEGER liDueTime;
    int64_t msec;

    /*
     * BSD filt_timervalidate / libkqueue POSIX & Linux reject a
     * negative interval with EINVAL.  NOTE_ABSOLUTE is the only
     * case where a "past" deadline is meaningful (and we clamp
     * msec to 0 below); plain relative intervals must be >= 0.
     */
    if (kn->kev.data < 0 && !(kn->kev.fflags & NOTE_ABSOLUTE)) {
        errno = EINVAL;
        return (-1);
    }

    kn->kev.flags |= EV_CLEAR;

    th = CreateWaitableTimer(NULL, FALSE, NULL);
    if (th == NULL) {
        dbg_lasterror("CreateWaitableTimer()");
        return (-1);
    }
    dbg_printf("th=%p - created timer handle", th);

    msec = convert_timedata_to_msec(kn->kev.data, kn->kev.fflags);

    if (kn->kev.fflags & NOTE_ABSOLUTE) {
        /*
         * NOTE_ABSOLUTE: kev.data is a deadline expressed in the
         * unit selected by NOTE_USECONDS / NOTE_NSECONDS /
         * NOTE_SECONDS (default ms), as ms-since-Unix-epoch.
         * Convert to a relative duration vs current realtime.
         * NOTE_ABSOLUTE is inherently oneshot - never refire.
         */
        FILETIME       ft;
        ULARGE_INTEGER li;
        const ULONGLONG epoch_offset = 116444736000000000ULL; /* 100ns */
        int64_t now_ms;

        GetSystemTimePreciseAsFileTime(&ft);
        li.LowPart  = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        now_ms = (int64_t)((li.QuadPart - epoch_offset) / 10000ULL);
        msec = msec - now_ms;
        if (msec < 0) msec = 0;
    }
    convert_msec_to_filetime(&liDueTime, (intptr_t) msec);

    {
        LONG period = ((kn->kev.flags & EV_ONESHOT) ||
                       (kn->kev.fflags & NOTE_ABSOLUTE)) ? 0 : (LONG) msec;
        /* No completion routine: dispatch is via the
         * RegisterWaitForSingleObject below. */
        if (!SetWaitableTimer(th, &liDueTime, period, NULL, NULL, FALSE)) {
            dbg_lasterror("SetWaitableTimer()");
            CloseHandle(th);
            return (-1);
        }
    }

    kn->kn_handle = th;
    RegisterWaitForSingleObject(&kn->kn_event_whandle, th, evfilt_timer_callback, kn, INFINITE, 0);
    knote_retain(kn);

    return (0);
}

int
evfilt_timer_knote_delete(struct filter *filt, struct knote *kn)
{
    return (ktimer_delete(filt,kn));
}

int
evfilt_timer_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * No native modify path; cancel the existing waitable timer
     * and create a fresh one with the new period/unit.  Common
     * code does not merge kev into kn->kev before calling kn_modify
     * (the filter is responsible), so do it here before recreating.
     * EV_RECEIPT is sticky on BSD - preserve it across modify.
     */
    if (evfilt_timer_knote_delete(filt, kn) < 0)
        return (-1);

    /*
     * BSD-sticky flags from initial EV_ADD (project_bsd_kev_flags
     * memory): leave kn->kev.flags alone, only refresh fflags/data
     * for the new period/unit.
     */
    kn->kev.fflags = kev->fflags;
    kn->kev.data   = kev->data;

    return evfilt_timer_knote_create(filt, kn);
}

int
evfilt_timer_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_timer_knote_create(filt, kn);
}

int
evfilt_timer_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_timer_knote_delete(filt, kn);
}

const struct filter evfilt_timer = {
    .kf_id      = EVFILT_TIMER,
    .kf_init    = evfilt_timer_init,
    .kf_destroy = evfilt_timer_destroy,
    .kf_copyout = evfilt_timer_copyout,
    .kn_create  = evfilt_timer_knote_create,
    .kn_modify  = evfilt_timer_knote_modify,
    .kn_delete  = evfilt_timer_knote_delete,
    .kn_enable  = evfilt_timer_knote_enable,
    .kn_disable = evfilt_timer_knote_disable,
};
