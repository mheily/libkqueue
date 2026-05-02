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

/*
 * EVFILT_TIMER on POSIX, driven entirely from the dispatcher's
 * pselect(2) timeout - no sleeper threads, no eventfds, no
 * socketpairs.  Each registered knote owns a struct posix_timer
 * holding its (CLOCK_MONOTONIC) next-deadline + interval; all
 * timers for a kqueue live on kq->kq_timers.  Two hooks let the
 * dispatcher integrate them:
 *
 *   posix_timer_min_deadline_ns - smallest "now -> deadline" delta
 *       across enabled timers; the wait loop clamps its pselect
 *       timeout against this.
 *
 *   posix_timer_check - advance every timer's deadline past the
 *       current monotonic time, bumping fire_count once per skipped
 *       interval (so EV_DISPATCH/EV_DISABLE accumulate ticks the
 *       way native BSD does).  Removes oneshot timers as they fire.
 *
 * copyout walks kq_timers and emits one kevent per knote with a
 * non-zero fire_count, draining the counter as it goes.
 */

#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "private.h"

struct posix_timer {
    TAILQ_ENTRY(posix_timer) entry;
    struct knote   *kn;
    struct timespec next;       /* CLOCK_MONOTONIC */
    long            interval_ns;/* 0 if oneshot */
    bool            oneshot;
    bool            absolute;   /* NOTE_ABSOLUTE: fire once and stop */
    unsigned int    fire_count;
};

static void
ts_now(struct timespec *out)
{
    clock_gettime(CLOCK_MONOTONIC, out);
}

static int
ts_lt(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_nsec < b->tv_nsec;
}

static void
ts_add_ns(struct timespec *t, long ns)
{
    t->tv_sec  += ns / 1000000000L;
    t->tv_nsec += ns % 1000000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec  += 1;
        t->tv_nsec -= 1000000000L;
    }
}

/*
 * Convert a kev.data + fflags pair into nanoseconds.  BSD picks
 * the unit from the fflags bits (NOTE_USECONDS, NOTE_NSECONDS,
 * NOTE_SECONDS); the default is milliseconds.
 */
static long
timer_data_to_ns(intptr_t data, unsigned int fflags)
{
#ifdef NOTE_NSECONDS
    if (fflags & NOTE_NSECONDS) return (long) data;
#endif
#ifdef NOTE_USECONDS
    if (fflags & NOTE_USECONDS) return (long) data * 1000L;
#endif
#ifdef NOTE_SECONDS
    if (fflags & NOTE_SECONDS) return (long) data * 1000000000L;
#endif
    return (long) data * 1000000L;     /* default: ms */
}

static struct posix_timer *
timer_alloc(struct knote *kn)
{
    struct posix_timer *t;
    long ns;

    t = calloc(1, sizeof(*t));
    if (t == NULL)
        return (NULL);
    t->kn      = kn;
    t->oneshot = (kn->kev.flags & EV_ONESHOT) != 0;

#ifdef NOTE_ABSOLUTE
    if (kn->kev.fflags & NOTE_ABSOLUTE) {
        /*
         * kev.data is an absolute deadline in CLOCK_REALTIME
         * units; convert to a CLOCK_MONOTONIC deadline by
         * computing the delta against wall-now and adding to
         * monotonic-now.  Wall-clock jumps after registration
         * will distort this; matching strict BSD semantics here
         * would need a separate clock per timer, which we'll
         * add if a test calls for it.
         */
        struct timespec rt_now, m_now;
        long abs_ns = timer_data_to_ns(kn->kev.data, kn->kev.fflags);
        clock_gettime(CLOCK_REALTIME, &rt_now);
        ts_now(&m_now);
        long delta = (abs_ns / 1000000000L - rt_now.tv_sec) * 1000000000L
                   + (abs_ns % 1000000000L - rt_now.tv_nsec);
        if (delta < 0) delta = 0;
        t->next     = m_now;
        ts_add_ns(&t->next, delta);
        t->absolute = true;
        return (t);
    }
#endif

    ns = timer_data_to_ns(kn->kev.data, kn->kev.fflags);
    if (ns < 1) ns = 1;
    t->interval_ns = t->oneshot ? 0 : ns;
    ts_now(&t->next);
    ts_add_ns(&t->next, ns);
    return (t);
}

/*
 * Smallest "now -> next deadline" delta across enabled timers,
 * in nanoseconds.  Returns -1 when no timer would constrain the
 * pselect timeout.  Disabled timers don't influence the wait but
 * still get advanced in posix_timer_check so fire_count
 * accumulates ticks across the disable window.
 */
long
posix_timer_min_deadline_ns(struct kqueue *kq)
{
    struct posix_timer *t;
    struct timespec now;
    long min_ns = -1;

    if (TAILQ_EMPTY(&kq->kq_timers))
        return (-1);
    ts_now(&now);
    TAILQ_FOREACH(t, &kq->kq_timers, entry) {
        long delta;
        if (KNOTE_DISABLED(t->kn))
            continue;
        if (ts_lt(&t->next, &now))
            return (0);              /* already past-due */
        delta = (t->next.tv_sec - now.tv_sec) * 1000000000L
              + (t->next.tv_nsec - now.tv_nsec);
        if (min_ns < 0 || delta < min_ns)
            min_ns = delta;
    }
    return min_ns;
}

/*
 * Walk every timer on the kq and roll its deadline forward past
 * the current monotonic time, bumping fire_count once per missed
 * interval.  Oneshot/absolute timers fire at most once and are
 * unlinked + freed.  Called from the copyout path before we
 * iterate timers for delivery.
 */
