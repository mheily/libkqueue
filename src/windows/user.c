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

#include "../common/private.h"

int
evfilt_user_init(struct filter *filt)
{
    return (0);
}

void
evfilt_user_destroy(struct filter *filt)
{
}

int
evfilt_user_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void* ptr)
{
    /*
     * Stale completion: NOTE_TRIGGER post raced an EV_DELETE.
     * The post path retains, so by the time we get here the
     * knote is alive but flagged.  Discard, balance the ref.
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    /*
     * EV_DISABLE drops pending fires on BSD/Linux; a NOTE_TRIGGER
     * that was posted before the disable arrived is still queued
     * in the IOCP - drop it cleanly here.
     */
    if (src->kev.flags & EV_DISABLE) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    memcpy(dst, &src->kev, sizeof(struct kevent));

    dst->fflags &= ~NOTE_FFCTRLMASK;     //FIXME: Not sure if needed
    dst->fflags &= ~NOTE_TRIGGER;
    /*
     * Linux/Solaris keep EV_ADD set in the returned event; FreeBSD
     * strips it.  Match the Linux/Solaris flavour so the shared
     * test suite passes here too.
     */
    if ((src->kev.flags & EV_CLEAR) || (src->kev.flags & EV_DISPATCH))
        src->kev.fflags &= ~NOTE_TRIGGER;

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return -1;
    }

    /* Balance the post's ref. */
    knote_release(src);
    return (1);
}

int
evfilt_user_knote_create(struct filter *filt, struct knote *kn)
{
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

    /*
     * Coalesce repeated NOTE_TRIGGERs that haven't been delivered
     * yet: post a single IOCP completion per "armed" -> "fired"
     * transition.  Otherwise the test's 10x NOTE_TRIGGER ends up
     * with 9 stale completions still queued in the IOCP after the
     * first drain, breaking the test_no_kevents() postcondition.
     */
    if ((!(kn->kev.flags & EV_DISABLE)) && (kev->fflags & NOTE_TRIGGER)) {
        int was_pending = kn->kev.fflags & NOTE_TRIGGER;
        kn->kev.fflags |= NOTE_TRIGGER;
        if (!was_pending) {
            /* Retain across the IOCP queue so a concurrent
             * EV_DELETE can't free the knote out from under the
             * dispatcher.  Released in evfilt_user_copyout. */
            knote_retain(kn);
            if (!PostQueuedCompletionStatus(kn->kn_kq->kq_iocp, 1,
                    (ULONG_PTR) 0, (LPOVERLAPPED) kn)) {
                dbg_lasterror("PostQueuedCompletionStatus()");
                knote_release(kn);
                return (-1);
            }
        }
    }

    return (0);
}

int
evfilt_user_knote_delete(struct filter *filt, struct knote *kn)
{
    return (0);
}

int
evfilt_user_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_user_knote_create(filt, kn);
}

int
evfilt_user_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_user_knote_delete(filt, kn);
}

const struct filter evfilt_user = {
    .kf_id      = EVFILT_USER,
    .kf_init    = evfilt_user_init,
    .kf_destroy = evfilt_user_destroy,
    .kf_copyout = evfilt_user_copyout,
    .kn_create  = evfilt_user_knote_create,
    .kn_modify  = evfilt_user_knote_modify,
    .kn_delete  = evfilt_user_knote_delete,
    .kn_enable  = evfilt_user_knote_enable,
    .kn_disable = evfilt_user_knote_disable,
};
