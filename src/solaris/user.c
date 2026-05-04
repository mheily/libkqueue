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
 * EVFILT_USER on illumos.
 *
 * Each knote owns a private sub-port.  NOTE_TRIGGER port_sends into
 * the sub-port; the sub-port is associated with the main kqueue port
 * as PORT_SOURCE_FD so the wake reaches the kqueue's waiter.  EV_DELETE
 * port_dissociates and close()s the sub-port, which is the revocation
 * port_send itself lacks: queued events vanish with the port.
 *
 * Per-knote kn_user.ctr coalesces NOTE_TRIGGERs (0->1 transition is the
 * only one that issues a port_send), mirroring Linux's eventfd counter.
 */

#include "private.h"

int
evfilt_user_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr UNUSED)
{
    /* xchg-to-zero drains the trigger counter; 0 means spurious wake. */
    if (atomic_exchange(&src->kn_user.ctr, 0) == 0)
        return (0);

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags &= ~NOTE_FFCTRLMASK;
    dst->fflags &= ~NOTE_TRIGGER;
    if (src->kev.flags & EV_CLEAR)
        src->kev.fflags &= ~NOTE_TRIGGER;

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}


int
evfilt_user_knote_create(struct filter *filt, struct knote *kn)
{
    int sub_fd;

    /*
     * knote_new calloc'd this to 0, but 0 is stdin.  Use -1 as the
     * "no sub-port yet" sentinel for the kn_delete teardown check.
     */
    kn->kn_user.subport = -1;

    if (kn->kn_udata == NULL && KN_UDATA_ALLOC(kn) == NULL) {
        dbg_puts("port_udata_alloc");
        return (-1);
    }

    sub_fd = port_create();
    if (sub_fd < 0) {
        dbg_perror("port_create(sub)");
        KN_UDATA_FREE(kn);
        return (-1);
    }

    if (port_associate(filter_epoll_fd(filt), PORT_SOURCE_FD,
                       sub_fd, POLLIN, kn->kn_udata) < 0) {
        dbg_perror("port_associate(sub)");
        if (close(sub_fd) < 0)
            dbg_perror("close(sub_fd) on cleanup");
        KN_UDATA_FREE(kn);
        return (-1);
    }

    kn->kn_user.subport = sub_fd;
    return (0);
}

int
evfilt_user_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    unsigned int ffctrl;
    unsigned int fflags;

    /* Excerpted from sys/kern/kern_event.c in FreeBSD HEAD */
    ffctrl = kev->fflags & NOTE_FFCTRLMASK;
    fflags = kev->fflags & NOTE_FFLAGSMASK;
    switch (ffctrl) {
        case NOTE_FFNOP:
            break;

        case NOTE_FFAND:
            kn->kev.fflags &= fflags;
            break;

        case NOTE_FFOR:
            kn->kev.fflags |= fflags;
            break;

        case NOTE_FFCOPY:
            kn->kev.fflags = fflags;
            break;

        default:
            /* unreachable: NOTE_FFCTRLMASK covers all four values above. */
            break;
    }

    if ((kn->kev.flags & EV_DISABLE) || !(kev->fflags & NOTE_TRIGGER))
        return (0);

    kn->kev.fflags |= NOTE_TRIGGER;

    /*
     * fetch_add (not CAS) so a racing producer's increment is never
     * dropped by the consumer's exchange-to-zero.  Only the 0->1
     * transition issues a port_send.
     */
    if (atomic_fetch_add(&kn->kn_user.ctr, 1) == 0)
        return (port_send(kn->kn_user.subport, EVFILT_USER, NULL));
    return (0);
}

int
evfilt_user_knote_delete(struct filter *filt, struct knote *kn)
{
    /*
     * port_dissociate drains the pending FD event from the main port;
     * close drops events still queued in the sub-port.  Callers mid-
     * port_getn are covered by the deferred-free epoch fence.
     */
    if (kn->kn_user.subport >= 0) {
        if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD,
                            kn->kn_user.subport) < 0)
            dbg_perror("port_dissociate(sub)");
        if (close(kn->kn_user.subport) < 0)
            dbg_perror("close(sub_port)");
        kn->kn_user.subport = -1;
    }

    if (kn->kn_udata != NULL)
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);
    return (0);
}

int
evfilt_user_knote_enable(struct filter *filt, struct knote *kn)
{
    /*
     * Re-associate the sub-port with the main port so future
     * NOTE_TRIGGERs reach the kqueue waiter.  knote_disable
     * dissociated and drained the sub-port; counter is 0, so the
     * next NOTE_TRIGGER will cleanly 0->1 and port_send.
     *
     * EV_ENABLE | NOTE_TRIGGER atomic re-arm: kevent_copyin_one
     * calls knote_enable here and then falls through to kn_modify,
     * which fires the trigger via the now-re-associated path.
     */
    if (port_associate(filter_epoll_fd(filt), PORT_SOURCE_FD,
                       kn->kn_user.subport, POLLIN, kn->kn_udata) < 0) {
        dbg_perror("port_associate(re-enable sub)");
        return (-1);
    }
    return (0);
}

int
evfilt_user_knote_disable(struct filter *filt, struct knote *kn)
{
    /*
     * BSD's EV_DISABLE drops pending events as well as future ones.
     * The sub-port may have a queued trigger (sent by an earlier
     * NOTE_TRIGGER that hasn't been drained), and the main port may
     * have an FD-ready entry for the sub-port.
     *
     * port_dissociate cancels the in-flight main-port entry; draining
     * the sub-port via port_getn(timeout=0) clears any kernel-side
     * trigger byte; resetting kn_user.ctr makes the next 0->1
     * transition after re-enable fire cleanly.
     */
    port_event_t    drained[1];
    uint_t          ngot = 0;
    struct timespec zero = { 0, 0 };

    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FD,
                        kn->kn_user.subport) < 0)
        dbg_perror("port_dissociate(sub) on EV_DISABLE");

    if (port_getn(kn->kn_user.subport, drained, 1, &ngot, &zero) < 0
        && errno != ETIME)
        dbg_perror("port_getn(sub) drain on EV_DISABLE");

    atomic_store(&kn->kn_user.ctr, 0);
    return (0);
}

const struct filter evfilt_user = {
    .kf_id      = EVFILT_USER,
    .kf_copyout = evfilt_user_copyout,
    .kn_create  = evfilt_user_knote_create,
    .kn_modify  = evfilt_user_knote_modify,
    .kn_delete  = evfilt_user_knote_delete,
    .kn_enable  = evfilt_user_knote_enable,
    .kn_disable = evfilt_user_knote_disable,
};
