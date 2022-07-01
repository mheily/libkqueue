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
#include <darling/emulation/ext/for-libkqueue.h>
#include <darlingserver/rpc-supplement.h>

#include "private.h"

#define NOTE_PASSINGFD 0x100000

int
evfilt_proc_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
	dserver_kqchan_call_notification_t notification;
	dserver_kqchan_call_proc_read_t call;
	dserver_kqchan_reply_proc_read_t reply;
	int rv;
	int fd;
	char controlbuf[CMSG_SPACE(sizeof(fd))];
	struct iovec reply_data = {
		.iov_base = &reply,
		.iov_len = sizeof(reply),
	};
	struct msghdr reply_msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &reply_data,
		.msg_iovlen = 1,
		.msg_control = controlbuf,
		.msg_controllen = sizeof(controlbuf),
	};
	struct cmsghdr* reply_cmsg;

	epoll_event_dump(ev);
	kevent_int_to_64(&src->kev, dst);

	// first, read the notification
	rv = recv(src->kdata.kn_dupfd, &notification, sizeof(notification), 0);
	if (rv < 0) {
		dbg_printf("evfilt_proc_copyout() reading notification failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (notification.header.number != dserver_kqchan_msgnum_notification) {
		dbg_puts("evfilt_proc_copyout() read invalid notification");
		return -1;
	}

	// next, request the data
	call.header.number = dserver_kqchan_msgnum_proc_read;
	call.header.pid = getpid();
	call.header.tid = THREAD_ID;
	rv = send(src->kdata.kn_dupfd, &call, sizeof(call), 0);
	if (rv < 0) {
		dbg_printf("evfilt_proc_copyout() sending request failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	rv = recvmsg(src->kdata.kn_dupfd, &reply_msg, 0);
	if (rv < 0) {
		dbg_printf("evfilt_proc_copyout() reading reply failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (reply.header.number != dserver_kqchan_msgnum_proc_read) {
		dbg_puts("evfilt_proc_copyout() read invalid reply");
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
		dbg_printf("evfilt_proc_copyout() server indicated failure: %d (%s)", -reply.header.code, strerror(-reply.header.code));
		return -1;
	}

	dst->fflags = reply.fflags;
	dst->data = reply.data;

	if (dst->fflags & NOTE_FORK)
	{
		if ((dst->flags & NOTE_TRACKERR) == 0 && src->kev.fflags & NOTE_TRACK)
		{
			// The server automatically creates
			// a new fd for the subprocess if NOTE_TRACK is used.
			// This is because otherwise we would miss NOTE_EXEC
			// most of the time.

			reply_cmsg = CMSG_FIRSTHDR(&reply_msg);
			if (!reply_cmsg || reply_cmsg->cmsg_level != SOL_SOCKET || reply_cmsg->cmsg_type != SCM_RIGHTS || reply_cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
				dbg_puts("evfilt_proc_copyout() no new kqchan passed with reply");
				return -1;
			}

			memcpy(&fd, CMSG_DATA(reply_cmsg), sizeof(int));

			if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
				dbg_puts("evfilt_proc_copyout() failed to make new kqchan close-on-exec");
				close(fd);
				return -1;
			}

			unsigned int new_pid = reply.data & 0xffff;

			dbg_printf("Received new fd %d for pid %d\n", fd, new_pid);

			// Start following the child process
			struct kqueue* kq = src->kn_kq;
			struct kevent64_s change;

			change.ident = new_pid;
			change.filter = EVFILT_PROC;
			change.flags = EV_ADD | EV_ENABLE;
			change.fflags = src->kev.fflags | NOTE_PASSINGFD;
			change.data = 0;
			change.ext[0] = fd;
			
			if (kevent_copyin_one(kq, &change) == -1)
				dst->fflags |= NOTE_TRACKERR;
		}

		// This happens when we were added with NOTE_TRACK,
		// but not with NOTE_FORK. The kernel notifies us
		// about the fork anyway so that we can act, but we
		// don't deliver it to the program.

		if (!(src->kev.fflags & NOTE_FORK))
			dst->filter = EVFILT_DROP; // drop event
	}
	else if (dst->fflags & NOTE_EXIT)
	{
		// Have this knote removed
		dst->flags |= EV_EOF | EV_ONESHOT;
		
		// NOTE_EXIT is always announced so that we can
		// remove the knote. Avoid passing the event to
		// the application if it is not interested in
		// this event.
		// We use EVFILT_DROP_POSTPROCESS to force the EV_ONESHOT to be processed.
		if (!(src->kev.fflags & NOTE_EXIT))
			dst->filter = EVFILT_DROP_POSTPROCESS; // drop event
	}

    return (0);
}

int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

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
		int ret = _dserver_rpc_kqchan_proc_open_4libkqueue(kn->kev.ident, kn->kev.fflags, &kn->kdata.kn_dupfd);
		if (ret < 0) {
			dbg_printf("evproc_create() failed: %d (%s)\n", ret, strerror(-ret));
			return -1;
		}

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
	dserver_kqchan_call_proc_modify_t call;
	dserver_kqchan_reply_proc_modify_t reply;
	int rv;

	call.header.number = dserver_kqchan_msgnum_proc_modify;
	call.header.pid = getpid();
	call.header.tid = THREAD_ID;
	call.flags = kn->data.events;

	rv = send(kn->kdata.kn_dupfd, &call, sizeof(call), 0);
	if (rv < 0) {
		dbg_printf("evfilt_proc_knote_modify send failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	rv = recv(kn->kdata.kn_dupfd, &reply, sizeof(reply), 0);
	if (rv < 0) {
		dbg_printf("evfilt_proc_knote_modify recv failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (reply.header.number != dserver_kqchan_msgnum_proc_modify) {
		dbg_puts("evfilt_proc_knote_modify invalid reply");
		return -1;
	}

	if (reply.header.code != 0) {
		// FIXME: same as in copyout: Linux code but Darwin strerror
		dbg_printf("evfilt_proc_knote_modify call failed: %d (%s)", -reply.header.code, strerror(-reply.header.code));
		return -1;
	}

    return 0;
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
{
    if ((kn->kev.flags & EV_DISABLE) == 0) {
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
    }

	(void) __close_for_kqueue(kn->kdata.kn_dupfd);
	kn->kdata.kn_dupfd = -1;
	return 0;
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

