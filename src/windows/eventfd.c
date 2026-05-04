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
 * IOCP-based eventfd shim for Windows.
 *
 * libkqueue's common code uses kqops.eventfd_raise() as a generic
 * "wake the kqueue waiter" doorbell.  On Linux that maps to writing
 * an eventfd that's registered in epoll; on Solaris to port_send
 * into the kqueue's event port; on Windows the waiter blocks in
 * GetQueuedCompletionStatus, so we model the doorbell as
 * PostQueuedCompletionStatus into kq->kq_iocp.
 *
 * efd_filter_id is set at init from filt->kf_id and stuffed into
 * the IOCP key on each post.  The platform copyout routes the
 * wakeup back to the right filter via filter_lookup(kq, key).
 *
 * No file descriptor is consumed; ef_id stays at -1 and
 * eventfd_descriptor returns -1.  That distinguishes these shims
 * from real fd-backed eventfds for any caller that cares.
 */

#include "../common/private.h"

int
windows_eventfd_init(struct eventfd *efd, struct filter *filt)
{
    efd->ef_id = -1;
    efd->ef_filt = filt;
    efd->efd_filter_id = filt->kf_id;
    atomic_store(&efd->efd_raised, 0);
    return (0);
}

void
windows_eventfd_close(struct eventfd *efd)
{
    /*
     * Nothing to release: the underlying transport is the kqueue's
     * IOCP, which is owned by the kqueue and freed when the kqueue
     * is freed.
     */
    (void) efd;
}

int
windows_eventfd_raise(struct eventfd *efd)
{
    struct kqueue *kq = efd->ef_filt->kf_kqueue;
    int expected = 0;

    /*
     * Coalesce: only post when transitioning 0 -> 1.  Without this
     * the doorbell isn't level-triggered like a real eventfd
     * counter and N raises produce N spurious wakes.
     * eventfd_lower clears the flag.
     */
    if (!atomic_compare_exchange_strong(&efd->efd_raised, &expected, 1))
        return (0);

    /*
     * overlap=NULL marks this completion as a doorbell rather
     * than a per-knote IOCP entry; windows_kevent_copyout uses
     * that to discriminate and routes via filter id in the key.
     */
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 0,
                                    (ULONG_PTR)(LONG_PTR) efd->efd_filter_id,
                                    NULL)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        atomic_store(&efd->efd_raised, 0);
        return (-1);
    }
    return (0);
}

int
windows_eventfd_lower(struct eventfd *efd)
{
    atomic_store(&efd->efd_raised, 0);
    return (0);
}

int
windows_eventfd_descriptor(struct eventfd *efd)
{
    (void) efd;
    return (-1);
}

int
windows_eventfd_register(struct kqueue *kq, struct eventfd *efd)
{
    /*
     * Nothing to do: PostQueuedCompletionStatus into kq->kq_iocp
     * from windows_eventfd_raise reaches the same IOCP the
     * kqueue's waiter is blocked in.  No epoll-style explicit
     * registration step required.
     */
    (void) kq;
    (void) efd;
    return (0);
}

void
windows_eventfd_unregister(struct kqueue *kq, struct eventfd *efd)
{
    (void) kq;
    (void) efd;
}
