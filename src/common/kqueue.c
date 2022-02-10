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

#ifdef _WIN32
static LONG kq_init_begin = 0;
static int kq_init_complete = 0;
#else
pthread_mutex_t kq_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t kq_is_initialized = PTHREAD_ONCE_INIT;
#endif

static unsigned int
get_fd_limit(void)
{
#ifdef _WIN32
    /* actually windows should be able to hold
       way more, as they use HANDLEs for everything.
       Still this number should still be sufficient for
       the provided number of kqueue fds.
       */
    return 65536;
#else
    struct rlimit rlim;
    
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
        dbg_perror("getrlimit(2)");
        return (65536);
    } else {
        return (rlim.rlim_max);
    }
#endif
}

static struct map *kqmap;

void
libkqueue_init(void)
{
#ifdef NDEBUG
    DEBUG_KQUEUE = 0;
#else
    char *s = getenv("KQUEUE_DEBUG");
    if (s != NULL && strlen(s) > 0) {
        DEBUG_KQUEUE = 1;

#ifdef _WIN32
    /* Initialize the Winsock library */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) 
        abort();
#endif

# if defined(_WIN32) && !defined(__GNUC__)
	/* Enable heap surveillance */
	{
		int tmpFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );
		tmpFlag |= _CRTDBG_CHECK_ALWAYS_DF;
		_CrtSetDbgFlag(tmpFlag);
	}
# endif /* _WIN32 */
    }
#endif

   kqmap = map_new(get_fd_limit()); // INT_MAX
   if (kqmap == NULL)
       abort(); 
   if (knote_init() < 0)
       abort();
   dbg_puts("library initialization complete");
#ifdef _WIN32
   kq_init_complete = 1;
#endif
}

#if DEADWOOD
static int
kqueue_cmp(struct kqueue *a, struct kqueue *b)
{
    return memcmp(&a->kq_id, &b->kq_id, sizeof(int)); 
}
#endif

void kqueue_cleanup(struct kqueue* kq)
{
    struct knote *next, *tmp;
    for (next = kq->kq_tofree.lh_first; next != NULL; next = tmp)
    {
        tmp = next->kn_entries2free.le_next;
        free(next);
    }
    LIST_INIT(&kq->kq_tofree);
}

/* Must hold the kqtree_mtx when calling this */
void
kqueue_free(struct kqueue *kq)
{
	kqueue_cleanup(kq);
    map_delete(kqmap, kq->kq_id);
    filter_unregister_all(kq);
    kqops.kqueue_free(kq);
    free(kq);
}

struct kqueue *
kqueue_lookup(int kq)
{
    return ((struct kqueue *) map_lookup(kqmap, kq));
}

static void _kqueue_close_cb(int kqfd, void* kqptr, void* private)
{
	struct kqueue* kq = (struct kqueue*) kqptr;
	int closing_fd = (int) private;
	struct kevent64_s ev[2];

	ev[0].ident = closing_fd;
	ev[0].flags = EV_DELETE;
	ev[0].filter = EVFILT_READ;
	ev[1].ident = closing_fd;
	ev[1].flags = EV_DELETE;
	ev[1].filter = EVFILT_WRITE;

	// We don't care if it fails or not...
	kevent64(kqfd, ev, 2, NULL, 0, 0, NULL);
}

void VISIBLE
kqueue_close(int kqfd)
{
    if (kqmap == NULL)
        return;

    pthread_mutex_lock(&kq_mtx);

    struct kqueue* kq = kqueue_lookup(kqfd);
    if (kq == NULL) {
        // It is not a kqueue fd, but it could be a fd inside a kqueue
        // Since we're creating duplicates of all fd's, we now have to walk
        // through all known kqueues and remove the fd from them.
		map_foreach(kqmap, _kqueue_close_cb, (void*)(long) kqfd);
    }
	else {
		kqueue_delref(kq);
    }
    pthread_mutex_unlock(&kq_mtx);
}

void VISIBLE
kqueue_dup(int oldfd, int newfd)
{
    if (kqmap == NULL)
        return;

    pthread_mutex_lock(&kq_mtx);

    struct kqueue* kq = kqueue_lookup(oldfd);
    if (kq != NULL)
    {
        kqueue_addref(kq);
        map_insert(kqmap, newfd, kq);
    }

    pthread_mutex_unlock(&kq_mtx);
}

static void _kqueue_close_atfork_cb(int kqfd, void* kqptr, void* private)
{
	struct kqueue* kq = (struct kqueue*) kqptr;

    // Mutexes are not valid after a fork, reset it to unlocked state
    pthread_mutex_init(&kq->kq_mtx, NULL);

    kqueue_delref(kq);
    __close_for_kqueue(kqfd);
}

void VISIBLE kqueue_atfork(void)
{
    if (kqmap != NULL)
    {
        // Mutexes are not valid after a fork, reset it to unlocked state
        pthread_mutex_init(&kq_mtx, NULL);
        map_foreach(kqmap, _kqueue_close_atfork_cb, NULL);
    }
}

void kqueue_addref(struct kqueue *kq)
{
	atomic_inc(&kq->kq_ref);
}

void kqueue_delref(struct kqueue *kq)
{
	if (!atomic_dec(&kq->kq_ref))
		kqueue_free(kq);
}

int VISIBLE
kqueue_impl(void)
{
	struct kqueue *kq;
    struct kqueue *tmp;

#ifdef _WIN32
    if (InterlockedCompareExchange(&kq_init_begin, 0, 1) == 0) {
        libkqueue_init();
    } else {
        while (kq_init_complete == 0) {
            sleep(1);
        }
    }
#else
    (void) pthread_mutex_lock(&kq_mtx);
    (void) pthread_once(&kq_is_initialized, libkqueue_init);
    (void) pthread_mutex_unlock(&kq_mtx);
#endif

    kq = calloc(1, sizeof(*kq));
    if (kq == NULL)
        return (-1);

    kq->kq_ref = 1;
	tracing_mutex_init(&kq->kq_mtx, NULL);
    LIST_INIT(&kq->kq_tofree);

    if (kqops.kqueue_init(kq) < 0) {
        free(kq);
        return (-1);
    }

    dbg_printf("created kqueue, fd=%d", kq->kq_id);

    tmp = map_delete(kqmap, kq->kq_id);
    if (tmp != NULL) {
        dbg_puts("FIXME -- memory leak here");
        // TODO: kqops.kqueue_free(tmp), or (better yet) decrease it's refcount
    }
    if (map_insert(kqmap, kq->kq_id, kq) < 0) {
        pthread_mutex_unlock(&kq_mtx);

        dbg_puts("map insertion failed");
        kqops.kqueue_free(kq);
        return (-1);
    }

    return (kq->kq_id);
}

static FILE* debug_file_actual_file = NULL;
static pthread_once_t debug_file_once_token = PTHREAD_ONCE_INIT;

static void init_debug_file(void) {
	if (getenv("KQUEUE_DEBUG_STDERR")) {
		debug_file_actual_file = stderr;
	} else {
		debug_file_actual_file = fopen("/tmp/kqueue-debug.log", "w");
	}
};

FILE* debug_file()
{
	pthread_once(&debug_file_once_token, init_debug_file);
	return debug_file_actual_file;
}
