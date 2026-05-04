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

#ifndef NDEBUG
static char *
inotify_mask_dump(uint32_t mask)
{
    static __thread char buf[1024];

#define INEVT_MASK_DUMP(attrib) \
    if (mask & attrib) \
       strcat(buf, #attrib" ");

    snprintf(buf, sizeof(buf), "mask = %d (", mask);
    INEVT_MASK_DUMP(IN_ACCESS);
    INEVT_MASK_DUMP(IN_MODIFY);
    INEVT_MASK_DUMP(IN_ATTRIB);
    INEVT_MASK_DUMP(IN_CLOSE_WRITE);
    INEVT_MASK_DUMP(IN_CLOSE_NOWRITE);
    INEVT_MASK_DUMP(IN_OPEN);
    INEVT_MASK_DUMP(IN_MOVED_FROM);
    INEVT_MASK_DUMP(IN_MOVED_TO);
    INEVT_MASK_DUMP(IN_CREATE);
    INEVT_MASK_DUMP(IN_DELETE);
    INEVT_MASK_DUMP(IN_DELETE_SELF);
    INEVT_MASK_DUMP(IN_MOVE_SELF);
    buf[strlen(buf) - 1] = ')';

    return (buf);
}

static char *
inotify_event_dump(struct inotify_event *evt)
{
    static __thread char buf[1024];

    if (evt->len > 0)
        snprintf(buf, sizeof(buf), "wd=%d mask=%s name=%s",
                evt->wd,
                inotify_mask_dump(evt->mask),
                evt->name);
    else
        snprintf(buf, sizeof(buf), "wd=%d mask=%s",
                evt->wd,
                inotify_mask_dump(evt->mask));

    return (buf);
}

#endif /* !NDEBUG */


/*
 * Returns:
 *   1  successfully read one inotify event into dst
 *   0  EAGAIN: another reader on the same inofd already drained
 *      it; caller must skip dispatch for this wakeup
 *  -1  read failed for some other reason
 */
int
get_one_event(struct inotify_event *dst, size_t len, int inofd)
{
    ssize_t n;
    size_t want = sizeof(struct inotify_event);

    dbg_puts("reading one inotify event");
    for (;;) {
        if (len < want) {
            dbg_printf("Needed %zu bytes have %zu bytes", want, len);
            return (-1);
        }

        /*
         * Last member of struct inotify_event is the name field.
         * this field will be padded to the next multiple of
         * struct inotify_event.
         *
         * We need this loop here so that we don't accidentally
         * read more than one inotify event per read call which
         * could happen if the event's name field were 0.
         */
        n = read(inofd, dst, want);
        if (n < 0) {
            switch (errno) {
            case EINVAL:
                want += sizeof(struct inotify_event);
                /* FALL-THROUGH */

            case EINTR:
                continue;

            case EAGAIN:
                /*
                 * Another waiter on the same kq already consumed
                 * the inotify event.
                 */
                return (0);
            }

            dbg_perror("read");
            return (-1);
        }
        break;
    }

    dbg_printf("read(2) from inotify wd: %ld bytes", (long)n);

#ifdef __COVERITY__
    /* Coverity complains this isn't \0 terminated, but it is */
    if (evt->len > 0) evt->name[ev->len - 1] = '\0';
#endif

    return (1);
}

static int
add_watch(struct filter *filt, struct knote *kn)
{
    int ifd;
    char path[PATH_MAX];
    uint32_t mask;

    /* Convert the fd to a pathname */
    if (linux_fd_to_path(path, sizeof(path), kn->kev.ident) < 0)
        return (-1);

    /* Convert the fflags to the inotify mask */
    mask = IN_CLOSE;
    if (kn->kev.fflags & NOTE_DELETE)
        mask |= IN_ATTRIB | IN_DELETE_SELF;
    if (kn->kev.fflags & NOTE_WRITE)
        mask |= IN_MODIFY | IN_ATTRIB;
    if (kn->kev.fflags & NOTE_EXTEND)
        mask |= IN_MODIFY | IN_ATTRIB;
    if (kn->kev.fflags & NOTE_TRUNCATE)
        mask |= IN_MODIFY | IN_ATTRIB;
    if ((kn->kev.fflags & NOTE_ATTRIB) ||
            (kn->kev.fflags & NOTE_LINK))
        mask |= IN_ATTRIB;
    if (kn->kev.fflags & NOTE_RENAME)
        mask |= IN_MOVE_SELF;
    if (kn->kev.flags & EV_ONESHOT)
        mask |= IN_ONESHOT;

    /*
     * IN_NONBLOCK so concurrent readers (multiple kevent() callers
     * on the same kq racing for the same inotify event) detect
     * "another thread got there first" via EAGAIN rather than
     * blocking forever.  See get_one_event() and
     * evfilt_vnode_copyout() for the consumer side.
     */
    ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ifd < 0) {
        if ((errno == EMFILE) || (errno == ENFILE)) {
            dbg_perror("inotify_init(2) fd_used=%u fd_max=%u", get_fd_used(), get_fd_limit());
        } else {
            dbg_perror("inotify_init(2)");
        }
        return (-1);
    }

    /* Add the watch */
    dbg_printf("inotify_add_watch(2); inofd=%d flags=%s path=%s",
            ifd, inotify_mask_dump(mask), path);
    kn->kev.data = inotify_add_watch(ifd, path, mask);
    if (kn->kev.data < 0) {
        dbg_perror("inotify_add_watch(2)");
        goto errout;
    }

    /* Add the inotify fd to the epoll set */
    KN_UDATA_ALLOC(kn);   /* populate this knote's kn_udata field */
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_ADD, ifd, EPOLL_EV_KN(EPOLLIN, kn)) < 0) {
        dbg_perror("epoll_ctl(2)");
        goto errout;
    }

    kn->kn_vnode.inotifyfd = ifd;

    return (0);

