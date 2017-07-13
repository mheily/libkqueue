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
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include "api.h"

#include "private.h"

extern int lkm_call(int call_nr, void* arg);

#define NOTE_PASSINGFD 0x100000

int
evfilt_proc_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
	struct evproc_event event;

	epoll_event_dump(ev);
	kevent_int_to_64(&src->kev, dst);

	if (read(src->kdata.kn_dupfd, &event, sizeof(event)) == sizeof(event))
	{
		dst->fflags = event.event;
		dst->data = event.extra;

		if (dst->fflags & NOTE_FORK)
		{
			dst->data = 0;
			if (src->kev.fflags & NOTE_TRACK)
			{
				// The kernel module automatically creates
				// a new fd for the subprocess if NOTE_TRACK is used.
				// This is because otherwise we would miss NOTE_EXEC
				// most of the time.
				
				unsigned int pid = event.extra & 0xffff;
				unsigned int new_fd = (event.extra >> 16) & 0xffff;
				dbg_printf("Received new fd %d for pid %d\n", new_fd, pid);

				// Start following the child process
				struct kqueue* kq = src->kn_kq;
				struct kevent64_s change;

				change.ident = pid;
				change.filter = EVFILT_PROC;
				change.flags = EV_ADD | EV_ENABLE;
				change.fflags = src->kev.fflags | NOTE_PASSINGFD;
				change.data = 0;
				change.ext[0] = new_fd;
				
				if (kevent_copyin_one(kq, &change) == -1)
					dst->fflags |= NOTE_TRACKERR;
			}

			// This happens when we were added with NOTE_TRACK,
			// but not with NOTE_FORK. The kernel notifies us
			// about the fork anyway so that we can act, but we
			// don't deliver it to the program.

			if (!(src->kev.fflags & NOTE_FORK))
				dst->filter = 0; // drop event
		}
		else if (dst->fflags & NOTE_EXIT)
		{
			// Have this knote removed
			dst->flags |= EV_EOF | EV_ONESHOT;
			
			// NOTE_EXIT is always announced so that we can
			// remove the knote. Add EV_DELETE to avoid passing
			// the event to the application if it is not interested
			// in this event
			if (!(src->kev.fflags & NOTE_EXIT))
				dst->flags |= EV_DELETE;
		}
	}
	else
	{
		dbg_puts("Didn't read expected data len for evfilt_proc");
		dst->filter = 0;
	}

    return (0);
}

int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    struct evproc_create args;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLIN;
    kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	if (kn->kev.fflags & NOTE_PASSINGFD)
	{
		kn->kdata.kn_dupfd = kn->kev.ext[0];
	}
	else
	{
		args.pid = kn->kev.ident;
		args.flags = kn->kev.fflags;

		kn->kdata.kn_dupfd = lkm_call(NR_evproc_create, &args);
		if (kn->kdata.kn_dupfd == -1)
			dbg_printf("evproc_create() failed: %s\n", strerror(errno));

		fcntl(kn->kdata.kn_dupfd, F_SETFD, FD_CLOEXEC);
	}

	kn->kev.fflags &= ~NOTE_PASSINGFD;

    if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        return (-1);
    }
    return 0;
}

int
evfilt_proc_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
	unsigned int flags = kn->data.events;
	write(kn->kdata.kn_dupfd, &flags, sizeof(flags));
    return 0;
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
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
evfilt_proc_knote_enable(struct filter *filt, struct knote *kn)
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
evfilt_proc_knote_disable(struct filter *filt, struct knote *kn)
{
	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

const struct filter evfilt_proc = {
    EVFILT_PROC,
    NULL,
    NULL,
    evfilt_proc_copyout,
    evfilt_proc_knote_create,
    evfilt_proc_knote_modify,
    evfilt_proc_knote_delete,
    evfilt_proc_knote_enable,
    evfilt_proc_knote_disable,         
};

