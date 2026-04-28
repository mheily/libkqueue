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

    if (filter_register_all(kq) < 0) {
        close(kq->kq_id);
        return (-1);
    }

    return (0);
}

void
solaris_kqueue_free(struct kqueue *kq)
{
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
    (void) kq;
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
        int remaining = nevents - written;

        if (remaining <= 0)
            break;

        evt = &evbuf[i];
        kn = evt->portev_user;
        skip_event = 0;
        dbg_printf("event=%s", port_event_dump(evt));

        switch (evt->portev_source) {
            case PORT_SOURCE_FD:
                /*
                 * portev_user is the knote that registered the
                 * port_associate; consult its kev.filter directly
                 * to dispatch to EVFILT_READ or EVFILT_WRITE
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
