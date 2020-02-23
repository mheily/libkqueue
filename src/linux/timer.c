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
#include <sys/time.h>

#ifndef HAVE_SYS_TIMERFD_H

/* Android 4.0 does not have this header, but the kernel supports timerfds */
#ifndef SYS_timerfd_create
#ifdef __ARM_EABI__
#define __NR_timerfd_create             (__NR_SYSCALL_BASE+350)
#define __NR_timerfd_settime            (__NR_SYSCALL_BASE+353)
#define __NR_timerfd_gettime            (__NR_SYSCALL_BASE+354)
#else
#error Unsupported architecture, need to get the syscall numbers
#endif

#define SYS_timerfd_create __NR_timerfd_create
#define SYS_timerfd_settime __NR_timerfd_settime
#define SYS_timerfd_gettime __NR_timerfd_gettime
#endif /* ! SYS_timerfd_create */

/* XXX-FIXME 
   These are horrible hacks that are only known to be true on RHEL 5 x86.
 */
#ifndef SYS_timerfd_settime
#define SYS_timerfd_settime (SYS_timerfd_create + 1)
#endif 
#ifndef SYS_timerfd_gettime
#define SYS_timerfd_gettime (SYS_timerfd_create + 2)
#endif

int timerfd_create(int clockid, int flags)
{
  return syscall(SYS_timerfd_create, clockid, flags);
}

int timerfd_settime(int ufc, int flags, const struct itimerspec *utmr,
                    struct itimerspec *otmr)
{
  return syscall(SYS_timerfd_settime, ufc, flags, utmr, otmr);
}

int timerfd_gettime(int ufc, struct itimerspec *otmr)
{
  return syscall(SYS_timerfd_gettime, ufc, otmr);
}

#endif

#define LINUX_CLOCK_MONOTONIC 1

#ifndef NDEBUG
static char *
itimerspec_dump(struct itimerspec *ts)
{
    static __thread char buf[1024];

    snprintf(buf, sizeof(buf),
            "itimer: [ interval=%lu s %lu ns, next expire=%lu s %lu ns ]",
            ts->it_interval.tv_sec,
            ts->it_interval.tv_nsec,
            ts->it_value.tv_sec,
            ts->it_value.tv_nsec
           );

    return (buf);
}
#endif

static void convert_to_itimerspec(struct itimerspec *dst, uint64_t src, bool oneshot, int fflags)
{
	time_t sec, nsec;
	struct timeval now;

	if (fflags & NOTE_ABSOLUTE)
	{
		dbg_printf("%s", "...timer is absolute\n");
		oneshot = true;
		gettimeofday(&now, NULL);
	}

	if (fflags & NOTE_SECONDS)
	{
		dbg_printf("...timer is in seconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			src -= now.tv_sec;

		sec = src;
		nsec = 0;
	}
	else if (fflags & NOTE_USECONDS)
	{
		dbg_printf("...timer is in useconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			src -= (now.tv_sec * 1000000ull) + now.tv_usec;

		sec = src / 1000000;
		nsec = (src % 1000000) * 1000;
	}
	else if (fflags & NOTE_NSECONDS)
	{
		dbg_printf("...timer is in nseconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			src -= (now.tv_sec * 1000000000ull) + now.tv_usec*1000;

		sec = src / 1000000000;
		nsec = src % 1000000000;
	}
	else
	{
		dbg_printf("...timer is in mseconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			src -= (now.tv_sec * 1000) + (now.tv_usec / 1000);

		sec = src / 1000;
		nsec = (src % 1000) * 1000000;
	}

    /* Set the interval */
    if (oneshot) {
        dst->it_interval.tv_sec = 0;
        dst->it_interval.tv_nsec = 0;
    } else {
        dst->it_interval.tv_sec = sec;
        dst->it_interval.tv_nsec = nsec;
    }

    /* Set the initial expiration */
    dst->it_value.tv_sec = sec;
    dst->it_value.tv_nsec = nsec;
    dbg_printf("%s", itimerspec_dump(dst));
}

int
evfilt_timer_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
    uint64_t expired;
    ssize_t n;

    kevent_int_to_64(&src->kev, dst);
    if (ev->events & EPOLLERR)
        dst->fflags = 1; /* FIXME: Return the actual timer error */
          
    /* On return, data contains the number of times the
       timer has been trigered.
     */
    n = read(src->data.pfd, &expired, sizeof(expired));
    if (n != sizeof(expired)) {
        dbg_puts("invalid read from timerfd");
        expired = 1;  /* Fail gracefully */
    } 
    dst->data = expired;

    return (0);
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    struct itimerspec ts;
    int tfd;

    kn->kev.flags |= EV_CLEAR;

    tfd = timerfd_create(LINUX_CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        dbg_printf("timerfd_create(2): %s", strerror(errno));
        return (-1);
    }
    dbg_printf("created timerfd %d", tfd);

    convert_to_itimerspec(&ts, kn->kev.data, kn->kev.flags & EV_ONESHOT, kn->kev.fflags);
    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        dbg_printf("timerfd_settime(2): %s", strerror(errno));
        __close_for_kqueue(tfd);
        return (-1);
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = kn;
    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_ADD, tfd, &ev) < 0) {
        dbg_printf("epoll_ctl(2): %d", errno);
        __close_for_kqueue(tfd);
        return (-1);
    }

    kn->data.pfd = tfd;
    return (0);
}

int
evfilt_timer_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
    struct itimerspec ts;
    int tfd = kn->data.pfd;

    kn->kev.data = kev->data;
    kn->kev.flags = kev->flags;
    kn->kev.fflags = kev->fflags;

    if (!kn->kev.data)
        kn->kev.data = 1;

    convert_to_itimerspec(&ts, kn->kev.data, kn->kev.flags & EV_ONESHOT, kn->kev.fflags);
    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        dbg_printf("timerfd_settime(2): %s", strerror(errno));
        return (-1);
    }

    return (0);
}

int
evfilt_timer_knote_delete(struct filter *filt, struct knote *kn)
{
    int rv = 0;

    if (kn->data.pfd == -1)
        return (0);

    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, kn->data.pfd, NULL) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        rv = -1;
    }
    if (__close_for_kqueue(kn->data.pfd) < 0) {
        dbg_printf("close(2): %s", strerror(errno));
        rv = -1;
    }

    kn->data.pfd = -1;
    return (rv);
}

int
evfilt_timer_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_timer_knote_modify(filt, kn, &kn->kev);
}

int
evfilt_timer_knote_disable(struct filter *filt, struct knote *kn)
{
    struct itimerspec ts;
    int tfd = kn->data.pfd;

    memset(&ts, 0, sizeof(ts));

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        dbg_printf("timerfd_settime(2): %s", strerror(errno));
        return (-1);
    }

    return 0;
}

const struct filter evfilt_timer = {
    EVFILT_TIMER,
    NULL,
    NULL,
    evfilt_timer_copyout,
    evfilt_timer_knote_create,
    evfilt_timer_knote_modify,
    evfilt_timer_knote_delete,
    evfilt_timer_knote_enable,
    evfilt_timer_knote_disable,     
};
