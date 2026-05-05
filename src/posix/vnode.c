/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

/*
 * struct stat's nanosecond accessors (st_mtim / st_ctim) need
 * the POSIX-2008 feature level; the project's _XOPEN_SOURCE=600
 * is POSIX-2001.  Bump locally so we can read sub-second
 * timestamps from the snapshot.  Spelled per-platform because
 * just bumping _POSIX_C_SOURCE doesn't always re-evaluate the
 * struct stat layout under glibc.
 */
#ifdef __linux__
#  define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

/*
 * EVFILT_VNODE on POSIX, implemented as fstat-snapshot polling.
 *
 * portable POSIX has no file-change notification primitive, so each
 * vnode knote keeps a struct stat snapshot taken at registration.
 * On every dispatch pass we fstat the fd, diff against the snapshot,
 * and translate field changes into NOTE_* fflags.  The snapshot
 * updates only when we emit, so a burst of changes that drains
 * within one poll interval coalesces into a single event whose
 * fflags is the union of everything that happened.
 *
 * Vnode knotes count as always-ready (they ride the kf_ready /
 * kq_always_ready accounting), so the wait loop's polling-interval
 * mechanism (NOTE_FILE_POLL_INTERVAL on EVFILT_LIBKQUEUE) controls
 * how often the diff runs.  Default is sched_yield each loop
 * (cooperative spin); set a positive interval to switch to actual
 * sleep-between-polls.
 *
 * Detection coverage:
 *   NOTE_DELETE   st_nlink dropped to 0 (last name unlinked).
 *   NOTE_EXTEND   st_size grew.
 *   NOTE_TRUNCATE st_size shrank (where the spec defines NOTE_TRUNCATE).
 *   NOTE_ATTRIB   st_ctime advanced without size moving.  Catches
 *                 chmod/chown/utimes/touch.
 *   NOTE_LINK     st_nlink changed (any direction, including to 0).
 *   NOTE_WRITE    not deliverable.  Size-only detection misses
 *                 same-size overwrites and dir-content mutations;
 *                 mtime-based detection conflates touch/utimes with
 *                 real writes.  Stripped from the public header on
 *                 the POSIX backend rather than ship a half-broken
 *                 NOTE_WRITE.
 *   NOTE_RENAME   not detected.  fstat through the fd reports the
 *                 same inode regardless of path, and POSIX has no
 *                 portable way to ask "what's my path now?".
 *                 Also stripped from the public header.
 *   NOTE_REVOKE   not portable, undef'd in the public header.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/private.h"

struct posix_vnode_state {
    struct stat last;       /* snapshot from the previous emit */
};

static struct posix_vnode_state *
vnode_state_alloc(int fd)
{
    struct posix_vnode_state *vs;
    struct stat               st;

    if (fstat(fd, &st) < 0) {
        dbg_perror("fstat");
        return NULL;
    }
    /*
     * Match BSD: EVFILT_VNODE only attaches to vnodes (regular
     * files and directories).  Pipes, sockets, etc. have no
     * lifecycle to observe via stat polling - reject early
     * with EINVAL rather than silently never firing.
     */
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        errno = EINVAL;
        return NULL;
    }
    vs = calloc(1, sizeof(*vs));
    if (vs == NULL) {
        dbg_perror("calloc");
        return NULL;
    }
    vs->last = st;
    return vs;
}

/*
 * st_mtime / st_ctime are seconds-only, which can hide rapid
 * touches that happen within the same wall-clock second.  Use
 * the POSIX-2008 nanosecond timespec accessors where available;
 * fall back to seconds otherwise.  Linux glibc, FreeBSD, illumos
 * and macOS all expose st_mtim / st_ctim.
 */
#if defined(__APPLE__)
/*
 * Apple keeps the BSD-flavour st_*timespec names regardless of
 * 64-bit-inode setting; they never added the POSIX-2008 st_mtim
 * spelling.  All other supported targets (Linux glibc, FreeBSD,
 * illumos) ship the POSIX names.
 */
#  define ST_MTIM(s)  ((s)->st_mtimespec)
#  define ST_CTIM(s)  ((s)->st_ctimespec)
#else
#  define ST_MTIM(s)  ((s)->st_mtim)
#  define ST_CTIM(s)  ((s)->st_ctim)
#endif