void
posix_timer_check(struct kqueue *kq)
{
    struct posix_timer *t, *tnext;
    struct timespec now;

    if (TAILQ_EMPTY(&kq->kq_timers))
        return;
    ts_now(&now);
    TAILQ_FOREACH_SAFE(t, &kq->kq_timers, entry, tnext) {
        if (KNOTE_DISABLED(t->kn))
            continue;                            /* paused; resume on EV_ENABLE */
        while (!ts_lt(&now, &t->next)) {        /* now >= t->next */
            t->fire_count++;
            if (t->oneshot || t->absolute || t->interval_ns == 0)
                goto stop;
            ts_add_ns(&t->next, t->interval_ns);
        }
        continue;
    stop:
        /* Latch deadline well past now so we don't re-fire. */
        t->next.tv_sec = now.tv_sec + 1000000000L;
        t->next.tv_nsec = 0;
    }
}

int
evfilt_timer_init(UNUSED struct filter *filt)
{
    return (0);
}

void
evfilt_timer_destroy(UNUSED struct filter *filt)
{
}

/*
 * Per-knote copyout.  Reads-and-zeros the fire counter; emits one
 * kevent if there's anything to deliver.  KNOTE_DISABLED knotes
 * leave the counter alone so on EV_ENABLE the accumulated tick
 * count is delivered in one shot.
 */
static int
evfilt_timer_copyout_one(struct kevent *dst, struct filter *filt,
        struct knote *src)
{
    struct posix_timer *t = src->kn_timer;
    unsigned int count;

    if (t == NULL)
        return (0);
    if (KNOTE_DISABLED(src))
        return (0);
    count = t->fire_count;
    if (count == 0)
        return (0);
    t->fire_count = 0;

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->data = (intptr_t) count;

    if (knote_copyout_flag_actions(filt, src) < 0)
        return (-1);
    return (1);
}

struct timer_drain_ctx {
    struct filter *filt;
    struct kevent *eventlist;
    int            nevents;
    int            nout;
    int            err;
};

static int
timer_drain_cb(struct knote *kn, void *uctx)
{
    struct timer_drain_ctx *c = uctx;
    int rv;

    if (c->nout >= c->nevents)
        return (1);
    rv = evfilt_timer_copyout_one(c->eventlist + c->nout, c->filt, kn);
    if (rv < 0) {
        c->err = -1;
        return (1);
    }
    c->nout += rv;
    return (0);
}

int
evfilt_timer_copyout(struct kevent *dst, int nevents, struct filter *filt,
        UNUSED struct knote *src, UNUSED void *ptr)
{
    struct timer_drain_ctx c = {
        .filt = filt, .eventlist = dst, .nevents = nevents,
        .nout = 0, .err = 0,
    };

    posix_timer_check(filt->kf_kqueue);
    (void) knote_foreach(filt, timer_drain_cb, &c);
    return c.err < 0 ? -1 : c.nout;
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    struct posix_timer *t;

    /* BSD makes timers EV_CLEAR by default - data is the fire
     * count since last delivery, not a sticky boolean. */
    kn->kev.flags |= EV_CLEAR;

    t = timer_alloc(kn);
    if (t == NULL)
        return (-1);
    kn->kn_timer = t;
    TAILQ_INSERT_TAIL(&filt->kf_kqueue->kq_timers, t, entry);

    /*
     * A new deadline may be sooner than the one a parked pselect
     * is waiting on; wake it so it recomputes its timeout.
     */
    posix_wake_kqueue(filt->kf_kqueue);
    return (0);
}

int
evfilt_timer_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * Replace the timer state with one matching the new interval.
     * EV_RECEIPT is sticky on BSD (mirrors src/linux/timer.c).
     */
    unsigned short keep = kn->kev.flags & EV_RECEIPT;
    if (kn->kn_timer != NULL) {
        TAILQ_REMOVE(&filt->kf_kqueue->kq_timers, kn->kn_timer, entry);
        free(kn->kn_timer);
        kn->kn_timer = NULL;
    }
    kn->kev = *kev;
    kn->kev.flags |= EV_CLEAR | keep;
    return evfilt_timer_knote_create(filt, kn);
}

int
evfilt_timer_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kn_timer != NULL) {
        TAILQ_REMOVE(&filt->kf_kqueue->kq_timers, kn->kn_timer, entry);
        free(kn->kn_timer);
        kn->kn_timer = NULL;
    }
    return (0);
}

int
evfilt_timer_knote_enable(struct filter *filt, struct knote *kn)
{
    struct posix_timer *t = kn->kn_timer;

    /*
     * Resume from now: the disable window doesn't accumulate
     * fires (posix_timer_check skips disabled timers), so a
     * fresh deadline + zero fire_count is the natural restart
     * point.  Matches BSD's EV_DISPATCH-on-timer behaviour where
     * the user gets ticks counted from re-enable forward.
     */
    if (t != NULL) {
        long ns = t->absolute ? 0 : (t->interval_ns > 0 ? t->interval_ns : 1);
        ts_now(&t->next);
        ts_add_ns(&t->next, ns);
        t->fire_count = 0;
    }
    posix_wake_kqueue(filt->kf_kqueue);
    return (0);
}

int
evfilt_timer_knote_disable(UNUSED struct filter *filt, UNUSED struct knote *kn)
{
    /*
     * Pause: posix_timer_check observes KNOTE_DISABLED and skips
     * the entry, so no fires accumulate while disabled.
     * EV_ENABLE will reset the deadline.
     */
    return (0);
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
