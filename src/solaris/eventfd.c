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
 * Implements kqops.eventfd_raise for illumos.
 *
 * The cross-platform code calls eventfd_raise to wake a kqueue waiter.
 * illumos has no eventfd(2), so we wake by port_send into the kqueue's
 * event port.  efd_events carries the filter id; solaris_kevent_copyout
 * dispatches via filter_lookup.
 *
 * No fd is allocated: ef_id stays -1, eventfd_descriptor returns -1.
 */

#include "../common/private.h"

int
solaris_eventfd_init(struct eventfd *efd, struct filter *filt)
{
    efd->ef_id = -1;
    efd->ef_filt = filt;
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
     * Only port_send on the 0->1 transition.  Without coalescing, N raises
     * fire N port events; consumers like evfilt_proc_knote_copyout drain
     * their ready-list on the first wake and assert kf_ready non-empty
     * on subsequent ones.
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
