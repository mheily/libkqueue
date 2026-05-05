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

#include "../common/private.h"

/*
 * Per-knote state ferried alongside the change-notification handle.
 * Allocated in knote_create and freed in knote_delete; pointed at
 * from kn->kn_handle via a tiny indirection so we keep the existing
 * (HANDLE-shaped) kn_handle slot for the FindFirstChangeNotification
 * handle and stash the rest off to the side.
 */
struct vnode_state {
    HANDLE          dir_handle;     /* FindFirstChangeNotification handle */
    wchar_t         *full_path;     /* canonical path of watched fd */
    LARGE_INTEGER   size;           /* last observed file size */
    DWORD           attrs;          /* last observed file attributes */
    DWORD           nlink;          /* last observed link count */
    FILETIME        mtime;          /* last observed last-write time */
};

static int
vnode_query(HANDLE fh, LARGE_INTEGER *size, DWORD *attrs, DWORD *nlink,
            FILETIME *mtime)
{
    BY_HANDLE_FILE_INFORMATION info;

    if (!GetFileInformationByHandle(fh, &info))
        return (-1);

    if (size) {
        size->LowPart = info.nFileSizeLow;
        size->HighPart = info.nFileSizeHigh;
    }
    if (attrs) *attrs = info.dwFileAttributes;
    if (nlink) *nlink = info.nNumberOfLinks;
    if (mtime) *mtime = info.ftLastWriteTime;

    return (0);
}

static wchar_t *
vnode_path_from_fd(int fd)
{
    HANDLE fh = (HANDLE)_get_osfhandle(fd);
    DWORD len, got;
    wchar_t *buf;

    if (fh == INVALID_HANDLE_VALUE)
        return NULL;

    len = GetFinalPathNameByHandleW(fh, NULL, 0, FILE_NAME_NORMALIZED);
    if (len == 0)
        return NULL;

    buf = malloc(sizeof(wchar_t) * (len + 1));
    if (buf == NULL)
        return NULL;

    got = GetFinalPathNameByHandleW(fh, buf, len + 1, FILE_NAME_NORMALIZED);
    if (got == 0 || got > len) {
        free(buf);
        return NULL;
    }
    return buf;
}

static wchar_t *
vnode_parent_dir(const wchar_t *path)
{
    wchar_t *copy, *slash;

    copy = _wcsdup(path);
    if (copy == NULL)
        return NULL;

    slash = wcsrchr(copy, L'\\');
    if (slash == NULL) slash = wcsrchr(copy, L'/');
    if (slash == NULL) {
        free(copy);
        return NULL;
    }
    *slash = L'\0';
    return copy;
}

static VOID CALLBACK
evfilt_vnode_callback(void *param, BOOLEAN fired)
{
    struct knote *kn;
    struct kqueue *kq;
    struct vnode_state *vs;

    assert(param);
    (void)fired;

    kn = (struct knote *)param;

    if (kn->kn_flags & KNFL_KNOTE_DELETED) {
        dbg_puts("knote marked for deletion, skipping event");
        return;
    }

    vs = (struct vnode_state *)kn->kn_handle;
    if (vs == NULL) return;

    /*
     * Re-arm the directory change notification.  If this fails the
     * knote is effectively dead; record it but still try to deliver
     * the current event to the user.
     */
    if (vs->dir_handle != INVALID_HANDLE_VALUE &&
        !FindNextChangeNotification(vs->dir_handle))
        dbg_lasterror("FindNextChangeNotification()");

    kq = kn->kn_kq;
    assert(kq);

    /*
     * Hold a ref for the queued completion so an EV_DELETE that
     * arrives before the entry is drained doesn't free the knote
     * out from under the dispatcher.  Released in copyout (or
     * dropped here on post failure).
     */
    knote_retain(kn);
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) kn)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return;
    }
}

