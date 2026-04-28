/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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
 * Port-based eventfd shim for illumos/Solaris.
 *
 * libkqueue's common code uses kqops.eventfd_raise() as a generic
 * "wake the kqueue waiter" doorbell.  On Linux that maps to writing
 * an eventfd that's registered in epoll.  illumos has no eventfd(2)
 * and the waiter blocks in port_getn(3C), so we model the doorbell
 * as port_send(3C) into the kqueue's event port.
 *
 * efd_events carries the originating filter id (set at init time
 * from filt->kf_id) and is written to portev_events on each
 * port_send.  The platform copyout dispatcher routes the wakeup
 * back to the right filter via filter_lookup(kq, evt->portev_events).
 * efd is the platform's per-filter eventfd (filt->kf_efd) so
 * signal_handler / user-filter NOTE_TRIGGER can call
 * kqops.eventfd_raise(&filt->kf_efd) without caring about the
 * port underneath.
 *
 * No file descriptor is consumed by these "eventfds"; ef_id is
 * left at -1 and eventfd_descriptor returns -1.  That distinguishes
 * them from real fd-backed eventfds for any caller that cares.
 */

#include "../common/private.h"

int
solaris_eventfd_init(struct eventfd *efd, struct filter *filt)
{
    efd->ef_id = -1;
    efd->ef_filt = filt;
    /*
     * Use the filter id as the port_send discriminator.  This
     * doubles as the routing key in solaris_kevent_copyout: a
     * single filter_lookup(kq, portev_events) recovers the
     * originating filter without parallel constants.
     */
    efd->efd_events = filt->kf_id;
    atomic_store(&efd->efd_raised, 0);
    return (0);
}

void
solaris_eventfd_close(struct eventfd *efd)
{
    /*
     * Nothing to release: the underlying transport is the kqueue's
     * event port, which is owned by the kqueue and freed when the
     * kqueue is freed.
     */
    (void) efd;
}

int
solaris_eventfd_raise(struct eventfd *efd)
{
    struct kqueue *kq = efd->ef_filt->kf_kqueue;
    unsigned int expected = 0;

    /*
     * Coalesce: only port_send when transitioning 0 -> 1.
     * Without this the doorbell isn't level-triggered like a real
     * eventfd counter and N raises produce N spurious wakes,
     * which trips assertions in consumers that drain their
     * ready-list on the first wake (e.g. evfilt_proc_knote_copyout
     * asserts kf_ready non-empty).  eventfd_lower clears the flag.
     */
    if (!atomic_compare_exchange_strong(&efd->efd_raised, &expected, 1))
        return (0);

    if (port_send(kq->kq_id, efd->efd_events, efd->ef_filt) < 0) {
        dbg_perror("port_send");
        atomic_store(&efd->efd_raised, 0);
        return (-1);
    }
    return (0);
}

int
solaris_eventfd_lower(struct eventfd *efd)
{
    atomic_store(&efd->efd_raised, 0);
    return (0);
}

int
solaris_eventfd_descriptor(struct eventfd *efd)
{
    (void) efd;
    return (-1);
}

int
solaris_eventfd_register(struct kqueue *kq, struct eventfd *efd)
{
    /*
     * Nothing to do: port_send into kq->kq_id from
     * solaris_eventfd_raise reaches the same port_getn the
     * kqueue's waiter is blocked in.  No epoll-style explicit
     * registration step required.
     */
    (void) kq;
    (void) efd;
    return (0);
}

void
solaris_eventfd_unregister(struct kqueue *kq, struct eventfd *efd)
{
    (void) kq;
    (void) efd;
}
