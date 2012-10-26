/* ------------ EXPERIMENTAL DESIGN PROTOTYPE -- DO NOT USE ------------ */
/*-
 * Copyright (c) 2012 Mark Heily <mark@heily.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#ifndef _KQUEUE_H_
#define _KQUEUE_H_

#ifdef  __cplusplus
extern "C" {
#endif

/* Determine if the platform provides a native kqueue() subsystem */
#if defined(__Darwin__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# define _KQ_NATIVE
#endif

/* For platforms that have kqueue, use the native functions */
#ifdef _KQ_NATIVE
# include <sys/event.h>
# define kqueue_t      int
# define kevent_t      struct kevent
# define kq_new        kqueue
# define kq_update     kevent
# define kq_close(kq)  close((kq))
# define kq_socket_t   int
# define kq_file_t     int
#else

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/timerfd.h>
#endif
#include <sys/types.h>

struct timespec;

struct _kevent_compat {
	uintptr_t	ident;		/* identifier for this event */
	short		filter;		/* filter for event */
	unsigned short flags;
	unsigned int fflags;
	intptr_t	data;
	void		*udata;		/* opaque user data identifier */
};
typedef struct _kevent_compat* kevent_t;

struct _kqueue_compat {
    int sig;      /* Object signature */
#elif defined(__linux__)
    int kq_epollfd;
#endif
} kqueue_t;
typedef struct _kqueue* kqueue_t;


/* Analog to kqueue() */
static inline kqueue_t 
kqueue_new(void)
{
    kqueue_t kq;

    if ((kq = malloc(sizeof(*kq))) == NULL)
        return(NULL);

    kq->sig = 0xABCD;

#ifdef __linux__
    kq->kq_epollfd = epoll_create(1);
    if (kq->kq_epollfd < 0) {
        free(kq);
        return(NULL);
    }
#else
#error Unsupported OS
#endif

    return (kq);
}

/* Analog to closing a kqueue descriptor */
static int
kqueue_close(kqueue_t kq)
{
    int rv = 0;

    if (kq == NULL || kq->sig != 0xABCD)
        return(-1);

#ifdef __linux__
    if (close(kq->kq_epollfd) < 0) 
        rv = -1;
#else
#error Unsupported OS
#endif

    return (rv);
}



/* Similar to kevent(2) */
static int
kqueue_update(kqueue_t kq, const struct kevent *changelist, int nchanges,
	    struct kevent *eventlist, int nevents,
	    const struct timespec *timeout)
{
    int i, nret, rv = 0;
    struct kevent *kev;
#ifdef __linux__
    struct epoll_event epevt;
#endif

    // TODO: assert nevents == 1, to simplify things for now

    if (kq == NULL || kq->sig != 0xABCD)
        return(-1);

    /* Process each event on the changelist */
    for (i = 0; i < nchanges; i++) {
        kev = changelist[i];
        switch (kev.flags) {
            case EV_ADD:
#ifdef __linux__
                // TODO
#endif
                break;

            case EV_DELETE:
#ifdef __linux__
                // TODO
#endif
                break;

            default:
                //ERROR:
                return (-2);
        }
    }

    /* Wait for kernel events */
#ifdef __linux__
    nret = epoll_wait(kq->kq_epollfd, &epevt, 1, 0);
    if (nret < 0) {
        return (nret);
#endif

    return (rv);
}

#endif /* ! _KQ_NATIVE */

/*--------------------------------------------------------------*/
/*      These should be used instead of EV_SET
/*--------------------------------------------------------------*/

/* Wait for a descriptor to become readable 

   Does not support: 
      * getting the listen backlog length
      * setting low watermark
      * setting high watermark
      * returning number of bytes available to read
      * vnodes, bpf devices

   Need to think about how to support:
      * EV_EOF equivalent
 */
# define evfilt_read_socket(kev, fd, udata) \
    EV_SET((kev), fd, EVFILT_READ, 0, 0, 0, udata)

/* Wait for a descriptor to become writable 
   Does not support: 
      * getting the listen backlog length
      * setting low watermark
      * setting high watermark
      * returning number of bytes available to read
      * vnodes, bpf devices

   Need to think about how to support:
      * EV_EOF equivalent
*/
# define evfilt_write_socket(kev, fd, udata) \
    EV_SET((kev), (fd), EVFILT_WRITE, EV_ADD, 0, 0, (udata))

/* TODO:  EVFILT_VNODE  */

/* Wait for a signal to be delivered */
# define evfilt_signal(kev, signum, udata) \
    EV_SET((kev), (signum), EVFILT_SIGNAL, EV_ADD, 0, 0, (udata))

/* Wait for a periodic timer to expire */
# define evfilt_timer(kev, ident, udata) \
    EV_SET((kev), (ident), EVFILT_TIMER, EV_ADD, 0, 0, (udata))

/* Wait for a one-time timer to expire */
# define evfilt_timeout(kev, ident, udata) \
    EV_SET((kev), (ident), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 0, (udata))


#ifdef  __cplusplus
}
#endif

#endif /* !_KQUEUE_H_ */