int
evfilt_vnode_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *src, UNUSED void *ptr)
{
    struct vnode_state *vs;
    HANDLE fh;
    LARGE_INTEGER size;
    DWORD attrs, nlink;
    FILETIME mtime;
    int gone = 0;

    /*
     * Stale completion left over from a callback that posted
     * before EV_DELETE landed: discard it and balance the post's
     * ref so the knote can finally be freed.  This must come
     * before any access to kn_handle / vnode_state because the
     * EV_DELETE path tears that state down.
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        dst->filter = 0;
        knote_release(src);
        return (0);
    }

    /*
     * EV_DISABLE'd vnode: a callback posted before the disable
     * arrived is still queued; drop it.
     */
    if (src->kev.flags & EV_DISABLE) {
        knote_release(src);
        dst->filter = 0;
        return (0);
    }

    vs = (struct vnode_state *)src->kn_handle;

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->fflags = 0;
    dst->data = 0;

    fh = (HANDLE)_get_osfhandle(src->kev.ident);
    if (fh == INVALID_HANDLE_VALUE || vnode_query(fh, &size, &attrs, &nlink, &mtime) < 0) {
        gone = 1;
    } else {
        /*
         * When the watched fd was opened with FILE_SHARE_DELETE
         * (which the test suite needs so rename/unlink subtests
         * can run alongside the watching fd), DeleteFile leaves
         * the handle valid but in "delete pending" state.
         * GetFileInformationByHandle still succeeds, so the
         * straightforward "fstat fails -> gone" check above
         * misses NOTE_DELETE.  Probe DeletePending via
         * GetFileInformationByHandleEx and treat it as gone.
         */
        FILE_STANDARD_INFO si;
        if (GetFileInformationByHandleEx(fh, FileStandardInfo,
                                          &si, sizeof(si)) &&
            si.DeletePending)
            gone = 1;
    }

    if (gone) {
        if (src->kev.fflags & NOTE_DELETE)
            dst->fflags |= NOTE_DELETE;
    } else if (vs != NULL) {
        if (size.QuadPart != vs->size.QuadPart) {
            if (src->kev.fflags & NOTE_WRITE)
                dst->fflags |= NOTE_WRITE;
            /*
             * BSD/Linux semantics: a NOTE_WRITE consumer sees
             * NOTE_EXTEND whenever the file grew, regardless of
             * whether NOTE_EXTEND was explicitly requested.  Match
             * that so the shared test suite passes.
             */
            if (size.QuadPart > vs->size.QuadPart &&
                (src->kev.fflags & (NOTE_WRITE | NOTE_EXTEND)))
                dst->fflags |= NOTE_EXTEND;
#ifdef NOTE_TRUNCATE
            if (size.QuadPart < vs->size.QuadPart &&
                (src->kev.fflags & NOTE_TRUNCATE))
                dst->fflags |= NOTE_TRUNCATE;
#endif
        }
        if (src->kev.fflags & NOTE_ATTRIB) {
            if (attrs != vs->attrs ||
                mtime.dwLowDateTime != vs->mtime.dwLowDateTime ||
                mtime.dwHighDateTime != vs->mtime.dwHighDateTime)
                dst->fflags |= NOTE_ATTRIB;
        }
        if (nlink != vs->nlink && (src->kev.fflags & NOTE_LINK))
            dst->fflags |= NOTE_LINK;

        /*
         * NOTE_RENAME: if the canonical path changed since the
         * knote was created, the file moved.  GetFinalPathNameByHandle
         * follows the file across renames within a volume.
         */
        if (src->kev.fflags & NOTE_RENAME) {
            wchar_t *cur = vnode_path_from_fd(src->kev.ident);
            if (cur != NULL && vs->full_path != NULL &&
                wcscmp(cur, vs->full_path) != 0) {
                dst->fflags |= NOTE_RENAME;
                free(vs->full_path);
                vs->full_path = cur;
            } else if (cur != NULL) {
                free(cur);
            }
        }

        vs->size = size;
        vs->attrs = attrs;
        vs->nlink = nlink;
        vs->mtime = mtime;
    }

    /* Discard the event if no requested fflag fired. */
    if (dst->fflags == 0) {
        dst->filter = 0;
        knote_release(src);
        return (0);
    }

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return -1;
    }

    /* Balance the ref the callback's post took. */
    knote_release(src);
    return (1);
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    struct vnode_state *vs;
    HANDLE fh;
    wchar_t *path = NULL;
    wchar_t *parent = NULL;
    DWORD filter_flags;

    fh = (HANDLE)_get_osfhandle(kn->kev.ident);
    if (fh == INVALID_HANDLE_VALUE) {
        dbg_puts("invalid file descriptor");
        errno = EBADF;
        return (-1);
    }
    /*
     * BSD filt_vfsattach checks fp->f_type == DTYPE_VNODE and
     * returns EINVAL otherwise.  Reject pipes / sockets / consoles
     * the same way: only disk files have meaningful vnode events.
     */
    if (GetFileType(fh) != FILE_TYPE_DISK) {
        dbg_puts("EVFILT_VNODE requires a regular file");
        errno = EINVAL;
        return (-1);
    }

    vs = calloc(1, sizeof(*vs));
    if (vs == NULL) {
        dbg_perror("calloc");
        return (-1);
    }
    vs->dir_handle = INVALID_HANDLE_VALUE;

    if (vnode_query(fh, &vs->size, &vs->attrs, &vs->nlink, &vs->mtime) < 0) {
        dbg_lasterror("GetFileInformationByHandle()");
        goto err;
    }

    path = vnode_path_from_fd(kn->kev.ident);
    if (path == NULL) {
        dbg_lasterror("GetFinalPathNameByHandle()");
        goto err;
    }
    vs->full_path = path;

    parent = vnode_parent_dir(path);
    if (parent == NULL) {
        dbg_puts("could not derive parent directory");
        goto err;
    }

    /*
     * Map kevent fflags onto FILE_NOTIFY_CHANGE_* flags.  These
     * are dispatched per parent-directory, not per-file: when
     * any file in the directory changes we re-stat ours and
     * decide what (if anything) to deliver.
     */
    filter_flags = 0;
    if (kn->kev.fflags & (NOTE_WRITE | NOTE_EXTEND))
        filter_flags |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    if (kn->kev.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_LINK))
        filter_flags |= FILE_NOTIFY_CHANGE_FILE_NAME |
                        FILE_NOTIFY_CHANGE_DIR_NAME;
    if (kn->kev.fflags & NOTE_ATTRIB)
        filter_flags |= FILE_NOTIFY_CHANGE_ATTRIBUTES |
                        FILE_NOTIFY_CHANGE_SECURITY |
                        FILE_NOTIFY_CHANGE_LAST_WRITE;
    if (filter_flags == 0)
        filter_flags = FILE_NOTIFY_CHANGE_LAST_WRITE;

    vs->dir_handle = FindFirstChangeNotificationW(parent, FALSE, filter_flags);
    free(parent);
    parent = NULL;
    if (vs->dir_handle == INVALID_HANDLE_VALUE) {
        dbg_lasterror("FindFirstChangeNotification()");
        goto err;
    }

    kn->kn_handle = vs;

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, vs->dir_handle,
        evfilt_vnode_callback, kn, INFINITE, 0) == 0) {
        dbg_lasterror("RegisterWaitForSingleObject()");
        FindCloseChangeNotification(vs->dir_handle);
        vs->dir_handle = INVALID_HANDLE_VALUE;
        kn->kn_handle = NULL;
        goto err;
    }

    return (0);

