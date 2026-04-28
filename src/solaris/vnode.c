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
 * EVFILT_VNODE on illumos via PORT_SOURCE_FILE.
 *
 * port_associate(PORT_SOURCE_FILE, &fobj, FILE_*, kn) monitors a
 * pathname, comparing the seeded mtime/atime/ctime against the
 * current stat each time the kernel checks.  Mapping to kqueue's
 * NOTE_* flags:
 *
 *      NOTE_DELETE   <-  FILE_DELETE
 *      NOTE_WRITE    <-  FILE_MODIFIED
 *      NOTE_EXTEND   <-  FILE_MODIFIED  (no separate signal)
 *      NOTE_ATTRIB   <-  FILE_ATTRIB
 *      NOTE_RENAME   <-  FILE_RENAME_TO | FILE_RENAME_FROM
 *      NOTE_TRUNCATE <-  FILE_TRUNC
 *      NOTE_LINK     <-  (no direct equivalent)
 *      NOTE_REVOKE   <-  (no direct equivalent)
 *
 * Like PORT_SOURCE_FD, PORT_SOURCE_FILE is one-shot per association:
 * after each event delivery we have to re-port_associate to keep
 * watching, so we cache the file_obj_t and FILE_* mask on the knote
 * (kn_vnode_fobj / kn_vnode_events).  The caller-supplied path is
 * referenced from fobj.fo_name, so we keep our own heap copy in
 * kn_vnode_path for the lifetime of the knote.
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
    if (fflags & NOTE_ATTRIB)   e |= FILE_ATTRIB;
    if (fflags & NOTE_RENAME)   e |= FILE_RENAME_TO | FILE_RENAME_FROM;
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
    return (f);
}

/*
 * Resolve the path for a knote that was registered against a file
 * descriptor.  EVFILT_VNODE in BSD takes an open fd as ident, but
 * PORT_SOURCE_FILE wants a pathname.  Resolve via /proc/self/path/<fd>
 * symlink (illumos-specific) so callers can keep the BSD-style fd
 * idiom.
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
 * Seed kn->kn_vnode_fobj from the caller-supplied identifier (an
 * open fd), then port_associate.  Stash everything we need to
 * re-arm later in the knote.
 */
static int
vnode_arm(struct filter *filt, struct knote *kn)
{
    struct stat st;
    int fd = (int) kn->kev.ident;
    unsigned int events;

    if (kn->kn_vnode_path == NULL) {
        kn->kn_vnode_path = fd_to_path(fd);
        if (kn->kn_vnode_path == NULL) {
            dbg_perror("readlink(/proc/self/path/<fd>)");
            return (-1);
        }
    }

    if (fstat(fd, &st) < 0) {
        dbg_perror("fstat");
        return (-1);
    }

    memset(&kn->kn_vnode_fobj, 0, sizeof(kn->kn_vnode_fobj));
    kn->kn_vnode_fobj.fo_name  = kn->kn_vnode_path;
    kn->kn_vnode_fobj.fo_mtime = st.st_mtim;
    kn->kn_vnode_fobj.fo_atime = st.st_atim;
    kn->kn_vnode_fobj.fo_ctime = st.st_ctim;

    events = fflags_to_file_events(kn->kev.fflags);
    kn->kn_vnode_events = events;

    if (port_associate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                       (uintptr_t) &kn->kn_vnode_fobj, events, kn) < 0) {
        dbg_perror("port_associate(PORT_SOURCE_FILE)");
        return (-1);
    }
    return (0);
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    return vnode_arm(filt, kn);
}

int
evfilt_vnode_knote_modify(struct filter *filt, struct knote *kn,
                          const struct kevent *kev)
{
    /*
     * port_dissociate (if still associated) + re-arm with the new
     * fflags.  It's fine if the dissociate fails (already
     * auto-removed by a prior delivery).
     */
    (void) port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                           (uintptr_t) &kn->kn_vnode_fobj);
    kn->kev.fflags = kev->fflags;
    return vnode_arm(filt, kn);
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{
    (void) port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                           (uintptr_t) &kn->kn_vnode_fobj);
    free(kn->kn_vnode_path);
    kn->kn_vnode_path = NULL;
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
    (void) port_dissociate(filter_epoll_fd(filt), PORT_SOURCE_FILE,
                           (uintptr_t) &kn->kn_vnode_fobj);
    return (0);
}

int
evfilt_vnode_copyout(struct kevent *dst, UNUSED int nevents,
                     struct filter *filt, struct knote *src, void *ptr)
{
    port_event_t *pe = (port_event_t *) ptr;

    (void) filt;

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags = file_events_to_fflags(pe->portev_events);

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
