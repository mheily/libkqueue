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
 * EVFILT_VNODE via PORT_SOURCE_FILE.
 *
 * port_associate(PORT_SOURCE_FILE, &fobj, FILE_*, kn) watches the
 * pathname; the kernel raises events when the filesystem changes
 * the inode.  NOTE_* mapping:
 *
 *      NOTE_DELETE   <-  FILE_DELETE
 *      NOTE_WRITE    <-  FILE_MODIFIED
 *      NOTE_EXTEND   <-  FILE_MODIFIED + st_size delta
 *      NOTE_ATTRIB   <-  FILE_ATTRIB
 *      NOTE_LINK     <-  FILE_ATTRIB + st_nlink delta
 *      NOTE_RENAME   <-  FILE_RENAME_TO | FILE_RENAME_FROM
 *      NOTE_TRUNCATE <-  FILE_TRUNC
 *      NOTE_REVOKE   <-  (no equivalent)
 *
 * NOTE_EXTEND and NOTE_LINK have no dedicated FILE_* event; we fstat
 * at arm time and on each delivery to detect size / nlink deltas.
 *
 * PORT_SOURCE_FILE is one-shot per delivery, so we cache the file_obj_t
 * and FILE_* mask in the knote and re-associate on each wake.  The
 * pathname is heap-copied (fobj.fo_name aliases caller storage).
 */

#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "../common/private.h"

static unsigned int
fflags_to_file_events(unsigned int fflags)
{
    unsigned int e = 0;

    if (fflags & NOTE_DELETE)   e |= FILE_DELETE;
    if (fflags & NOTE_WRITE)    e |= FILE_MODIFIED;
    if (fflags & NOTE_EXTEND)   e |= FILE_MODIFIED;
    /*
     * NOTE_LINK rides on FILE_ATTRIB - st_nlink change is reported
     * by the kernel as an attribute change.
     */
    if (fflags & (NOTE_ATTRIB | NOTE_LINK))
                                e |= FILE_ATTRIB;
    if (fflags & NOTE_RENAME)   e |= FILE_RENAME_TO | FILE_RENAME_FROM;
    if (fflags & NOTE_TRUNCATE) e |= FILE_TRUNC;
    return (e);
}

static unsigned int
file_events_to_fflags(unsigned int events)
{
    unsigned int f = 0;

    if (events & FILE_DELETE)        f |= NOTE_DELETE;
    if (events & FILE_MODIFIED)      f |= NOTE_WRITE;
    if (events & FILE_ATTRIB)        f |= NOTE_ATTRIB;
    if (events & FILE_RENAME_TO)     f |= NOTE_RENAME;
    if (events & FILE_RENAME_FROM)   f |= NOTE_RENAME;
    if (events & FILE_TRUNC)         f |= NOTE_TRUNCATE;
    return (f);
}

/*
 * BSD's EVFILT_VNODE takes an fd; PORT_SOURCE_FILE wants a path.
 * Resolve via the /proc/self/path/<fd> symlink (illumos-specific).
 */
static char *
fd_to_path(int fd)
{
    char link[64];
    char buf[PATH_MAX + 1];
    ssize_t n;

    snprintf(link, sizeof(link), "/proc/self/path/%d", fd);
    n = readlink(link, buf, sizeof(buf) - 1);
    if (n < 0)
        return (NULL);
    buf[n] = '\0';
    return (strdup(buf));
}

/*
 * Seed kn->kn_vnode.fobj from the caller-supplied identifier (an
 * open fd), then port_associate.  Stash everything we need to
 * re-arm later in the knote.
 */
