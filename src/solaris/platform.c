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

#include <assert.h>
#include <inttypes.h>

#include "../common/private.h"


/*
 * Per-thread port event scratch buffer used to ferry data between
 * kevent_wait() and kevent_copyout() within a single kevent() call.
 *
 * Per-thread is fine because the buffer is only live for the duration
 * of one kevent() call; we never carry events across calls.  USER
 * coalescing is producer-side via the per-knote kn_user_ctr atomic,
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

/** Allocate a fresh port_udata bound to a knote.
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

/** Mark a udata as stale and queue it for deferred reclamation.
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

/** Sweep the deferred-free list, reclaiming eligible udatas.
 *
 * A deferred udata is eligible for free once every still-in-flight
 * kevent() caller has an entry epoch strictly greater than the
 * udata's boundary epoch.
 */
static void
solaris_kqueue_sweep_deferred(struct kqueue *kq)
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

    while ((u = TAILQ_FIRST(&kq->ud_deferred_free)) && (u->ud_boundary_epoch < oldest->epoch)) {
        dbg_printf("kq=%p udata=%p - reclaiming, boundary=%" PRIu64 " min_inflight=%" PRIu64,
                   kq, u, u->ud_boundary_epoch, oldest->epoch);
        TAILQ_REMOVE(&kq->ud_deferred_free, u, ud_deferred_entry);
        free(u);
    }
}

