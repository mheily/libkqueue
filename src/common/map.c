/*
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
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

struct map {
    size_t len;
    void **data;
    pthread_rwlock_t mtx;
};

struct map *
map_new(size_t len)
{
    struct map *dst;

    dst = calloc(1, sizeof(struct map));
    if (dst == NULL)
        return (NULL);

    pthread_rwlock_init(&dst->mtx, NULL);

#ifdef _WIN32
    dst->data = calloc(len, sizeof(void*));
    if(dst->data == NULL) {
        dbg_perror("calloc()");
        free(dst);
        return NULL;
    }
    dst->len = len;
#else
    dst->data = mmap(NULL, len * sizeof(void *), PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_NORESERVE | MAP_ANON, -1, 0);
    if (dst->data == MAP_FAILED) {
        dbg_perror("mmap(2)");
        free(dst);
        return (NULL);
    }
    dst->len = len;
#endif

    return (dst);
}

int
map_insert(struct map *m, int idx, void *ptr)
{
    if (unlikely(idx < 0 || idx > (int)m->len))
           return (-1);

    int rv;
    pthread_rwlock_wrlock(&m->mtx);
    if (m->data[idx] == NULL) {
        m->data[idx] = ptr;
        dbg_printf("idx=%i - inserted ptr=%p into map", idx, ptr);
        rv = 0;
    } else {
        dbg_printf("idx=%i - tried to insert ptr=%p into a non-empty location (cur_ptr=%p)",
                   idx, ptr, m->data[idx]);
        rv = -1;
    }
    pthread_rwlock_unlock(&m->mtx);

    return (rv);
}

void *
map_lookup(struct map *m, int idx)
{
    if (unlikely(idx < 0 || idx > (int)m->len))
        return (NULL);

    void *rv;
    pthread_rwlock_rdlock(&m->mtx);
    rv = m->data[idx];
    pthread_rwlock_unlock(&m->mtx);

    return (rv);
}

void *
map_delete(struct map *m, int idx)
{
    if (unlikely(idx < 0 || idx > (int)m->len))
           return ((void *)-1);

    pthread_rwlock_wrlock(&m->mtx);
    void *old_value = m->data[idx];
    m->data[idx] = NULL;
    pthread_rwlock_unlock(&m->mtx);

    return (old_value);
}
