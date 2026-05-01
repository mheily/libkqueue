/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
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
#include <signal.h>

#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "private.h"

#ifndef NDEBUG
static char *
itimerspec_dump(struct itimerspec *ts)
{
    static __thread char buf[1024];

    snprintf(buf, sizeof(buf),
            "itimer: [ interval=%lu s %lu ns, next expire=%lu s %lu ns ]",
            ts->it_interval.tv_sec,
            ts->it_interval.tv_nsec,
            ts->it_value.tv_sec,
            ts->it_value.tv_nsec
           );

    return (buf);
}
#endif

/*
 * Unit-selector bits in NOTE_* fflags (USECONDS/NSECONDS/SECONDS,
 * default ms).  Mutually exclusive per BSD kqueue.
 */
#define NOTE_TIMER_MASK (NOTE_ABSOLUTE-1)

static void
convert_timedata_to_itimerspec(struct itimerspec *dst, long src,
                               unsigned int flags, int oneshot)
{
    time_t sec, nsec;

    switch (flags & NOTE_TIMER_MASK) {
    case NOTE_USECONDS:
        sec = src / 1000000;
        nsec = (src % 1000000) * 1000;
        break;
    case NOTE_NSECONDS:
        sec = src / 1000000000;
        nsec = (src % 1000000000);
        break;
    case NOTE_SECONDS:
        sec = src;
        nsec = 0;
        break;
    default: /* milliseconds */
        sec = src / 1000;
        nsec = (src % 1000) * 1000000;
    }

    if (oneshot) {
        dst->it_interval.tv_sec = 0;
        dst->it_interval.tv_nsec = 0;
    } else {
        dst->it_interval.tv_sec = sec;
        dst->it_interval.tv_nsec = nsec;
    }

    dst->it_value.tv_sec = sec;
    dst->it_value.tv_nsec = nsec;
    dbg_printf("%s", itimerspec_dump(dst));
}

int
evfilt_timer_init(struct filter *filt UNUSED)
{
    return (0);
}

void
evfilt_timer_destroy(struct filter *filt UNUSED)
{
    return;
}

int
evfilt_timer_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr)
{
    port_event_t *pe = (port_event_t *) ptr;

    memcpy(dst, &src->kev, sizeof(*dst));

    /*
     * Solaris collapses repeated SIGEV_PORT timer firings into a
     * single port_event_t whose portev_events field carries the
     * total number of expirations since the last delivery (a
     * single tick reports 1, two ticks 2, etc.).  portev_events is
     * unsigned int; kev.data is intptr_t per the BSD spec, so widen
     * before assignment to avoid negative-on-overflow when overrun
     * exceeds INT_MAX.
     */
    dst->data = (intptr_t) (unsigned int) pe->portev_events;

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    port_notify_t pn = { 0 };
    struct sigevent se = { 0 };
    struct itimerspec ts;
    timer_t timerid;
    bool fresh_udata = false;

    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    kn->kev.flags |= EV_CLEAR;

    if (kn->kn_udata == NULL) {
        if (KN_UDATA_ALLOC(kn) == NULL) {
            dbg_puts("port_udata_alloc");
            return (-1);
        }
        fresh_udata = true;
    }

    pn.portnfy_port = filter_epoll_fd(filt);
    pn.portnfy_user = (void *) kn->kn_udata;

    se.sigev_notify = SIGEV_PORT;
    se.sigev_value.sival_ptr = &pn;

    if (timer_create (CLOCK_MONOTONIC, &se, &timerid) < 0) {
        dbg_perror("timer_create(2)");
        if (fresh_udata)
            KN_UDATA_FREE(kn);
        return (-1);
    }

    convert_timedata_to_itimerspec(&ts, kn->kev.data, kn->kev.fflags,
                                   kn->kev.flags & EV_ONESHOT);
    int abstime = (kn->kev.fflags & NOTE_ABSOLUTE) ? TIMER_ABSTIME : 0;
    if (timer_settime(timerid, abstime, &ts, NULL) < 0) {
        dbg_perror("timer_settime(2)");
        if (timer_delete(timerid) < 0)
            dbg_perror("timer_delete(2) on cleanup");
        if (fresh_udata)
            KN_UDATA_FREE(kn);
        return (-1);
    }

    kn->kn_timerid = timerid;
    dbg_printf("th=%lu - created timer", (unsigned long) timerid);

    return (0);
}

int
evfilt_timer_knote_modify(struct filter *filt UNUSED, struct knote *kn,
        const struct kevent *kev)
{
    struct itimerspec ts;

    /*
     * timer_settime on the existing timer keeps any pending expirations
     * (timer_getoverrun); a delete+recreate would zero them.  The new
     * period is in the incoming kev - kn->kev still holds the old values;
     * common code overwrites kn->kev after we return.
     */
    convert_timedata_to_itimerspec(&ts, kev->data, kev->fflags,
                                   kev->flags & EV_ONESHOT);
    int abstime = (kev->fflags & NOTE_ABSOLUTE) ? TIMER_ABSTIME : 0;
    if (timer_settime(kn->kn_timerid, abstime, &ts, NULL) < 0) {
        dbg_perror("timer_settime(2)");
        return (-1);
    }
    return (0);
}

int
evfilt_timer_knote_delete(struct filter *filt, struct knote *kn)
{
    int rv = 0;

    /*
     * If the knote is already disabled the kernel timer is gone (knote_disable
     * deleted it), so don't call timer_delete again.  Hand the udata to
     * cleanup regardless: a timer fire could have queued a port event whose
     * portev_user references this knote (see port_udata in platform.h).
     */
    if (!(kn->kev.flags & EV_DISABLE)) {
        dbg_printf("th=%d - deleting timer", kn->kn_timerid);
        rv = timer_delete(kn->kn_timerid);
    }

    if (kn->kn_udata != NULL)
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

    return (rv);
}

int
evfilt_timer_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_timer_knote_create(filt, kn);
}

int
evfilt_timer_knote_disable(struct filter *filt UNUSED, struct knote *kn)
{
    /*
     * Stop the kernel timer but keep the udata: a re-enable reuses it,
     * and an in-flight port event queued just before this call can still
     * resolve to a live knote.  EV_DELETE handles the udata cleanup.
     */
    dbg_printf("th=%d - deleting timer", kn->kn_timerid);
    return timer_delete(kn->kn_timerid);
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