/** Rebase the epoch counter to head off uint64 wrap.  See linux/platform.c
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

    solaris_kqueue_sweep_deferred(kq);
}

/** Wake every thread parked in port_getn on this kqueue.
 *
 * port_alert(3C) with PORT_ALERT_SET puts the port into alert mode:
 * every current and subsequent port_getn returns immediately with a
 * PORT_SOURCE_ALERT event in evbuf.  We don't bother clearing the
 * alert (PORT_ALERT_UPDATE 0) because by the time this is called the
 * kqueue is already unreachable - new kevent() callers fail at
 * kqueue_lookup with ENOENT, so future port_getn calls on this fd
 * shouldn't happen.  In-flight callers exit, the last one out runs
 * kqueue_complete_deferred_free, and the port fd is closed by the
 * user (or leaked to process exit, the same as today).
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
     */
    max = (nevents > 0 && nevents <= MAX_KEVENT) ? (uint_t) nevents : 1;
    nget = 1;

    reset_errno();
    dbg_puts("waiting for events");
    rv = port_getn(kq->kq_id, evbuf, max, &nget, (struct timespec *) ts);

    dbg_printf("rv=%d errno=%d (%s) nget=%d", rv, errno, strerror(errno), nget);

    /*
     * Verified against illumos usr/src/uts/common/fs/portfs/port.c:
     * port_getn() doesn't update *nget on any error path, and the
     * portfs() syscall wrapper skips the nget copyout when returning
     * an error.  So on rv != 0 the post-call nget is just our
     * pre-call sentinel (1) lingering - it does NOT mean the kernel
     * wrote an event.  Treat any non-zero rv as "no events written".
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

        /*
         * Resolve the knote via the heap-allocated port_udata that
         * was passed to port_associate / port_send.  If the udata is
         * marked stale, the EV_DELETE that retired it ran while we
         * were blocked in port_getn; the knote is gone and any
         * dispatch would dereference freed memory.  Skip the slot.
         *
         * EVFILT_SIGNAL is the exception - it port_sends the FILTER
         * pointer (filters live as long as the kqueue), not a knote
         * udata, so the PORT_SOURCE_USER case below sets `kn` for it
         * after dispatching by portev_events filter id.
         */
        ud = NULL;
        kn = NULL;
        /*
         * PORT_SOURCE_ALERT events are raised by solaris_kqueue_interrupt
         * to wake parked waiters when a deferred kqueue_free is pending.
         * The portev_user field is whatever we passed to port_alert
         * (NULL); skip the slot silently and let the caller exit so
         * the deferred free can complete.
         */
        if (evt->portev_source == PORT_SOURCE_ALERT) {
            dbg_printf("kq=%p - PORT_SOURCE_ALERT, skipping", kq);
            continue;
        }
        if (evt->portev_source != PORT_SOURCE_USER ||
            evt->portev_events == EVFILT_USER) {
            ud = (struct port_udata *) evt->portev_user;
            if (ud == NULL || ud->ud_stale) {
                dbg_printf("kq=%p ud=%p - stale udata, skipping", kq, ud);
                continue;
            }
            kn = ud->ud_kn;
        }

        switch (evt->portev_source) {
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
                rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);

                /*
                 * Solaris event ports are one-shot per association
                 * (port_associate(3C): "When an event for a
                 * PORT_SOURCE_FD object is retrieved, the object no
                 * longer has an association with the port").  We
                 * have to re-associate after retrieval to keep
                 * watching - the kernel won't fire again on its own.
                 *
                 * Skip re-arm for:
                 *  - EV_ONESHOT, EV_DISPATCH: the dispatcher tail
                 *    will tear down or disable the knote anyway.
                 *  - EV_CLEAR: edge-triggered semantics.  BSD/Linux
                 *    EV_CLEAR fires only on transitions, not on
                 *    every check.  Re-associating immediately on a
                 *    still-readable socket would generate a second
                 *    event that EV_CLEAR consumers don't expect
                 *    (test_kevent_socket_clear's test_no_kevents
                 *    after a partial drain).  Approximation: fire
                 *    once per EV_ADD; user re-adds or EV_ENABLEs
                 *    to re-arm.
                 *
                 * For default (level-triggered) EV_ADD knotes we
                 * always re-arm so subsequent kevent()s keep seeing
                 * readiness.  This is what test_kevent_socket_eof
                 * needs: EV_EOF redelivered on every check until
                 * the knote is deleted.
                 */
                if (rv > 0 && !(kn->kev.flags &
                                 (EV_CLEAR | EV_DISPATCH | EV_ONESHOT))) {
                    if (filt->kn_create(filt, kn) < 0)
                        rv = -1;
                }

                /*
                 * EVFILT_READ only: if FIONREAD reported 0 bytes
                 * pending and the filter didn't flag EOF/ERROR, we
                 * raced a reader, skip to preserve "exactly one
                 * event per readable burst" semantics.  EV_EOF/
                 * EV_ERROR are real terminal events that legitimately
                 * have data=0 (peer shutdown, socket error).
                 *
                 * EVFILT_WRITE legitimately reports data=0 (e.g.
                 * regular files always claim infinite writability,
                 * the filter just returns the SO_SNDBUF or 0), so
                 * don't apply the FIONREAD heuristic there.
                 */
                if (kn->kev.filter == EVFILT_READ &&
                    eventlist->data == 0 &&
                    !(eventlist->flags & (EV_EOF | EV_ERROR)))
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
                 * PORT_SOURCE_USER wakeups carry the originating
                 * filter id in portev_events (set by
                 * solaris_eventfd_raise / evfilt_user's port_send).
                 * That doubles as our dispatch discriminator -
                 * filter_lookup by id and call its kf_copyout.
                 *
                 * Per-filter cookie semantics differ:
                 *  - EVFILT_SIGNAL is doorbell-only; portev_user
                 *    is the filter pointer and the copyout walks
                 *    sigtbl[] to find what fired.  rv == 0 means
                 *    spurious wakeup, mark the slot to skip and
                 *    knote_lookup the real knote by signum (the
                 *    copyout populated eventlist->ident) for the
                 *    dispatcher tail.
                 *  - EVFILT_USER carries the triggering knote in
                 *    portev_user, which we feed straight in.
                 */
                if (filter_lookup(&filt, kq, evt->portev_events) < 0) {
                    dbg_printf("unsupported filter id in portev_events: %d",
                               evt->portev_events);
                    abort();
                }
                rv = filt->kf_copyout(eventlist, remaining, filt, kn, evt);
                /*
                 * Both EVFILT_SIGNAL and EVFILT_USER call
                 * knote_copyout_flag_actions inside their own
                 * copyout, so null kn unconditionally to skip the
                 * post-switch dispatcher tail (which would
                 * double-apply EV_ONESHOT/EV_DISPATCH on the
                 * just-deleted/disabled knote).
                 *
                 * For EVFILT_SIGNAL, rv == 0 also indicates a
                 * spurious wakeup (no enabled knote with pending
                 * count) - mark the slot to skip.
                 */
                if (filt->kf_id == EVFILT_SIGNAL && rv == 0)
                    skip_event = 1;
                kn = NULL;
                break;

            default:
                dbg_puts("unsupported source");
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