static int
vnode_arm(struct filter *filt, struct knote *kn)
{
    struct stat st;
    int fd = (int) kn->kev.ident;
    unsigned int events;
    bool fresh_udata = false;

    if (kn->kn_vnode.path == NULL) {
        kn->kn_vnode.path = fd_to_path(fd);
        if (kn->kn_vnode.path == NULL) {
            dbg_perror("readlink(/proc/self/path/<fd>)");
            return (-1);
        }
    }

    if (fstat(fd, &st) < 0) {
        dbg_perror("fstat");
        return (-1);
    }

    memset(&kn->kn_vnode.fobj, 0, sizeof(kn->kn_vnode.fobj));
    kn->kn_vnode.fobj.fo_name  = kn->kn_vnode.path;
    kn->kn_vnode.fobj.fo_mtime = st.st_mtim;
    kn->kn_vnode.fobj.fo_atime = st.st_atim;
    kn->kn_vnode.fobj.fo_ctime = st.st_ctim;

    /*
     * Save current size and nlink as the baseline.  Copyout re-fstats
     * on each delivery and emits NOTE_EXTEND / NOTE_LINK if they changed.
     */
    kn->kn_vnode.nlink = st.st_nlink;
    kn->kn_vnode.size  = st.st_size;

    events = fflags_to_file_events(kn->kev.fflags);
    kn->kn_vnode.events = events;

    if (kn->kn_udata == NULL) {
        if (KN_UDATA_ALLOC(kn) == NULL) {
            dbg_puts("port_udata_alloc");
            return (-1);
        }
        fresh_udata = true;
    }

    if (port_associate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                       (uintptr_t) &kn->kn_vnode.fobj, events, kn->kn_udata) < 0) {
        dbg_perror("port_associate(PORT_SOURCE_FILE)");
        if (fresh_udata)
            KN_UDATA_FREE(kn);
        return (-1);
    }
    return (0);
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    return vnode_arm(filt, kn);
}

int
evfilt_vnode_knote_modify(struct filter *filt, struct knote *kn,
                          const struct kevent *kev)
{
    /*
     * port_dissociate (if still associated) + re-arm with the new
     * fflags.  It's fine if the dissociate fails (already
     * auto-removed by a prior delivery); just log.
     */
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                        (uintptr_t) &kn->kn_vnode.fobj) < 0)
        dbg_perror("port_dissociate(PORT_SOURCE_FILE) on EV_MODIFY (likely already auto-removed)");
    kn->kev.fflags = kev->fflags;
    return vnode_arm(filt, kn);
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                        (uintptr_t) &kn->kn_vnode.fobj) < 0)
        dbg_perror("port_dissociate(PORT_SOURCE_FILE) on EV_DELETE (likely already auto-removed)");
    free(kn->kn_vnode.path);
    kn->kn_vnode.path = NULL;
    if (kn->kn_udata != NULL)
        KN_UDATA_DEFER_FREE(filt->kf_kqueue, kn);
    return (0);
}

int
evfilt_vnode_knote_enable(struct filter *filt, struct knote *kn)
{
    return vnode_arm(filt, kn);
}

int
evfilt_vnode_knote_disable(struct filter *filt, struct knote *kn)
{
    if (port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                        (uintptr_t) &kn->kn_vnode.fobj) < 0)
        dbg_perror("port_dissociate(PORT_SOURCE_FILE) on EV_DISABLE (likely already auto-removed)");
    return (0);
}

int
evfilt_vnode_copyout(struct kevent *dst, UNUSED int nevents,
                     struct filter *filt, struct knote *src, void *ptr)
{
    port_event_t *pe = (port_event_t *) ptr;
    struct stat   st;

    (void) filt;

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags = file_events_to_fflags(pe->portev_events);

    /*
     * Synthesise NOTE_EXTEND / NOTE_LINK from a fresh fstat: the
     * kernel groups size and link-count changes under FILE_MODIFIED /
     * FILE_ATTRIB, so we have to compare ourselves.  fstat against
     * the original ident (an fd) avoids races against rename: a path
     * that was renamed away still resolves through the open fd.
     *
     * If fstat fails we fall back to whatever the FILE_* mapping
     * gave us; deltas just won't be reported for this delivery.
     */
    if (fstat((int) src->kev.ident, &st) == 0) {
        if ((st.st_size > src->kn_vnode.size) &&
            (src->kev.fflags & NOTE_EXTEND))
            dst->fflags |= NOTE_EXTEND;
        if ((st.st_nlink != src->kn_vnode.nlink) &&
            (src->kev.fflags & NOTE_LINK))
            dst->fflags |= NOTE_LINK;
        src->kn_vnode.size  = st.st_size;
        src->kn_vnode.nlink = st.st_nlink;
    }

    /*
     * The mapping above can set NOTE_* the caller didn't register for:
     * FILE_ATTRIB raises NOTE_ATTRIB and NOTE_LINK, but the caller may
     * only want one.  Restrict to what they asked for.
     */
    dst->fflags &= src->kev.fflags;

    if (knote_copyout_flag_actions(filt, src) < 0) return (-1);
    return (1);
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