static int
ts_neq(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec || a->tv_nsec != b->tv_nsec;
}

/*
 * Diff the current stat against the stored snapshot, return the
 * union of NOTE_* bits the change implies.  Heuristics, since we
 * only see the after-state, not the underlying syscall:
 *
 *   NOTE_EXTEND  = size grew.
 *   NOTE_TRUNCATE = size shrank.
 *   NOTE_ATTRIB  = ctime moved without size moving.  Covers
 *                  chmod / chown / utimes / touch.
 *   NOTE_LINK    = st_nlink changed (any direction, including to 0).
 *   NOTE_DELETE  = st_nlink dropped to 0.
 */
static unsigned int
vnode_diff_to_note(const struct stat *prev, const struct stat *now)
{
    unsigned int fflags = 0;
    int          size_changed = (now->st_size != prev->st_size);

#ifdef NOTE_DELETE
    if (now->st_nlink == 0 && prev->st_nlink != 0)
        fflags |= NOTE_DELETE;
#endif
#ifdef NOTE_LINK
    if (now->st_nlink != prev->st_nlink)
        fflags |= NOTE_LINK;
#endif
#ifdef NOTE_EXTEND
    if (now->st_size > prev->st_size)
        fflags |= NOTE_EXTEND;
#endif
#ifdef NOTE_TRUNCATE
    if (now->st_size < prev->st_size)
        fflags |= NOTE_TRUNCATE;
#endif
#ifdef NOTE_ATTRIB
    /*
     * unlink() always advances ctime without moving size; suppress NOTE_ATTRIB
     * when NOTE_DELETE fires so callers don't see spurious attribute events.
     */
    if (!size_changed && ts_neq(&ST_CTIM(now), &ST_CTIM(prev)) && !(fflags & NOTE_DELETE))
        fflags |= NOTE_ATTRIB;
#endif
    return fflags;
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    struct kqueue *kq = filt->kf_kqueue;

    kn->kn_vnode = vnode_state_alloc((int) kn->kev.ident);
    if (kn->kn_vnode == NULL)
        return (-1);

    /*
     * Vnode knotes are always-ready and dispatched drain-style:
     * we don't link to kf_ready (the dispatcher detaches before
     * kf_copyout, which would lose the always-ready property);
     * copyout instead walks the filter's knote index via
     * knote_foreach.  We still bump kq_always_ready so the wait
     * loop knows to drive periodic re-evaluation.
     */
    if (!(kn->kn_flags & KNFL_ALWAYS_READY_BUMPED)) {
        kq->kq_always_ready++;
        kn->kn_flags |= KNFL_ALWAYS_READY_BUMPED;
    }
    posix_wake_kqueue(kq);
    return (0);
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{
    struct kqueue *kq = filt->kf_kqueue;

    if (kn->kn_flags & KNFL_ALWAYS_READY_BUMPED) {
        if (kq->kq_always_ready > 0)
            kq->kq_always_ready--;
        kn->kn_flags &= ~KNFL_ALWAYS_READY_BUMPED;
    }
    if (kn->kn_vnode != NULL) {
        free(kn->kn_vnode);
        kn->kn_vnode = NULL;
    }
    return (0);
}

int
evfilt_vnode_knote_modify(UNUSED struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    /*
     * BSD modify replaces the watch mask (kev.fflags) but leaves
     * the knote's existence alone.  Take the new fflags and
     * udata; everything else stays.
     */
    kn->kev.fflags = kev->fflags;
    kn->kev.udata  = kev->udata;
    return (0);
}

int
evfilt_vnode_knote_enable(struct filter *filt, struct knote *kn)
{
    struct kqueue *kq = filt->kf_kqueue;

    if (!(kn->kn_flags & KNFL_ALWAYS_READY_BUMPED)) {
        if (!LIST_INSERTED(kn, kn_ready))
            LIST_INSERT_HEAD(&filt->kf_ready, kn, kn_ready);
        kq->kq_always_ready++;
        kn->kn_flags |= KNFL_ALWAYS_READY_BUMPED;
    }
    /*
     * Re-seed the snapshot so changes that happened while
     * disabled don't all fire on the next poll.  Matches BSD's
     * EV_ENABLE-resets-state semantics.
     */
    if (kn->kn_vnode != NULL && fstat((int) kn->kev.ident, &kn->kn_vnode->last) < 0)
        dbg_perror("fstat (enable)");
    posix_wake_kqueue(kq);
    return (0);
}

int
evfilt_vnode_knote_disable(struct filter *filt, struct knote *kn)
{
    struct kqueue *kq = filt->kf_kqueue;

    if (kn->kn_flags & KNFL_ALWAYS_READY_BUMPED) {
        if (LIST_INSERTED(kn, kn_ready))
            LIST_REMOVE_ZERO(kn, kn_ready);
        if (kq->kq_always_ready > 0)
            kq->kq_always_ready--;
        kn->kn_flags &= ~KNFL_ALWAYS_READY_BUMPED;
    }
    return (0);
}

/*
 * Per-knote copyout: stat the fd, diff against the snapshot,
 * emit if any of the consumer's requested NOTE_* bits triggered.
 * Updates the snapshot on emit so subsequent calls don't re-fire
 * for the same change.
 */
static int
evfilt_vnode_copyout_one(struct kevent *dst, struct filter *filt,
        struct knote *src)
{
    struct stat   now;
    unsigned int  changed;
    unsigned int  emit;

    if (src->kn_vnode == NULL || KNOTE_DISABLED(src))
        return (0);
    if (fstat((int) src->kev.ident, &now) < 0) {
        if (errno == EBADF && (src->kev.fflags & NOTE_DELETE)) {
            memcpy(dst, &src->kev, sizeof(*dst));
            dst->fflags = NOTE_DELETE;
            if (knote_copyout_flag_actions(filt, src) < 0)
                return (-1);
            return (1);
        }
        return (0);
    }

    changed = vnode_diff_to_note(&src->kn_vnode->last, &now);
    emit    = changed & src->kev.fflags;
    dbg_printf("kn=%p prev{size=%lld ctim=%ld.%09ld} now{size=%lld ctim=%ld.%09ld} changed=%#x mask=%#x emit=%#x",
               src,
               (long long) src->kn_vnode->last.st_size,
               (long) ST_CTIM(&src->kn_vnode->last).tv_sec,
               (long) ST_CTIM(&src->kn_vnode->last).tv_nsec,
               (long long) now.st_size,
               (long) ST_CTIM(&now).tv_sec,
               (long) ST_CTIM(&now).tv_nsec,
               changed, (unsigned int) src->kev.fflags, emit);
    if (emit == 0)
        return (0);

    /*
     * BSD spec: dst->fflags is the full set of bits the change
     * actually triggered, not just those the consumer asked for.
     * The mask only gates whether we emit at all.
     */
    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags = changed;
    src->kn_vnode->last = now;

    if (knote_copyout_flag_actions(filt, src) < 0)
        return (-1);
    return (1);
}

struct vnode_drain_ctx {
    struct filter *filt;
    struct kevent *eventlist;
    int            nevents;
    int            nout;
    int            err;
};

static int
vnode_drain_cb(struct knote *kn, void *uctx)
{
    struct vnode_drain_ctx *c = uctx;
    int rv;

    if (c->nout >= c->nevents)
        return (1);
    rv = evfilt_vnode_copyout_one(c->eventlist + c->nout, c->filt, kn);
    if (rv < 0) {
        c->err = -1;
        return (1);
    }
    c->nout += rv;
    return (0);
}

int
evfilt_vnode_copyout(struct kevent *dst, int nevents, struct filter *filt,
        UNUSED struct knote *src, UNUSED void *ptr)
{
    struct vnode_drain_ctx c = {
        .filt = filt, .eventlist = dst, .nevents = nevents,
        .nout = 0, .err = 0,
    };

    (void) knote_foreach(filt, vnode_drain_cb, &c);
    return c.err < 0 ? -1 : c.nout;
}

const struct filter evfilt_vnode = {
    .kf_id      = EVFILT_VNODE,
    .kf_copyout = evfilt_vnode_copyout,
    .kn_create  = evfilt_vnode_knote_create,
    .kn_modify  = evfilt_vnode_knote_modify,
    .kn_delete  = evfilt_vnode_knote_delete,
    .kn_enable  = evfilt_vnode_knote_enable,
    .kn_disable = evfilt_vnode_knote_disable,
};
