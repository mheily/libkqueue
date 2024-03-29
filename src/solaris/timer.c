/*
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

/* Convert milliseconds into seconds+nanoseconds */
static void
convert_msec_to_itimerspec(struct itimerspec *dst, int src, int oneshot)
{
    time_t sec, nsec;

    sec = src / 1000;
    nsec = (src % 1000) * 1000000;

    /* Set the interval */
    if (oneshot) {
        dst->it_interval.tv_sec = 0;
        dst->it_interval.tv_nsec = 0;
    } else {
        dst->it_interval.tv_sec = sec;
        dst->it_interval.tv_nsec = nsec;
    }

    /* Set the initial expiration */
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
    struct knote *src, void *ptr UNUSED)
{
    /* port_event_t *pe = (port_event_t *) ptr; */

    memcpy(dst, &src->kev, sizeof(*dst));
    //TODO:
    //if (ev->events & EPOLLERR)
    //    dst->fflags = 1; /* FIXME: Return the actual timer error */

    dst->data = timer_getoverrun(src->kn_timerid) + 1;

#if FIXME
    timerid = src->kn_timerid;
    //should be done in kqops.copyout()
    if (src->kev.flags & EV_DISPATCH) {
        timer_delete(src->kn_timerid);
    } else if (src->kev.flags & EV_ONESHOT) {
        timer_delete(src->kn_timerid);
    }
#endif

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    port_notify_t pn;
    struct sigevent se;
    struct itimerspec ts;
    timer_t timerid;

    kn->kev.flags |= EV_CLEAR;

    pn.portnfy_port = filter_epoll_fd(filt);
    pn.portnfy_user = (void *) kn;

    se.sigev_notify = SIGEV_PORT;
    se.sigev_value.sival_ptr = &pn;

    if (timer_create (CLOCK_MONOTONIC, &se, &timerid) < 0) {
        dbg_perror("timer_create(2)");
        return (-1);
    }

    convert_msec_to_itimerspec(&ts, kn->kev.data, kn->kev.flags & EV_ONESHOT);
    if (timer_settime(timerid, 0, &ts, NULL) < 0) {
        dbg_perror("timer_settime(2)");
        (void) timer_delete(timerid);
        return (-1);
    }

    kn->kn_timerid = timerid;
    dbg_printf("th=%lu - created timer", (unsigned long) timerid);

    return (0);
}

int
evfilt_timer_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    (void)filt;
    (void)kn;
    (void)kev;
    return (-1); /* STUB */
}

int
evfilt_timer_knote_delete(struct filter *filt UNUSED, struct knote *kn)
{
    if (kn->kev.flags & EV_DISABLE)
        return (0);

    dbg_printf("th=%d - deleting timer", kn->kn_timerid);
    return timer_delete(kn->kn_timerid);
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
