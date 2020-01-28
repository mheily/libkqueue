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

#include "private.h"

extern int lkm_call(int call_nr, void* arg);

int
evfilt_machport_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
	struct evpset_event kernel_event;
	int rv;

    epoll_event_dump(ev);
    kevent_int_to_64(&src->kev, dst);

	rv = read(src->kdata.kn_dupfd, &kernel_event, sizeof(kernel_event));
	if (rv < 0) {
		dbg_printf("evfilt_machport_copyout() failed: %s", strerror(-rv));
		return -1;	
	}

	if (kernel_event.flags != 0)
		dst->flags = kernel_event.flags;
	dst->data = kernel_event.port;
	dst->ext[1] = kernel_event.msg_size;
    
#if DARLING_MACH_API_VERSION > 15
    if (kernel_event.msg_size <= sizeof(kernel_event.process_data) && kernel_event.receive_status == MACH_MSG_SUCCESS)
        dst->ext[0] = kernel_event.process_data;
#endif
	dst->fflags = kernel_event.receive_status;

    return (0);
}

int
evfilt_machport_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    int port = kn->kev.ident;
	struct evfilt_machport_open_args args;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLIN;
    kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

    kn->kdata.kn_dupfd = eventfd(0, EFD_CLOEXEC);

    args.port_name = port;
	args.opts.rcvbuf = (void*) kn->kev.ext[0];
	args.opts.rcvbuf_size = kn->kev.ext[1];
	args.opts.sfflags = kn->kev.fflags;

    kn->kdata.kn_dupfd = lkm_call(NR_evfilt_machport_open, &args);
    if (kn->kdata.kn_dupfd < 0) {
        dbg_printf("evfilt_machport_open: %s", strerror(-kn->kdata.kn_dupfd));
        return (-1);
    }

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
	struct evpset_options opts;
	int rv;

	opts.rcvbuf = (void*) kev->ext[0];
	opts.rcvbuf_size = kev->ext[1];
	opts.sfflags = kev->fflags;

	rv = write(kn->kdata.kn_dupfd, &opts, sizeof(opts));
	if (rv < 0) {
		dbg_printf("evfilt_machport_knote_modify failed: %s", strerror(-rv));
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

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

int
evfilt_machport_knote_disable(struct filter *filt, struct knote *kn)
{
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
