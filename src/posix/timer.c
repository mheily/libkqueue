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

/*
 * EVFILT_TIMER on POSIX, driven entirely from the dispatcher's
 * pselect(2) timeout - no sleeper threads, no eventfds, no
 * socketpairs.  Each registered, enabled knote owns a struct
 * posix_timer linked into kq->kq_timers, an RB-tree keyed on the
 * (CLOCK_MONOTONIC) next-deadline.  Two hooks let the dispatcher
 * integrate them:
 *
 *   posix_timer_min_deadline_ns - O(log N) peek at the earliest
 *       deadline; the wait loop clamps its pselect timeout
 *       against it.
 *
 *   posix_timer_check - pop past-due timers off the front of the
 *       tree, bump fire_count, advance and re-insert (periodics)
 *       or leave detached (oneshot/absolute).  Idle case is
 *       O(log N); on K fires it's O(K log N).
 *
 * Disabled timers are unlinked from the tree on EV_DISABLE so they
 * don't contribute to the wait deadline; EV_ENABLE recomputes
 * next-deadline as now+interval and re-inserts.  Matches BSD's
 * EV_DISPATCH-on-timer behaviour where ticks accumulate from
 * re-enable forward, not across the disable window.
 *
 * copyout walks the per-filter knote tree (knote_foreach) and
 * emits one kevent per knote with a non-zero fire_count.  We
 * iterate knotes rather than the deadline tree so a copyout
 * delivers in knote order, not deadline order.
 */

#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "private.h"

struct posix_timer {
    RB_ENTRY(posix_timer) entry;
    struct knote   *kn;
    struct timespec next;       /* CLOCK_MONOTONIC */
    long            interval_ns;/* 0 if oneshot */
    bool            oneshot;
    bool            absolute;   /* NOTE_ABSOLUTE: fire once and stop */
    bool            in_tree;    /* linked in kq->kq_timers */
    unsigned int    fire_count;
};

static int
ts_lt(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_nsec < b->tv_nsec;
}

/*
 * RB ordering: earlier deadline first, tiebreak on pointer
 * identity so two timers with the same deadline can coexist
 * (RB_INSERT requires a strict weak ordering).
 */
static int
posix_timer_cmp(const struct posix_timer *a, const struct posix_timer *b)
{
    if (ts_lt(&a->next, &b->next)) return -1;
    if (ts_lt(&b->next, &a->next)) return  1;
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

/*
 * RB_GENERATE_STATIC uses BSD-ism `__unused` (not defined under
 * -std=gnu11 + _XOPEN_SOURCE on Linux glibc); spell it directly.
 */
RB_GENERATE_INTERNAL(posix_timer_tree, posix_timer, entry, posix_timer_cmp,
                     __attribute__((unused)) static)

static void
ts_now(struct timespec *out)
{
    clock_gettime(CLOCK_MONOTONIC, out);
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

    /*
     * Negative interval: FreeBSD's filt_timervalidate rejects
     * with EINVAL; OpenBSD/NetBSD silently accept and may fire
     * immediately or busy-loop.  Match FreeBSD and reject - it's
     * the strictest contract callers can rely on portably.
     */
    if (kn->kev.data < 0) {
        free(t);
        errno = EINVAL;
        return NULL;
    }
    ns = timer_data_to_ns(kn->kev.data, kn->kev.fflags);
    if (ns < 1) ns = 1;
    t->interval_ns = t->oneshot ? 0 : ns;
    ts_now(&t->next);
    ts_add_ns(&t->next, ns);
    return (t);
}

static void
timer_link(struct kqueue *kq, struct posix_timer *t)
{
    if (t->in_tree)
        return;
    RB_INSERT(posix_timer_tree, &kq->kq_timers, t);
    t->in_tree = true;
}

static void
timer_unlink(struct kqueue *kq, struct posix_timer *t)
{
    if (!t->in_tree)
        return;
    RB_REMOVE(posix_timer_tree, &kq->kq_timers, t);
    t->in_tree = false;
}

/*
 * Smallest "now -> next deadline" delta across enabled timers,
 * in nanoseconds.  Returns -1 when no enabled timer would
 * constrain the pselect timeout.  O(log N) - just RB_MIN.
 */
long
posix_timer_min_deadline_ns(struct kqueue *kq)
{
    struct posix_timer *t;
    struct timespec now;
    long delta;

    t = RB_MIN(posix_timer_tree, &kq->kq_timers);
    if (t == NULL)
        return (-1);
    ts_now(&now);
    if (ts_lt(&t->next, &now))
        return (0);                              /* already past-due */
    delta = (t->next.tv_sec - now.tv_sec) * 1000000000L
          + (t->next.tv_nsec - now.tv_nsec);
    return delta;
}

/*
 * Pop past-due timers off the front of the tree.  For each one,
 * bump fire_count, advance the deadline (periodic) or detach
 * (oneshot/absolute), and re-insert if still relevant.  Idle
 * case is O(log N) for the RB_MIN; with K fires it's O(K log N).
 * Disabled timers aren't in the tree and aren't touched.
 */
void
posix_timer_check(struct kqueue *kq)
{
    struct posix_timer *t;
    struct timespec now;

    t = RB_MIN(posix_timer_tree, &kq->kq_timers);
    if (t == NULL)
        return;
    ts_now(&now);

    while (t != NULL && !ts_lt(&now, &t->next)) {
        timer_unlink(kq, t);
        t->fire_count++;
        if (t->oneshot || t->absolute || t->interval_ns == 0) {
            /*
             * Detach from the deadline tree but leave the struct
             * alive; copyout will deliver the single fire and
             * the EV_ONESHOT path (or explicit EV_DELETE) frees.
             * Latch deadline so a stray re-link wouldn't refire.
             */
            t->next.tv_sec = now.tv_sec + 1000000000L;
            t->next.tv_nsec = 0;
        } else {
            /* Catch up missed intervals before re-linking so the
             * single re-insert lands at the post-catch-up key. */
            do {
                ts_add_ns(&t->next, t->interval_ns);
                if (ts_lt(&now, &t->next))
                    break;
                t->fire_count++;
            } while (1);
            timer_link(kq, t);
        }
        t = RB_MIN(posix_timer_tree, &kq->kq_timers);
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
 * kevent if there's anything to deliver.  Disabled knotes don't
 * accumulate fires (not in the tree) so the early-out here is
 * just defence-in-depth.
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
    timer_link(filt->kf_kqueue, t);

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
        timer_unlink(filt->kf_kqueue, kn->kn_timer);
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
        timer_unlink(filt->kf_kqueue, kn->kn_timer);
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
     * Resume from now: re-link with a fresh deadline and zero
     * the fire counter.  Matches BSD's EV_DISPATCH-on-timer
     * behaviour where ticks count from re-enable forward.
     */
    if (t != NULL) {
        long ns = t->absolute ? 0 : (t->interval_ns > 0 ? t->interval_ns : 1);
        ts_now(&t->next);
        ts_add_ns(&t->next, ns);
        t->fire_count = 0;
        timer_link(filt->kf_kqueue, t);
    }
    posix_wake_kqueue(filt->kf_kqueue);
    return (0);
}

int
evfilt_timer_knote_disable(struct filter *filt, struct knote *kn)
{
    /*
     * Pause: unlink from the deadline tree so we don't constrain
     * pselect on a knote that won't deliver.  EV_ENABLE re-links
     * with a fresh deadline.
     */
    if (kn->kn_timer != NULL)
        timer_unlink(filt->kf_kqueue, kn->kn_timer);
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
