/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

#include <assert.h>
#include <inttypes.h>

#include "../common/private.h"


/*
 * Per-thread port event scratch buffer used to ferry data between
 * kevent_wait() and kevent_copyout() within a single kevent() call.
 *
 * Per-thread is fine because the buffer is only live for the duration
 * of one kevent() call; we never carry events across calls.  USER
 * coalescing is producer-side via the per-knote kn_user.ctr atomic,
 * so we drain exactly what the caller asked for and don't need an
 * overflow stash.
 */
static __thread port_event_t evbuf[MAX_KEVENT];

#ifndef NDEBUG

/* Dump a poll(2) events bitmask */
static char *
poll_events_dump(short events)
{
    static __thread char buf[512];

#define _PL_DUMP(attrib) \
    if (events == attrib) \
       strcat(buf, " "#attrib);

    snprintf(buf, 512, "events = %hd 0x%o (", events, events);
    _PL_DUMP(POLLIN);
    _PL_DUMP(POLLPRI);
    _PL_DUMP(POLLOUT);
    _PL_DUMP(POLLRDNORM);
    _PL_DUMP(POLLRDBAND);
    _PL_DUMP(POLLWRBAND);
    _PL_DUMP(POLLERR);
    _PL_DUMP(POLLHUP);
    _PL_DUMP(POLLNVAL);
    strcat(buf, ")");

    return (buf);

#undef _PL_DUMP
}

static char *
port_event_dump(port_event_t *evt)
{
    static __thread char buf[512];

    if (evt == NULL) {
        snprintf(buf, sizeof(buf), "NULL ?!?!\n");
        goto out;
    }

#define PE_DUMP(attrib) \
    if (evt->portev_source == attrib) \
       strcat(buf, #attrib);

    snprintf(buf, 512,
                " { object = %u, user = %p, %s, source = %d (",
                (unsigned int) evt->portev_object,
                evt->portev_user,
                poll_events_dump(evt->portev_events),
                evt->portev_source);
    PE_DUMP(PORT_SOURCE_AIO);
    PE_DUMP(PORT_SOURCE_FD);
    PE_DUMP(PORT_SOURCE_TIMER);
    PE_DUMP(PORT_SOURCE_USER);
    PE_DUMP(PORT_SOURCE_ALERT);
    strcat(buf, ") }");
#undef PE_DUMP

out:
    return (buf);
}

#endif /* !NDEBUG */

int
solaris_kqueue_init(struct kqueue *kq)
{
    if ((kq->kq_id = port_create()) < 0) {
        dbg_perror("port_create(2)");
        return (-1);
    }
    dbg_printf("created event port; fd=%d", kq->kq_id);

    kq->kq_next_epoch = 0;
    TAILQ_INIT(&kq->kq_inflight);
    TAILQ_INIT(&kq->ud_deferred_free);

    if (filter_register_all(kq) < 0) {
        close(kq->kq_id);
        return (-1);
    }

    return (0);
}

void
solaris_kqueue_free(struct kqueue *kq)
{
    struct port_udata *u;

    /*
     * Drop any udatas still pending deferred reclamation.  By the
     * time solaris_kqueue_free runs, no kevent() callers can be in
     * flight on this kqueue (kq_mtx is held by the caller), so any
     * surviving udata on ud_deferred_free is unconditionally safe
     * to reclaim.
     */
    while ((u = TAILQ_FIRST(&kq->ud_deferred_free))) {
        TAILQ_REMOVE(&kq->ud_deferred_free, u, ud_deferred_entry);
        free(u);
    }

    /*
     * Don't close kq->kq_id here.  port_create's fd is the
     * user-visible kqueue handle; the user owns close().  Closing
     * it from here is unsafe because kqueue_free_by_id may run
     * during a concurrent kqueue() that has just received the same
     * fd back from port_create after the user closed the previous
     * kqueue - close(fd) in that window would tear down the brand
     * new kqueue's port and surface as EBADF in port_getn.
     *
     * Tradeoff: a kqueue that's freed by libkqueue (e.g. atexit
     * cleanup) without the user having called close() leaves the
     * fd open until process exit.  Process teardown reaps it.
     */
}

/*
 * Allocate fresh port_udata bound to a knote.
 *
 * @param[in] kn    knote the udata is paired with.  Aliased via ud_kn.
 * @return the new udata, or NULL on allocation failure.
 */
struct port_udata *
port_udata_alloc(struct knote *kn)
{
    struct port_udata *u = calloc(1, sizeof(*u));
    if (u == NULL) return NULL;
    u->ud_kn = kn;
    return u;
}

/*
 * Mark a udata as stale and queue it for deferred reclamation.
 *
 * Called under kq_mtx by EV_DELETE (or any other path that's about
 * to free the containing knote).  After port_udata_defer_free
 * returns, the caller can safely free the knote - copyout will see
 * `ud_stale == true` and skip dispatch.
 */
void
port_udata_defer_free(struct kqueue *kq, struct port_udata *u)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    if (u == NULL) return;
    assert(!u->ud_stale);

    u->ud_stale = true;
    u->ud_boundary_epoch = kq->kq_next_epoch;
    TAILQ_INSERT_TAIL(&kq->ud_deferred_free, u, ud_deferred_entry);

    dbg_printf("kq=%p udata=%p - deferred free, boundary_epoch=%" PRIu64,
               kq, u, u->ud_boundary_epoch);
}

/*
 * Sweep the deferred-free list, reclaiming eligible udatas.
 *
 * A deferred udata is eligible for free once every still-in-flight
 * kevent() caller has an entry epoch strictly greater than the
 * udata's boundary epoch.
 */
static void
solaris_kqueue_deferred_sweep(struct kqueue *kq)
{
    struct port_udata          *u;
    struct kqueue_kevent_state *oldest;

    kqueue_mutex_assert(kq, MTX_LOCKED);

    oldest = TAILQ_FIRST(&kq->kq_inflight);
    if (!oldest) {
        while ((u = TAILQ_FIRST(&kq->ud_deferred_free))) {
            dbg_printf("kq=%p udata=%p - reclaiming, no callers in flight", kq, u);
            TAILQ_REMOVE(&kq->ud_deferred_free, u, ud_deferred_entry);
            free(u);
        }
        return;
    }

    /*
     * Safe because every filter revokes via the kernel on EV_DELETE:
     * port_dissociate for FD/FILE/TIMER, close-of-sub-port for
     * EVFILT_USER.  Once observers have exited, the ud is unreachable.
     */
    while ((u = TAILQ_FIRST(&kq->ud_deferred_free)) && (u->ud_boundary_epoch < oldest->epoch)) {
        dbg_printf("kq=%p udata=%p - reclaiming, boundary=%" PRIu64 " min_inflight=%" PRIu64,
                   kq, u, u->ud_boundary_epoch, oldest->epoch);
        TAILQ_REMOVE(&kq->ud_deferred_free, u, ud_deferred_entry);
        free(u);
    }
}

/*
 * Rebase the epoch counter to head off uint64 wrap.  See linux/platform.c
 * for the full rationale; in practice this never runs.
 */
static void
solaris_kqueue_epoch_rebase(struct kqueue *kq)
{
    struct kqueue_kevent_state *s;
    struct port_udata          *u;
    uint64_t                    base;

    kqueue_mutex_assert(kq, MTX_LOCKED);

    s = TAILQ_FIRST(&kq->kq_inflight);
    base = s ? (s->epoch - 1) : kq->kq_next_epoch;

    dbg_printf("kq=%p - epoch rebase, subtracting %" PRIu64 " from all live epochs", kq, base);

    TAILQ_FOREACH(s, &kq->kq_inflight, entry)
        s->epoch -= base;
    TAILQ_FOREACH(u, &kq->ud_deferred_free, ud_deferred_entry)
        u->ud_boundary_epoch -= base;
    kq->kq_next_epoch -= base;
}

void
solaris_kevent_enter(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    if (unlikely(kq->kq_next_epoch == UINT64_MAX)) solaris_kqueue_epoch_rebase(kq);

    state->epoch = ++kq->kq_next_epoch;
    TAILQ_INSERT_TAIL(&kq->kq_inflight, state, entry);

    dbg_printf("kq=%p - kevent_enter epoch=%" PRIu64, kq, state->epoch);
}

void
solaris_kevent_exit(struct kqueue *kq, struct kqueue_kevent_state *state)
{
    kqueue_mutex_assert(kq, MTX_LOCKED);

    TAILQ_REMOVE(&kq->kq_inflight, state, entry);
    dbg_printf("kq=%p - kevent_exit epoch=%" PRIu64, kq, state->epoch);

    solaris_kqueue_deferred_sweep(kq);
}

/*
 * Wake every thread parked in port_getn on this kqueue.
 *
 * PORT_ALERT_SET makes every port_getn return immediately with a
 * PORT_SOURCE_ALERT event.  Not cleared: by the time this runs the
 * kqueue is already unreachable (map_remove ran), so no new callers
 * can land here.  In-flight callers drain and exit; the last one
 * runs kqueue_complete_deferred_free.
 */
static void
solaris_kqueue_interrupt(struct kqueue *kq)
{
    if (port_alert(kq->kq_id, PORT_ALERT_SET, 0, NULL) < 0)
        dbg_perror("port_alert(PORT_ALERT_SET)");
    else
        dbg_printf("kq=%p - port_alert raised", kq);
}

int
solaris_kevent_wait(
        struct kqueue *kq,
        int nevents,
        const struct timespec *ts)

{
    int rv;
    uint_t nget;
    uint_t max;

    /*
     * port_getn(3C) takes two count parameters:
     *  - `max` (3rd arg): size of the events buffer, the upper bound
     *    on returned events.
     *  - `*nget` (4th arg, in/out): MINIMUM number of events to wait
     *    for before returning (it's the blocking threshold), and on
     *    return the actual number of events written.
     *
     * For BSD kevent semantics ("block until >=1 event, fill the
     * caller's buffer up to nevents") we want max=nevents and
     * *nget=1.  Passing *nget=nevents would block until nevents
     * events arrived, which deadlocks tests that expect a single
     * event from a single trigger.
     *
     * Cap at MAX_KEVENT (the static evbuf size).  Callers asking for
     * more get the rest on the next kevent() call.
     */
    if (nevents <= 0)
        max = 1;
    else if (nevents > MAX_KEVENT)
        max = MAX_KEVENT;
    else
        max = (uint_t) nevents;
    nget = 1;

    reset_errno();
    if (ts == NULL)
        dbg_printf("waiting for events (no timeout)");
    else
        dbg_printf("waiting for events (timeout %ld.%09lds)",
                   (long) ts->tv_sec, (long) ts->tv_nsec);
    rv = port_getn(kq->kq_id, evbuf, max, &nget, (struct timespec *) ts);

    dbg_printf("rv=%d errno=%d (%s) nget=%d", rv, errno, strerror(errno), nget);

    /*
     * port_getn uses *nget as both input (wait for *nget events to be
     * delivered) and output (count delivered).  On error the kernel
     * skips the output write (verified in illumos uts/common/fs/portfs/
     * port.c), leaving our input value of 1 in place - which is also a
     * valid output for "one event delivered".  Check rv to disambiguate.
     */
    if (rv != 0) {
        if (errno == ETIME) {
            dbg_puts("no events within the given timeout");
            return (0);
        }
        if (errno == EINTR) {
            dbg_puts("signal caught");
            return (-1);
        }
        dbg_perror("port_getn(2)");
        return (-1);
    }

    return (nget);
}

int
solaris_kevent_copyout(struct kqueue *kq, int nready,
        struct kevent *eventlist, int nevents)
{
    port_event_t  *evt;
    struct knote  *kn;
    struct filter *filt;
    int i, rv, skip_event, written = 0;

    for (i = 0; i < nready; i++) {
        struct port_udata *ud;
        int remaining = nevents - written;

        if (remaining <= 0)
            break;

        evt = &evbuf[i];
        skip_event = 0;
        dbg_printf("event=%s", port_event_dump(evt));

        ud = NULL;
        kn = NULL;

        /*
         * portev_user holds a port_udata*, not the knote.  A port event
         * can outlive its knote: EV_DELETE may free the knote while events
         * for it sit queued in the port.  The udata is heap-allocated and
         * defer-freed so we always have a live struct to test.  ud_stale
         * = true means the knote is gone; skip dispatch.  PORT_SOURCE_USER
         * uses a filter pointer instead, handled in the switch.
         */
        if (evt->portev_source != PORT_SOURCE_USER) {
            ud = (struct port_udata *) evt->portev_user;
            if (ud == NULL || ud->ud_stale) {
                dbg_printf("kq=%p ud=%p - stale udata, skipping", kq, ud);
                continue;
            }
            kn = ud->ud_kn;

            /*
             * BSD's EV_DISABLE drops pending events as well as future
             * ones.  An event may have been kernel-queued before the
             * disable hook ran (or before this thread observed the
             * disable through kq_mtx); suppress dispatch when the knote
             * is currently disabled.  EVFILT_USER's sub-port path
             * already drains on disable; this catches the
             * vnode/socket/timer cases where the kernel buffers the
             * event in the main port queue.
             */
            if (kn->kev.flags & EV_DISABLE) {
                dbg_printf("kq=%p kn=%p - knote disabled, skipping", kq, kn);
                continue;
            }
        }

        switch (evt->portev_source) {
            case PORT_SOURCE_ALERT:
                /*
                 * Raised by solaris_kqueue_interrupt during deferred kqueue_free.
                 * Skip so the caller exits and the last one out runs
                 * kqueue_complete_deferred_free.
                 */
                dbg_printf("kq=%p - PORT_SOURCE_ALERT, skipping", kq);
                continue;

            case PORT_SOURCE_FD:
                /*
                 * portev_user is the knote's udata (set by the
                 * filter's port_associate); consult kev.filter
                 * directly to dispatch to EVFILT_READ or EVFILT_WRITE
                 * (separate per-knote associations on the same fd
                 * fire as separate port events).
                 */
                if (filter_lookup(&filt, kq, kn->kev.filter) < 0) {
                    dbg_printf("PORT_SOURCE_FD: unknown filter %d on kn=%p",
                               kn->kev.filter, kn);
                    rv = -1;
                    break;
                }

                /*
                 * PORT_SOURCE_FD on the sub-port fired because the sub-port
                 * has a queued event.  Drain it via port_getn so the sub-port
                 * fd is no longer POLLIN-readable.  PORT_SOURCE_FD is one-shot,
                 * so we re-associate before dispatch to keep watching.  The
                 * drain buffer is sized 1 because evfilt_user_knote_modify
                 * coalesces triggers at the producer side: at most one event
                 * is ever queued.
                 */
                if (kn->kev.filter == EVFILT_USER) {
                    port_event_t    sub_buf[1];
                    uint_t          sub_max = 1;
                    uint_t          sub_nget = 0;
                    struct timespec zero = { 0, 0 };

                    if (port_getn(kn->kn_user.subport, sub_buf, sub_max, &sub_nget, &zero) < 0
                        && errno != ETIME) {
                        dbg_perror("port_getn(sub)");
                    }
                    if (port_associate(kq->kq_id, PORT_SOURCE_FD,
                                       kn->kn_user.subport, POLLIN,
                                       kn->kn_udata) < 0) {
                        dbg_perror("port_associate(re-arm sub)");
                        rv = -1;
                        break;
                    }
                    rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);
                    /*
                     * The copyout already disabled or deleted the knote per
                     * EV_DISPATCH/EV_ONESHOT.  The post-switch block runs them
                     * again unless we null out kn (it's conditional on kn != NULL).
                     */
                    kn = NULL;
                    break;
                }

                rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);

                /*
                 * PORT_SOURCE_FD is one-shot: port_associate auto-removes the
                 * watch on delivery, so we have to re-associate to keep getting
                 * events.  Skip re-arm for:
                 *   EV_ONESHOT/EV_DISPATCH - the post-switch block will delete
                 *     or disable the knote anyway.
                 *   EV_CLEAR - edge-triggered: re-arming on a still-ready fd
                 *     would fire a duplicate the consumer doesn't expect.
                 * Default (level-triggered) re-arms so subsequent kevent()s
                 * keep seeing persistent readiness (e.g. EV_EOF redelivered on
                 * every check until EV_DELETE).
                 */
                if (rv > 0 && !(kn->kev.flags &
                                 (EV_CLEAR | EV_DISPATCH | EV_ONESHOT))) {
                    if (filt->kn_create(filt, kn) < 0)
                        rv = -1;
                }

                /*
                 * EVFILT_READ: data=0 with no EOF/ERROR is treated as a
                 * spurious wake on stream sockets (drain race) and on
                 * regular files (poll-always-ready).  Datagram/raw
                 * sockets carry real zero-length payloads, so we let
                 * them through.  EOF and ERROR are terminal events where
                 * data=0 is correct (peer shutdown, socket error).
                 */
                if (kn->kev.filter == EVFILT_READ &&
                    eventlist->data == 0 &&
                    !(eventlist->flags & (EV_EOF | EV_ERROR)) &&
                    !(kn->kn_flags & (KNFL_SOCKET_DGRAM | KNFL_SOCKET_RDM |
                                      KNFL_SOCKET_SEQPACKET | KNFL_SOCKET_RAW)))
                    skip_event = 1;

                break;

            case PORT_SOURCE_TIMER:
                filter_lookup(&filt, kq, EVFILT_TIMER);
                rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);
                /*
                 * evfilt_timer_copyout calls knote_copyout_flag_actions
                 * itself; null kn so the post-switch dispatcher tail
                 * doesn't double-apply EV_ONESHOT/EV_DISPATCH and
                 * cause a use-after-free on the just-deleted knote.
                 */
                kn = NULL;
                break;

            case PORT_SOURCE_FILE:
                /*
                 * PORT_SOURCE_FILE is one-shot per association
                 * (like PORT_SOURCE_FD), and portev_user is the
                 * watching knote (set by evfilt_vnode_knote_create
                 * via vnode_arm).  Re-arm after delivery unless the
                 * caller asked for one-shot/dispatch/clear or the
                 * file is gone.
                 */
                filter_lookup(&filt, kq, EVFILT_VNODE);
                rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);
                if (rv > 0 && !(kn->kev.flags &
                                (EV_CLEAR | EV_DISPATCH | EV_ONESHOT)) &&
                    !(eventlist->fflags & NOTE_DELETE)) {
                    if (filt->kn_create(filt, kn) < 0)
                        rv = -1;
                }
                /*
                 * evfilt_vnode_copyout calls knote_copyout_flag_actions
                 * itself; null kn so the post-switch dispatcher tail
                 * doesn't double-action.
                 */
                kn = NULL;
                break;

            case PORT_SOURCE_USER:
                /*
                 * The only consumer of PORT_SOURCE_USER on the main
                 * port is solaris_eventfd_raise, which port_sends
                 * the filter pointer (lives as long as the kqueue,
                 * no per-knote ud lifetime to manage).  EVFILT_USER
                 * triggers go through their per-knote sub-port and
                 * surface as PORT_SOURCE_FD (handled above).
                 *
                 * portev_events carries the originating filter id;
                 * rv == 0 means a spurious wakeup (no enabled knote
                 * with a pending count), mark skip and let
                 * knote_copyout_flag_actions inside the copyout
                 * drive the dispatcher tail.
                 */
                if (filter_lookup(&filt, kq, evt->portev_events) < 0) {
                    dbg_printf("unsupported filter id in portev_events: %d",
                               evt->portev_events);
                    abort();
                }
                /*
                 * No per-knote ud here; both consumers
                 * (evfilt_signal_copyout, evfilt_proc_knote_copyout)
                 * walk filter-level state to find what fired and mark
                 * their src param UNUSED.  Pass NULL explicitly.
                 */
                rv = filt->kf_copyout(eventlist, remaining, filt, NULL, evt);
                if (rv == 0)
                    skip_event = 1;
                break;

            default:
                dbg_printf("unsupported source portev_source=%u", evt->portev_source);
                abort();
        }

        if (rv < 0) {
            dbg_puts("kevent_copyout failed");
            return (-1);
        }

        /*
         * Apply EV_DISPATCH/EV_ONESHOT for filters whose copyout
         * doesn't do it themselves (FD/TIMER).  EVFILT_SIGNAL
         * sets kn = NULL so this is a no-op for it; the signal
         * copyout drove knote_copyout_flag_actions per emitted
         * knote already.
         */
        if (kn && !skip_event && rv > 0) {
            if (eventlist->flags & EV_DISPATCH)
                knote_disable(filt, kn);
            if (eventlist->flags & EV_ONESHOT)
                knote_delete(filt, kn);
        }

        if (skip_event)
            continue;

        eventlist += rv;
        written += rv;
    }

    return (written);
}

const struct kqueue_vtable kqops =
{
    .kqueue_init        = solaris_kqueue_init,
    .kqueue_free        = solaris_kqueue_free,
    .kqueue_interrupt   = solaris_kqueue_interrupt,
    .kevent_wait        = solaris_kevent_wait,
    .kevent_copyout     = solaris_kevent_copyout,
    .eventfd_init       = solaris_eventfd_init,
    .eventfd_close      = solaris_eventfd_close,
    .eventfd_raise      = solaris_eventfd_raise,
    .eventfd_lower      = solaris_eventfd_lower,
    .eventfd_descriptor = solaris_eventfd_descriptor,
    .eventfd_register   = solaris_eventfd_register,
    .eventfd_unregister = solaris_eventfd_unregister,
};
