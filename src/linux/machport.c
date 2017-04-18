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
#include "api.h"

#include "private.h"

extern int lkm_call(int call_nr, void* arg);

int
evfilt_machport_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;

    epoll_event_dump(ev);
    kevent_int_to_64(&src->kev, dst);
    dst->data = 1024; // TODO: dummy value (message size)

    return (0);
}

int
evfilt_machport_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    int port = kn->kev.ident;
    struct eventfd_machport_attach args;
    int rv;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLIN;
    kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

    kn->kdata.kn_dupfd = eventfd(0, EFD_CLOEXEC);

    args.port_name = port;
    args.evfd = kn->kdata.kn_dupfd;

    rv = lkm_call(NR_eventfd_machport_attach, &args);
    if (rv != 0) {
        dbg_printf("eventfd_machport_attach: %s", strerror(-rv));
        return (-1);
    }

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
    return 0;
}

int
evfilt_machport_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    else {
        int rv;
        struct eventfd_machport_detach args = {
            .port_name = kn->kev.ident,
            .evfd = kn->kdata.kn_dupfd
        };
        rv = lkm_call(NR_eventfd_machport_detach, &args);
        
        if (rv != 0) {
            dbg_printf("eventfd_machport_detach: %s", strerror(-rv));
        }
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        (void) close(kn->kdata.kn_dupfd);
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