err:
    if (vs->full_path) free(vs->full_path);
    if (parent) free(parent);
    free(vs);
    return (-1);
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{
    struct vnode_state *vs = (struct vnode_state *)kn->kn_handle;

    if (vs == NULL)
        return (0);

    if (kn->kn_event_whandle != NULL) {
        if (!UnregisterWaitEx(kn->kn_event_whandle, INVALID_HANDLE_VALUE))
            dbg_lasterror("UnregisterWaitEx()");
        kn->kn_event_whandle = NULL;
    }
    if (vs->dir_handle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(vs->dir_handle);
        vs->dir_handle = INVALID_HANDLE_VALUE;
    }
    if (vs->full_path) free(vs->full_path);
    free(vs);
    kn->kn_handle = NULL;
    return (0);
}

int
evfilt_vnode_knote_modify(struct filter *filt, struct knote *kn,
    const struct kevent *kev)
{
    /*
     * Common code does not pre-merge kev into kn->kev (filter's job).
     * EV_RECEIPT is sticky on BSD; preserve across modify.
     */
    if (evfilt_vnode_knote_delete(filt, kn) < 0)
        return (-1);

    kn->kev.fflags = kev->fflags;
    kn->kev.data   = kev->data;

    return evfilt_vnode_knote_create(filt, kn);
}

int
evfilt_vnode_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_vnode_knote_create(filt, kn);
}

int
evfilt_vnode_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_vnode_knote_delete(filt, kn);
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