errout:
    inotify_rm_watch(ifd, kn->kev.data);
    kn->kn_vnode.inotifyfd = -1;
    (void) close(ifd);
    /*
     * If we reached here from the post-KN_UDATA epoll_ctl failure,
     * the kernel never accepted the udata - free direct.  If we came
     * from the pre-KN_UDATA inotify_add_watch failure, kn_udata is
     * still NULL and KN_UDATA_FREE is a no-op.
     */
    KN_UDATA_FREE(kn);
    return (-1);
}

static int
delete_watch(struct filter *filt, struct knote *kn)
{
    int ifd = kn->kn_vnode.inotifyfd;

    if (ifd < 0)
        return (0);
    if (epoll_ctl(filter_epoll_fd(filt), EPOLL_CTL_DEL, ifd, NULL) < 0) {
        dbg_perror("epoll_ctl(2)");
        return (-1);
    }

    /*
     * Tear down the udata too: a re-enable will allocate a fresh
     * one in add_watch().  Doing it here keeps the disable->enable
     * cycle leak-free.
     */
    KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);

    (void) close(ifd);
    kn->kn_vnode.inotifyfd = -1;

    return (0);
}

int
evfilt_vnode_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, void *ptr UNUSED)
{
    uint8_t buf[sizeof(struct inotify_event) + NAME_MAX + 1] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    struct inotify_event *evt = (struct inotify_event *)buf;
    struct stat sb;
    uint32_t merged_mask = 0;

    /*
     * Drain every inotify event currently readable for this knote's
     * watch and OR-merge their masks.  Matches BSD's VFS-layer
     * coalescing: multiple VOPs between two kevent() drains deliver
     * one event whose fflags is the union of every triggering bit.
     * One inotify fd per knote, so all drained events belong here.
     */
    for (;;) {
        int rv = get_one_event(evt, sizeof(buf), src->kn_vnode.inotifyfd);
        if (rv < 0) return (-1);
        if (rv == 0) break;

        dbg_printf("inotify event: %s", inotify_event_dump(evt));

        if (evt->mask & IN_IGNORED) {
            /*
             * IN_IGNORED follows any rm_watch (incl. our own
             * EV_DISABLE/EV_ENABLE cycle, which generates a
             * stale IN_IGNORED on a wd the kernel may then
             * reuse for the new watch).  Skip it: real file
             * deletion already surfaces via IN_DELETE_SELF.
             */
            continue;
        }
        /*
         * IN_CLOSE_WRITE / IN_CLOSE_NOWRITE: any process closed
         * an fd to the file.  That doesn't mean the watch should
         * end; multiple processes can hold fds.  No NOTE_* maps
         * to this, so drop the bit before merging.
         */
        merged_mask |= evt->mask & ~(IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
    }

    /*
     * Race: another waiter drained the inotify fd before we got
     * here, leaving us with nothing to deliver.  IN_NONBLOCK on
     * the inotify fd lets the losing read return rv=0 instead of
     * blocking forever.  Also reaches here when every drained bit
     * was IN_IGNORED / IN_CLOSE_* (informational, no NOTE_* map).
     */
    if (merged_mask == 0)
        return (0);

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->data = 0;
    dst->fflags = 0;

    /* No error checking because fstat(2) should rarely fail */
    //FIXME: EINTR
    if (fstat(src->kev.ident, &sb) < 0 && errno == ENOENT) {
        if (src->kev.fflags & NOTE_DELETE)
            dst->fflags |= NOTE_DELETE;
    } else {
        if (merged_mask & (IN_ATTRIB | IN_MODIFY)) {
            if (sb.st_nlink == 0 && src->kev.fflags & NOTE_DELETE)
                dst->fflags |= NOTE_DELETE;
            if (sb.st_nlink != src->kn_vnode.nlink &&
                src->kev.fflags & NOTE_LINK)
                dst->fflags |= NOTE_LINK;
            /*
             * NOTE_TRUNCATE is an OpenBSD extension to BSD kqueue
             * (sys/event.h:107, ufs_setattr fires it only when
             * vap->va_size < oldsize).  Inotify delivers IN_MODIFY
             * for truncate(2) but doesn't distinguish it from a
             * write, so we synthesise from st_size shrinkage to match
             * OpenBSD's "shrink only" semantic.
             */
            if (sb.st_size < src->kn_vnode.size &&
                src->kev.fflags & NOTE_TRUNCATE)
                dst->fflags |= NOTE_TRUNCATE;
            /*
             * BSD libkqueue convention: a write that extends the file
             * delivers NOTE_EXTEND alongside NOTE_WRITE - so register
             * either to receive it.  Linux's inotify reports IN_MODIFY
             * for both append and overwrite; we synthesise NOTE_EXTEND
             * by comparing st_size against the cached baseline.
             */
            if (sb.st_size > src->kn_vnode.size &&
                src->kev.fflags & (NOTE_EXTEND | NOTE_WRITE))
                dst->fflags |= NOTE_EXTEND;
            src->kn_vnode.nlink = sb.st_nlink;
            src->kn_vnode.size = sb.st_size;
        }
    }

    if (merged_mask & IN_MODIFY && src->kev.fflags & NOTE_WRITE)
        dst->fflags |= NOTE_WRITE;
    if (merged_mask & IN_ATTRIB && src->kev.fflags & NOTE_ATTRIB)
        dst->fflags |= NOTE_ATTRIB;
    if (merged_mask & IN_MOVE_SELF && src->kev.fflags & NOTE_RENAME)
        dst->fflags |= NOTE_RENAME;
    if (merged_mask & IN_DELETE_SELF && src->kev.fflags & NOTE_DELETE)
        dst->fflags |= NOTE_DELETE;

    if (knote_copyout_flag_actions(filt, src) < 0) return -1;

    return (1);
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    struct stat sb;

    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    if (fstat(kn->kev.ident, &sb) < 0) {
        dbg_puts("fstat failed");
        return (-1);
    }
    kn->kn_vnode.nlink = sb.st_nlink;
    kn->kn_vnode.size = sb.st_size;
    kn->kev.data = -1;

    return (add_watch(filt, kn));
}

int
evfilt_vnode_knote_modify(struct filter *filt UNUSED, struct knote *kn,
        const struct kevent *kev)
{
    char path[PATH_MAX];
    uint32_t mask;
    int wd;

    if (kn->kn_vnode.inotifyfd < 0) {
        /*
         * Knote was disabled or never armed; nothing to update on
         * the kernel side.  Common code stamps the new fflags into
         * kn->kev itself before kn_enable runs, so there's nothing
         * for us to do here.  Leave kev.fflags alone - common code
         * doesn't sync it post-modify.
         */
        kn->kev.fflags = kev->fflags;
        return (0);
    }

    if (linux_fd_to_path(path, sizeof(path), kn->kev.ident) < 0)
        return (-1);

    /*
     * Recompute the inotify mask from the new fflags and replace
     * the existing watch.  inotify_add_watch with the same (fd,
     * path) pair updates the mask of the existing watch in place
     * and returns the same wd, so we don't need to remove the old
     * watch first.
     */
    mask = IN_CLOSE;
    if (kev->fflags & NOTE_DELETE)
        mask |= IN_ATTRIB | IN_DELETE_SELF;
    if (kev->fflags & NOTE_WRITE)
        mask |= IN_MODIFY | IN_ATTRIB;
    if (kev->fflags & NOTE_EXTEND)
        mask |= IN_MODIFY | IN_ATTRIB;
    if ((kev->fflags & NOTE_ATTRIB) || (kev->fflags & NOTE_LINK))
        mask |= IN_ATTRIB;
    if (kev->fflags & NOTE_RENAME)
        mask |= IN_MOVE_SELF;
    if (kn->kev.flags & EV_ONESHOT)
        mask |= IN_ONESHOT;

    dbg_printf("inotify_add_watch(2) replace; inofd=%d flags=%s path=%s",
               kn->kn_vnode.inotifyfd, inotify_mask_dump(mask), path);
    wd = inotify_add_watch(kn->kn_vnode.inotifyfd, path, mask);
    if (wd < 0) {
        dbg_perror("inotify_add_watch(2) on EV_MODIFY");
        return (-1);
    }

    kn->kev.fflags = kev->fflags;
    kn->kev.data   = wd;
    return (0);
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{
    /*
     * delete_watch handles the udata teardown: when ifd >= 0 it
     * defer-frees and nulls kn_udata; when ifd < 0 the knote was
     * already disabled (which goes through delete_watch too), so
     * kn_udata is already NULL.
     */
    return delete_watch(filt, kn);
}

int
evfilt_vnode_knote_enable(struct filter *filt, struct knote *kn)
{
    return add_watch(filt, kn);
}

int
evfilt_vnode_knote_disable(struct filter *filt, struct knote *kn)
{
    return delete_watch(filt, kn);
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
