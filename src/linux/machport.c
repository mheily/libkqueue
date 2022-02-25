/*
 * Copyright (c) 2017 Lubos Dolezel
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

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <mach/message.h>
#include "api.h"
#include <darling/emulation/ext/for-libkqueue.h>
#include <darlingserver/rpc-supplement.h>

#include "private.h"

int
evfilt_machport_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
	dserver_kqchan_call_notification_t notification;
	dserver_kqchan_call_mach_port_read_t call;
	dserver_kqchan_reply_mach_port_read_t reply = {0};
	int rv;

    epoll_event_dump(ev);
    kevent_int_to_64(&src->kev, dst);

	// first, read the notification
	rv = recv(src->kdata.kn_dupfd, &notification, sizeof(notification), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_copyout() reading notification failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (notification.header.number != dserver_kqchan_msgnum_notification) {
		dbg_puts("evfilt_machport_copyout() read invalid notification");
		return -1;
	}

	// next, request the data
	call.header.number = dserver_kqchan_msgnum_mach_port_read;
	call.header.pid = getpid();
	call.header.tid = THREAD_ID;
	call.default_buffer = (uint64_t)&src->kn_extra_buffer[0];
	call.default_buffer_size = sizeof(src->kn_extra_buffer);
	rv = send(src->kdata.kn_dupfd, &call, sizeof(call), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_copyout() sending request failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	// now, read the reply
	rv = recv(src->kdata.kn_dupfd, &reply, sizeof(reply), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_copyout() reading reply failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (reply.header.number != dserver_kqchan_msgnum_mach_port_read) {
		dbg_puts("evfilt_machport_copyout() read invalid reply");
		return -1;
	}

	if (reply.header.code == 0xdead) {
		// server indicated there was actually no event available to read right now;
		// drop the event
		dst->filter = EVFILT_DROP;
		return 0;
	}

	if (reply.header.code != 0) {
		// FIXME: the returned code is actually a Linux code (but strerror is provided by Darwin libc here)
		dbg_printf("evfilt_machport_copyout() server indicated failure: %d (%s)", -reply.header.code, strerror(-reply.header.code));
		return -1;
	}

	if (reply.kev.flags != 0)
		dst->flags = reply.kev.flags;
	dst->data = reply.kev.data;
	dst->ext[0] = reply.kev.ext[0];
	dst->ext[1] = reply.kev.ext[1];
	dst->fflags = reply.kev.fflags;

	// TODO: we need to properly support kevent_qos with regards to the data_out argument;
	//       this is what's supposed to be passed in as the default buffer, not a buffer of our own.

    return (0);
}

int
evfilt_machport_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    int port = kn->kev.ident;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLIN;
    kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	int status = _dserver_rpc_kqchan_mach_port_open_4libkqueue(port, (void*)kn->kev.ext[0], kn->kev.ext[1], kn->kev.fflags, &kn->kdata.kn_dupfd);
	if (status < 0) {
		dbg_printf("evfilt_machport_open: %s", strerror(-status));
		return (-1);
	}

	dbg_printf("evfilt_machport_open: listening to FD %d for events %d", kn->kdata.kn_dupfd, ev.events);

    fcntl(kn->kdata.kn_dupfd, F_SETFD, FD_CLOEXEC);

    if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        return (-1);
    }
    return 0;
}

int
evfilt_machport_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
	dserver_kqchan_call_mach_port_modify_t call;
	dserver_kqchan_reply_mach_port_modify_t reply = {0};
	int rv;

	call.header.number = dserver_kqchan_msgnum_mach_port_modify;
	call.header.pid = getpid();
	call.header.tid = THREAD_ID;
	call.receive_buffer = kev->ext[0];
	call.receive_buffer_size = kev->ext[1];
	call.saved_filter_flags = kev->fflags;

	rv = send(kn->kdata.kn_dupfd, &call, sizeof(call), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_knote_modify send failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	rv = recv(kn->kdata.kn_dupfd, &reply, sizeof(reply), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_knote_modify recv failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (reply.header.number != dserver_kqchan_msgnum_mach_port_modify) {
		dbg_puts("evfilt_machport_knote_modify invalid reply");
		return -1;
	}

	if (reply.header.code != 0) {
		// FIXME: same as in copyout: Linux code but Darwin strerror
		dbg_printf("evfilt_machport_knote_modify call failed: %d (%s)", -reply.header.code, strerror(-reply.header.code));
		return -1;
	}

    return 0;
}

int
evfilt_machport_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    else {
        
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        (void) __close_for_kqueue(kn->kdata.kn_dupfd);
        kn->kdata.kn_dupfd = -1;
        return 0;
    }
}

int
evfilt_machport_knote_enable(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	dbg_printf("enabling machport knote with ID=%llu for events %d", kn->kev.ident, ev.events);

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

int
evfilt_machport_knote_disable(struct filter *filt, struct knote *kn)
{
	dbg_printf("disable machport knote with ID=%llu", kn->kev.ident);
	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

const struct filter evfilt_machport = {
    EVFILT_MACHPORT,
    NULL,
    NULL,
    evfilt_machport_copyout,
    evfilt_machport_knote_create,
    evfilt_machport_knote_modify,
    evfilt_machport_knote_delete,
    evfilt_machport_knote_enable,
    evfilt_machport_knote_disable,         
};
