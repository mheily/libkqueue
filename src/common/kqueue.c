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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "private.h"

int DEBUG_KQUEUE = 0;
char *KQUEUE_DEBUG_IDENT = "KQ";

int
kqueue_close(kqueue_t kq)
{
    filter_unregister_all(kq);
#if defined(__sun__)
    port_event_t *pe = (port_event_t *) pthread_getspecific(kq->kq_port_event);

    if (kq->kq_port > 0) 
        close(kq->kq_port);
    free(pe);
#endif
    free(kq);

    return (0);
}

/* Non-portable kqueue initalization code. */
static int
kqueue_sys_init(struct kqueue *kq)
{
#if defined(__sun__)
    port_event_t *pe;

    if ((kq->kq_port = port_create()) < 0) {
        dbg_perror("port_create(2)");
        return (-1);
    }
    if (pthread_key_create(&kq->kq_port_event, NULL) != 0)
       abort();
    if ((pe = calloc(1, sizeof(*pe))) == NULL) 
       abort();
    if (pthread_setspecific(kq->kq_port_event, pe) != 0)
       abort();
#endif
    return (0);
}

kqueue_t
kqueue_open(void)
{
    struct kqueue *kq;

#ifndef __ANDROID__
    int cancelstate;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate) != 0)
        return (NULL);
#endif

    kq = calloc(1, sizeof(*kq));
    if (kq == NULL)
        return (NULL);
    kq->kq_ref = 1;
    pthread_mutex_init(&kq->kq_mtx, NULL);

#ifdef NDEBUG
    DEBUG_KQUEUE = 0;
#else
    DEBUG_KQUEUE = (getenv("KQUEUE_DEBUG") == NULL) ? 0 : 1;
#endif

    if (kqueue_sys_init(kq) < 0)
        goto errout;

    if (filter_register_all(kq) < 0)
        goto errout;

    dbg_printf("created kqueue, id=%d", kq->kq_id);
#ifndef __ANDROID__
    (void) pthread_setcancelstate(cancelstate, NULL);
#endif

    return (kq);

errout:

#if defined(__sun__)
    if (kq->kq_port > 0) 
	close(kq->kq_port);
#endif
    free(kq);
#ifndef __ANDROID__
    (void) pthread_setcancelstate(cancelstate, NULL);
#endif
    return (NULL);
}
