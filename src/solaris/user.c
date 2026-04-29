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

#include "private.h"

int
evfilt_user_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr UNUSED)
{
    /*
     * Drain the per-knote trigger counter atomically.  Coalesces
     * concurrent NOTE_TRIGGERs the same way Linux's per-knote
     * eventfd does: producers fetch_add+conditionally-port_send
     * on the 0->1 transition, the consumer xchg-to-zero here
     * picks up everything accumulated since the last drain.
     *
     * Zero means a spurious wake (another consumer thread already
     * drained, or the port_event arrived after the producer's
     * accumulated triggers were folded into a previous emit).
     */
    if (atomic_exchange(&src->kn_user_ctr, 0) == 0)
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
evfilt_user_knote_create(struct filter *filt UNUSED, struct knote *kn)
{
#if TODO
    unsigned int ffctrl;

    //determine if EV_ADD + NOTE_TRIGGER in the same kevent will cause a trigger */
    if ((!(dst->kev.flags & EV_DISABLE)) && src->fflags & NOTE_TRIGGER) {
        dst->kev.fflags |= NOTE_TRIGGER;
        eventfd_raise(filt->kf_pfd);
    }

#endif
    if (kn->kn_udata == NULL && KN_UDATA_ALLOC(kn) == NULL) {
        dbg_puts("port_udata_alloc");
        return (-1);
    }
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
            /* XXX Return error? */
            break;
    }

    if ((kn->kev.flags & EV_DISABLE) || !(kev->fflags & NOTE_TRIGGER))
        return (0);

    kn->kev.fflags |= NOTE_TRIGGER;

    /*
     * Bump the per-knote counter unconditionally and only port_send
     * on the 0->1 transition.  fetch_add (vs CAS-on-flag) so a
     * producer that observes a non-zero counter still leaves its
     * increment visible to whichever consumer drains next - a CAS
     * racing the consumer's exchange-to-zero would silently drop
     * the trigger.
     *
     * portev_events carries the filter id; the platform copyout
     * dispatcher uses it to filter_lookup back to EVFILT_USER.
     * portev_user is the triggering knote.
     */
    if (atomic_fetch_add(&kn->kn_user_ctr, 1) == 0)
        return (port_send(filter_epoll_fd(filt), filt->kf_id, kn->kn_udata));
    return (0);
}

int
evfilt_user_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kn_udata != NULL)
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);
    return (0);
}

int
evfilt_user_knote_enable(struct filter *filt UNUSED, struct knote *kn UNUSED)
{
    /* FIXME: what happens if NOTE_TRIGGER is in fflags?
       should the event fire? */
    return (0);
}

int
evfilt_user_knote_disable(struct filter *filt UNUSED, struct knote *kn UNUSED)
{
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
