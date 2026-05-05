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

#include <stdbool.h>
#include "common.h"

/* Create an empty file */
static void
testfile_create(const char *path)
{
    int fd;

    if ((fd = open(path, O_CREAT | O_WRONLY, 0600)) < 0)
        die("open");
    close(fd);
}

#ifdef _WIN32
/*
 * Win32 has no /usr/bin/touch; mimic it by re-opening the file
 * with FILE_FLAG_BACKUP_SEMANTICS and setting the last-write time
 * to "now".  ReadDirectoryChangesW / FindFirstChangeNotification
 * fire on FILE_NOTIFY_CHANGE_LAST_WRITE, which our vnode filter
 * maps to NOTE_ATTRIB.
 */
static void
testfile_touch(const char *path)
{
    HANDLE h;
    FILETIME ft;

    h = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        die("CreateFile (touch)");
    /*
     * GetSystemTimePreciseAsFileTime gives 100ns resolution; a
     * pair of close-together touches produces FILETIMEs that
     * actually differ.  GetSystemTime (ms-resolution) collides
     * with the mtime snapshot the vnode filter takes at
     * knote_create time when the two happen in the same ms,
     * which makes the filter's delta-stat miss the change and
     * discard the event.
     */
    GetSystemTimePreciseAsFileTime(&ft);
    if (!SetFileTime(h, NULL, NULL, &ft))
        die("SetFileTime");
    CloseHandle(h);
}

/* echo hi >> path - stay in C land so shell quoting isn't an issue */
static void
testfile_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0)
        die("open (write)");
    if (write(fd, "hi\n", 3) != 3)
        die("write");
    close(fd);
}
#else
static void
testfile_touch(const char *path)
{
    char buf[1024];

#if defined(LIBKQUEUE_BACKEND_POSIX)
    /*
     * Stat-snapshot polling on a coarse-ctime filesystem
     * (overlayfs, ext4 default = 1ms) loses changes that land
     * inside the same tick as the prior stat.  EV_ADD just stat'd
     * the file; sleep past the granularity so touch's ctime
     * lands on a later tick and the diff sees it.
     */
    usleep(2000);
#endif
    snprintf(buf, sizeof(buf), "touch %s", path);
    if (system(buf) != 0)
        die("system");
}

static void
testfile_write(const char *path)
{
    char buf[1024];

    snprintf(buf, sizeof(buf), "echo hi >> %s", path);
    if (system(buf) != 0)
        die("system");
}
#endif

static void
testfile_rename(const char *path, int step)
{
    char buf[1024];

    snprintf(buf, sizeof(buf), "%s.tmp", path);
    /* XXX-FIXME use of 'step' conceals a major memory corruption
            when the file is renamed twice.
            To replicate, remove "if step" conditional so
            two renames occur in this function.
            */
    if (step == 0) {
        if (rename(path, buf) != 0)
            err(1,"rename");
    } else {
        if (rename(buf, path) != 0)
            err(1,"rename");
    }
}

void
test_kevent_vnode_add(struct test_context *ctx)
{
    struct kevent kev;

    testfile_create(ctx->testfile);

#ifdef _WIN32
    /*
     * Neither MSVCRT open() nor _sopen sets FILE_SHARE_DELETE on
     * the underlying CreateFile, which means while this fd is
     * live the file can't be renamed or unlinked - tripping the
     * rename / unlink subtests with EACCES.  Open directly via
     * CreateFile with all three share bits and adopt the HANDLE
     * as a CRT fd via _open_osfhandle.
     */
    {
        HANDLE h = CreateFileA(ctx->testfile, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            err(1, "CreateFile of %s", ctx->testfile);
        ctx->vnode_fd = _open_osfhandle((intptr_t) h, O_RDWR | O_BINARY);
        if (ctx->vnode_fd < 0) {
            CloseHandle(h);
            err(1, "_open_osfhandle of %s", ctx->testfile);
        }
    }
#else
    ctx->vnode_fd = open(ctx->testfile, O_RDWR);
#endif
    if (ctx->vnode_fd < 0)
        err(1, "open of %s", ctx->testfile);

    unsigned int mask = NOTE_ATTRIB | NOTE_DELETE;
#ifdef NOTE_WRITE
    mask |= NOTE_WRITE;
#endif
#ifdef NOTE_RENAME
    mask |= NOTE_RENAME;
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD,
            mask, 0, NULL);
}

void
test_kevent_vnode_note_delete(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_ONESHOT, NOTE_DELETE, 0, NULL);

    if (unlink(ctx->testfile) < 0)
        die("unlink");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /*
     * macOS fires NOTE_LINK alongside NOTE_DELETE on the final unlink.
     * Our POSIX backend matches that behaviour (any nlink change fires
     * NOTE_LINK per BSD spec).  FreeBSD/OpenBSD only fire NOTE_DELETE
     * on the final unlink, so gate accordingly.
     */
#if defined(__APPLE__) || defined(LIBKQUEUE_BACKEND_POSIX)
    kev.fflags |= NOTE_LINK;
#endif

    kevent_cmp(&kev, ret);
}

#ifdef NOTE_WRITE
void
test_kevent_vnode_note_write(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_ONESHOT, NOTE_WRITE, 0, NULL);

    testfile_write(ctx->testfile);

    /*
     * macOS 11.5.2 does not add NOTE_EXTEND,
     * BSD kqueue does, as does libkqueue.
     */
#ifndef __APPLE__
    kev.fflags |= NOTE_EXTEND;
#endif
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
}
#endif

void
test_kevent_vnode_note_attrib(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);

    testfile_touch(ctx->testfile);

    kevent_rv_cmp(1, kevent(ctx->kqfd, NULL, 0, &kev, 1, NULL));
    if (kev.ident != ctx->vnode_fd ||
            kev.filter != EVFILT_VNODE ||
            kev.fflags != NOTE_ATTRIB)
        err(1, "%s - incorrect event (sig=%u; filt=%d; flags=%d)",
                ctx->cur_test_id, (unsigned int)kev.ident, kev.filter, kev.flags);
}

#ifdef NOTE_RENAME
void
test_kevent_vnode_note_rename(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_ONESHOT, NOTE_RENAME, 0, NULL);

    testfile_rename(ctx->testfile, 0);

    kevent_rv_cmp(1, kevent(ctx->kqfd, NULL, 0, &kev, 1, NULL));
    if (kev.ident != ctx->vnode_fd ||
            kev.filter != EVFILT_VNODE ||
            kev.fflags != NOTE_RENAME)
        err(1, "%s - incorrect event (sig=%u; filt=%d; flags=%d)",
                ctx->cur_test_id, (unsigned int)kev.ident, kev.filter, kev.flags);

    testfile_rename(ctx->testfile, 1);

    test_no_kevents(ctx->kqfd);
}
#endif

void
test_kevent_vnode_del(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
}

/*
 * Subscribe to NOTE_EXTEND alone and grow the file by appending a
 * byte; expect a NOTE_EXTEND delivery.  Linux/Solaris synthesise
 * NOTE_EXTEND from a fstat size delta on the underlying file event
 * (IN_MODIFY / FILE_MODIFIED).
 */
static void
test_kevent_vnode_note_extend(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_EXTEND, 0, NULL);

    /* Append: opens for write, writes one byte, closes.  Native
     * BSDs report NOTE_EXTEND for any size growth. */
    int wfd = open(ctx->testfile, O_WRONLY | O_APPEND);
    if (wfd < 0) die("open(O_APPEND)");
    if (write(wfd, "x", 1) != 1) die("write");
    if (close(wfd) < 0) die("close(O_APPEND)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t) ctx->vnode_fd ||
        ret[0].filter != EVFILT_VNODE ||
        !(ret[0].fflags & NOTE_EXTEND))
        die("NOTE_EXTEND not delivered: %s", kevent_to_str(&ret[0]));
}

/*
 * Subscribe to NOTE_LINK alone and add then remove a hardlink.  Both
 * st_nlink-changing operations should fire NOTE_LINK; backends that
 * piggy-back on FILE_ATTRIB / IN_ATTRIB synthesise this from a delta.
 *
 * Win32 uses CreateHardLinkA / DeleteFileA in place of POSIX
 * link/unlink.
 */
#ifdef _WIN32
#  define _link(src, dst)   (CreateHardLinkA((dst), (src), NULL) ? 0 : -1)
#  define _unlink(p)        (DeleteFileA(p) ? 0 : -1)
#else
#  define _link(src, dst)   link((src), (dst))
#  define _unlink(p)        unlink(p)
#endif
static void
test_kevent_vnode_note_link(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          link_path[1024];

    snprintf(link_path, sizeof(link_path), "%s.link", ctx->testfile);
    (void) _unlink(link_path);  /* tolerate leftover from a prior run */

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_LINK, 0, NULL);

    if (_link(ctx->testfile, link_path) < 0)
        die("link(%s, %s)", ctx->testfile, link_path);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t) ctx->vnode_fd ||
        ret[0].filter != EVFILT_VNODE ||
        !(ret[0].fflags & NOTE_LINK))
        die("NOTE_LINK not delivered on link: %s",
            kevent_to_str(&ret[0]));

    if (_unlink(link_path) < 0)
        die("unlink(%s)", link_path);

    /* Backends auto-disarm on delivery: re-add (the modify path
     * re-arms with the original fflags). */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_LINK, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_LINK))
        die("NOTE_LINK not delivered on unlink: %s",
            kevent_to_str(&ret[0]));

    /* Tidy: drop the watch. */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_DELETE, 0, 0, NULL);
}

/*
 * Subscribe to NOTE_TRUNCATE alone and shrink the file via ftruncate;
 * expect a NOTE_TRUNCATE delivery.  NOTE_TRUNCATE is an OpenBSD
 * extension to BSD kqueue (sys/event.h:107, fires only when
 * vap->va_size < oldsize in ufs_setattr); illumos has a dedicated
 * FILE_TRUNC kernel event so this is a direct mapping; Linux's vnode
 * synthesises from st_size shrinkage.  FreeBSD/macOS native kqueue
 * don't define NOTE_TRUNCATE - the gate keeps this test buildable
 * there.
 */
/*
 * Subscribe to NOTE_EXTEND alone and grow the file via ftruncate
 * (no write path involved).  FreeBSD's vop_setattr_post fires
 * NOTE_EXTEND when va_size > oldsize; libkqueue must do the same
 * for non-write growth.
 */
#if defined(NOTE_EXTEND) && !defined(_WIN32)
/* Win32 has _chsize, not ftruncate; rely on the POSIX-only path. */
static void
test_kevent_vnode_note_extend_ftruncate(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct stat   st;

    if (fstat(ctx->vnode_fd, &st) < 0)
        die("fstat");

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_EXTEND, 0, NULL);

    if (ftruncate(ctx->vnode_fd, st.st_size + 4096) < 0)
        die("ftruncate(grow)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_EXTEND))
        die("NOTE_EXTEND not delivered on ftruncate-up: %s",
            kevent_to_str(&ret[0]));
}
#endif

/*
 * NOTE_ATTRIB via fchmod: mode-bit change advances ctime without
 * touching size or mtime.  Distinct kernel path from utimes.
 */
#ifndef _WIN32  /* fchmod is POSIX-only */
static void
test_kevent_vnode_note_attrib_chmod(struct test_context *ctx)
{
    struct kevent kev, ret[1];

#if defined(LIBKQUEUE_BACKEND_POSIX)
    /* See testfile_touch: pad past coarse ctime tick. */
    usleep(2000);
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);

    if (fchmod(ctx->vnode_fd, 0644) < 0)
        die("fchmod");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("NOTE_ATTRIB not delivered on fchmod: %s",
            kevent_to_str(&ret[0]));
}

#endif /* !_WIN32 (fchmod) */

/*
 * NOTE_ATTRIB via fchown: chown to the file's current owner still
 * bumps ctime (kernel doesn't no-op same-uid chown on most FSes).
 * Avoids needing root.
 */
#ifndef _WIN32  /* fchown is POSIX-only */
static void
test_kevent_vnode_note_attrib_chown(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct stat   st;

    if (fstat(ctx->vnode_fd, &st) < 0)
        die("fstat");

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);

    if (fchown(ctx->vnode_fd, st.st_uid, st.st_gid) < 0)
        die("fchown");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("NOTE_ATTRIB not delivered on fchown: %s",
            kevent_to_str(&ret[0]));
}

#endif /* !_WIN32 (fchown) */

/*
 * rename(A, B) when both exist: B's vnode loses its name to A and
 * the kernel fires NOTE_DELETE on B's watch (FreeBSD vop_rename_post
 * a_tvp branch).  Plain unlink is already covered; this exercises
 * the rename-over kernel path.
 */
static void
test_kevent_vnode_note_delete_rename_over(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          src_path[1024];
    int           target_fd;

    snprintf(src_path, sizeof(src_path), "%s.src", ctx->testfile);
    (void) unlink(src_path);
    testfile_create(src_path);

    /* Fresh target file we can watch then have renamed over. */
    char tgt_path[1024];
    snprintf(tgt_path, sizeof(tgt_path), "%s.tgt", ctx->testfile);
    (void) unlink(tgt_path);
    testfile_create(tgt_path);
    target_fd = open(tgt_path, O_RDWR);
    if (target_fd < 0)
        die("open(tgt)");

    kevent_add(ctx->kqfd, &kev, target_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_DELETE, 0, NULL);

    if (rename(src_path, tgt_path) < 0)
        die("rename");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_DELETE))
        die("NOTE_DELETE not delivered on rename-over: %s",
            kevent_to_str(&ret[0]));

    close(target_fd);
    (void) unlink(tgt_path);
}

/*
 * rename-overwrite delivery ordering: watch BOTH the source A and
 * the target B; rename(A,B) clobbers B.  OpenBSD ufs_rename
 * (ufs_vnops.c:993-1052) fires NOTE_DELETE on the clobbered tvp
 * BEFORE NOTE_RENAME on the source fvp.  Verify the ordering
 * invariant.
 */
#ifdef NOTE_RENAME
static void
test_kevent_vnode_rename_overwrite_ordering(struct test_context *ctx)
{
    struct kevent kev, ret[2];
    char          src_path[1024];
    char          tgt_path[1024];
    int           src_fd, tgt_fd;
    int           rv;
    bool          found_delete = false, found_rename = false;

    snprintf(src_path, sizeof(src_path), "%s.ros_src", ctx->testfile);
    snprintf(tgt_path, sizeof(tgt_path), "%s.ros_tgt", ctx->testfile);
    (void) unlink(src_path);
    (void) unlink(tgt_path);
    testfile_create(src_path);
    testfile_create(tgt_path);

    src_fd = open(src_path, O_RDWR);
    tgt_fd = open(tgt_path, O_RDWR);
    if (src_fd < 0 || tgt_fd < 0) die("open");

    kevent_add(ctx->kqfd, &kev, src_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_RENAME, 0, NULL);
    kevent_add(ctx->kqfd, &kev, tgt_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_DELETE, 0, NULL);

    if (rename(src_path, tgt_path) < 0) die("rename");

    /*
     * Drain up to 2 events.  Use kevent() directly because the
     * kevent_get_timeout helper returns a 0/1 readiness signal, not
     * a count.
     */
    {
        struct timespec wait = { 1, 0 };
        rv = kevent(ctx->kqfd, NULL, 0, ret, 2, &wait);
        if (rv < 0) die("kevent");
        if (rv < 2) {
            /* Second drain in case the events came in two batches. */
            struct timespec poll = { 0, 100 * 1000 * 1000 };
            int more = kevent(ctx->kqfd, NULL, 0, &ret[rv], 2 - rv, &poll);
            if (more > 0) rv += more;
        }
    }
    if (rv < 2)
        die("expected NOTE_DELETE and NOTE_RENAME, got %d events", rv);

    for (int i = 0; i < 2; i++) {
        if (ret[i].ident == (uintptr_t) tgt_fd && (ret[i].fflags & NOTE_DELETE))
            found_delete = true;
        else if (ret[i].ident == (uintptr_t) src_fd && (ret[i].fflags & NOTE_RENAME))
            found_rename = true;
    }
    if (!found_delete)
        die("rename-overwrite: missing NOTE_DELETE on tgt_fd");
    if (!found_rename)
        die("rename-overwrite: missing NOTE_RENAME on src_fd");

    /*
     * BSD orders NOTE_DELETE on tvp before NOTE_RENAME on fvp; with
     * a single-threaded drain we expect the array index reflects
     * that.  Linux/POSIX backends may reorder due to dispatch order
     * differences - so allow either order but require both bits to
     * be present.
     */

    close(src_fd);
    close(tgt_fd);
    (void) unlink(tgt_path);
}
#endif

/*
 * Watch a directory fd; create then unlink a child inside.
 * BSD fires NOTE_WRITE on the parent dir's vnode for any child
 * namespace mutation (vop_create_post / vop_remove_post).
 */
#if defined(NOTE_WRITE) && !defined(_WIN32)
/* Win32 CRT _open() can't return a fd for a directory (no
 * FILE_FLAG_BACKUP_SEMANTICS); the test relies on POSIX-style
 * open(O_DIRECTORY). */
static void
test_kevent_vnode_note_write_directory(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          dir_path[1024];
    char          child_path[1024];
    int           dir_fd;

    snprintf(dir_path, sizeof(dir_path), "%s.dir", ctx->testfile);
    snprintf(child_path, sizeof(child_path), "%s/child", dir_path);
    (void) unlink(child_path);
    (void) rmdir(dir_path);
    if (mkdir(dir_path, 0700) < 0)
        die("mkdir(dir)");

    dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0)
        die("open(dir)");

    kevent_add(ctx->kqfd, &kev, dir_fd, EVFILT_VNODE,
               EV_ADD, NOTE_WRITE, 0, NULL);

    testfile_create(child_path);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_WRITE))
        die("NOTE_WRITE not delivered on child create: %s",
            kevent_to_str(&ret[0]));

    if (unlink(child_path) < 0)
        die("unlink(child)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_WRITE))
        die("NOTE_WRITE not delivered on child unlink: %s",
            kevent_to_str(&ret[0]));

    kevent_add(ctx->kqfd, &kev, dir_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    close(dir_fd);
    (void) rmdir(dir_path);
}
#endif

/*
 * Watch a directory; create a subdirectory inside.  The new subdir's
 * ".." link bumps the parent's st_nlink, which BSD reports as
 * NOTE_LINK on the parent.
 */
#if defined(NOTE_LINK) && !defined(_WIN32)
/* See test_kevent_vnode_note_write_directory for the O_DIRECTORY note. */
static void
test_kevent_vnode_note_link_directory(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          dir_path[1024];
    char          sub_path[1024];
    int           dir_fd;

    snprintf(dir_path, sizeof(dir_path), "%s.linkdir", ctx->testfile);
    snprintf(sub_path, sizeof(sub_path), "%s/sub", dir_path);
    (void) rmdir(sub_path);
    (void) rmdir(dir_path);
    if (mkdir(dir_path, 0700) < 0)
        die("mkdir(dir)");

    dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0)
        die("open(dir)");

    kevent_add(ctx->kqfd, &kev, dir_fd, EVFILT_VNODE,
               EV_ADD, NOTE_LINK, 0, NULL);

    if (mkdir(sub_path, 0700) < 0)
        die("mkdir(sub)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_LINK))
        die("NOTE_LINK not delivered on subdir create: %s",
            kevent_to_str(&ret[0]));

    if (rmdir(sub_path) < 0)
        die("rmdir(sub)");

    kevent_add(ctx->kqfd, &kev, dir_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    close(dir_fd);
    (void) rmdir(dir_path);
}
#endif

/*
 * In-place overwrite at the same size: pwrite N bytes at offset 0
 * over an existing N-byte file.  BSD fires NOTE_WRITE because data
 * changed.  POSIX stat-polling can't distinguish this from utimes,
 * so the public header redacts NOTE_WRITE on that backend and the
 * test compiles out.
 */
#if defined(NOTE_WRITE) && !defined(_WIN32)
/* pwrite is POSIX-only; Win32 has _write + _lseek but no atomic
 * combined seek-and-write equivalent on a CRT fd. */
static void
test_kevent_vnode_note_write_inplace(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          buf[16];

    /* Seed a known-size payload. */
    if (pwrite(ctx->vnode_fd, "AAAAAAAAAAAAAAAA", 16, 0) != 16)
        die("pwrite(seed)");

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_WRITE, 0, NULL);

    /* Overwrite the same range, no size change. */
    memset(buf, 'B', sizeof(buf));
    if (pwrite(ctx->vnode_fd, buf, sizeof(buf), 0) != (ssize_t) sizeof(buf))
        die("pwrite(overwrite)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_WRITE))
        die("NOTE_WRITE not delivered on same-size overwrite: %s",
            kevent_to_str(&ret[0]));
}
#endif

#ifdef NOTE_TRUNCATE
#ifdef _WIN32
#  define _ftruncate(fd, n) (_chsize_s((fd), (__int64)(n)) == 0 ? 0 : -1)
#else
#  define _ftruncate(fd, n) ftruncate((fd), (n))
#endif
static void
test_kevent_vnode_note_truncate(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* Make sure there's something to truncate. */
    if (_ftruncate(ctx->vnode_fd, 4096) < 0)
        die("ftruncate(grow)");

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_TRUNCATE, 0, NULL);

    if (_ftruncate(ctx->vnode_fd, 0) < 0)
        die("ftruncate(shrink)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t) ctx->vnode_fd ||
        ret[0].filter != EVFILT_VNODE ||
        !(ret[0].fflags & NOTE_TRUNCATE))
        die("NOTE_TRUNCATE not delivered: %s",
            kevent_to_str(&ret[0]));
}
#endif

void
test_kevent_vnode_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev;

    test_no_kevents(ctx->kqfd);

    /* Add the watch and immediately disable it */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);
    kev.flags = EV_DISABLE;
    kevent_update(ctx->kqfd, &kev);

    /* Confirm that the watch is disabled */
    testfile_touch(ctx->testfile);
    test_no_kevents(ctx->kqfd);

    /* Re-enable and check again */
    kev.flags = EV_ENABLE;
    kevent_update(ctx->kqfd, &kev);
    testfile_touch(ctx->testfile);
    kevent_rv_cmp(1, kevent(ctx->kqfd, NULL, 0, &kev, 1, NULL));
    if (kev.ident != ctx->vnode_fd ||
            kev.filter != EVFILT_VNODE ||
            kev.fflags != NOTE_ATTRIB)
        err(1, "%s - incorrect event (sig=%u; filt=%d; flags=%d)",
                ctx->cur_test_id, (unsigned int)kev.ident, kev.filter, kev.flags);
}

#ifdef EV_DISPATCH
void
test_kevent_vnode_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD | EV_DISPATCH, NOTE_ATTRIB, 0, NULL);

    testfile_touch(ctx->testfile);

    kevent_rv_cmp(1, kevent(ctx->kqfd, NULL, 0, &kev, 1, NULL));
    if (kev.ident != ctx->vnode_fd ||
            kev.filter != EVFILT_VNODE ||
            kev.fflags != NOTE_ATTRIB)
        err(1, "%s - incorrect event (sig=%u; filt=%d; flags=%d)",
                ctx->cur_test_id, (unsigned int)kev.ident, kev.filter, kev.flags);

    /* Confirm that the watch is disabled automatically */
    testfile_touch(ctx->testfile);
    test_no_kevents(ctx->kqfd);

    /* Re-enable the kevent */
    /* FIXME- is EV_DISPATCH needed when rearming ? */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ENABLE | EV_DISPATCH, 0, 0, NULL);
    kev.flags = EV_ADD | EV_DISPATCH;   /* FIXME: may not be portable */
    kev.fflags = NOTE_ATTRIB;
    testfile_touch(ctx->testfile);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* Delete the watch */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, NOTE_ATTRIB, 0, NULL);
}
#endif     /* EV_DISPATCH */

/*
 * Multiple changes between drains coalesce into one event whose
 * fflags is the union of all triggered NOTE_* bits.  Touch (ATTRIB
 * via vop_setattr_post) + ftruncate-up (EXTEND via vop_setattr_post
 * with va_size > oldsize) between two drains: BSD ORs fflags from
 * each VOP into the pending set, delivers one event with both bits.
 */
static void
test_kevent_vnode_fflag_accumulation(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct stat   st;

    if (fstat(ctx->vnode_fd, &st) < 0)
        die("fstat");

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB | NOTE_EXTEND, 0, NULL);

    testfile_touch(ctx->testfile);
    if (ftruncate(ctx->vnode_fd, st.st_size + 4096) < 0)
        die("ftruncate(grow)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_EXTEND))
        die("expected NOTE_EXTEND in unioned fflags: %s",
            kevent_to_str(&ret[0]));
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("expected NOTE_ATTRIB in unioned fflags: %s",
            kevent_to_str(&ret[0]));
}

/*
 * EV_CLEAR resets the accumulator after delivery: the second drain
 * with no further change must return nothing, then a fresh change
 * fires again.
 */
static void
test_kevent_vnode_ev_clear(struct test_context *ctx)
{
    struct kevent kev, ret[1];

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_CLEAR, NOTE_ATTRIB, 0, NULL);

    testfile_touch(ctx->testfile);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("first delivery missing NOTE_ATTRIB: %s",
            kevent_to_str(&ret[0]));

    /* No further change: EV_CLEAR must leave the accumulator empty. */
    test_no_kevents(ctx->kqfd);

    /* Fresh change re-arms. */
    testfile_touch(ctx->testfile);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("second delivery missing NOTE_ATTRIB after EV_CLEAR re-arm: %s",
            kevent_to_str(&ret[0]));

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
}

/*
 * Closing the watched fd while the knote is armed: native BSD's
 * fdrop hook auto-removes the knote.  POSIX backend's next fstat
 * returns EBADF; if NOTE_DELETE is in the mask we treat that as the
 * file going away and deliver one final NOTE_DELETE.
 */
static void
test_kevent_vnode_fd_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          path[1024];
    int           fd;

    snprintf(path, sizeof(path), "%s.fdclose", ctx->testfile);
    (void) unlink(path);
    testfile_create(path);
    fd = open(path, O_RDWR);
    if (fd < 0) die("open(fdclose)");

    kevent_add(ctx->kqfd, &kev, fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_DELETE, 0, NULL);

    if (close(fd) < 0)
        die("close(watched_fd)");

#if defined(LIBKQUEUE_BACKEND_POSIX)
    /*
     * POSIX backend polls; give the dispatch loop one cycle to
     * fstat the now-EBADF fd and synthesise NOTE_DELETE.
     */
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    int rv = kevent(ctx->kqfd, NULL, 0, ret, 1, &ts);
    if (rv < 0)
        die("kevent(fd_close)");
    if (rv == 1 && !(ret[0].fflags & NOTE_DELETE))
        die("fd close: unexpected fflags: %s", kevent_to_str(&ret[0]));
#else
    /*
     * Native BSD / Linux inotify: knote is auto-removed when the
     * file goes out of scope.  Either no event or NOTE_DELETE is
     * acceptable; what we don't tolerate is a crash.
     */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
    (void) kevent(ctx->kqfd, NULL, 0, ret, 1, &ts);
#endif
    (void) unlink(path);

    /*
     * The unlink may itself trigger NOTE_DELETE on backends that
     * watch the parent directory (Win32 FindFirstChangeNotificationW,
     * Linux inotify on a watched-fd's path).  Drain any pending
     * completion in this test's scope so it doesn't leak into the
     * next test's first kevent.
     */
    {
        struct timespec poll = { 0, 100 * 1000 * 1000 };
        (void) kevent(ctx->kqfd, NULL, 0, ret, 1, &poll);
    }
}

/*
 * Two kqueues watching the same fd: a single change must fire on
 * both and draining one must not affect the other.
 */
static void
test_kevent_vnode_multi_kqueue(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           kq2;

    kq2 = kqueue();
    if (kq2 < 0)
        die("kqueue(2)");

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);
    kevent_add(kq2, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    testfile_touch(ctx->testfile);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("kq1 missed NOTE_ATTRIB: %s", kevent_to_str(&ret[0]));

    kevent_get(ret, NUM_ELEMENTS(ret), kq2, 1);
    if (!(ret[0].fflags & NOTE_ATTRIB))
        die("kq2 missed NOTE_ATTRIB: %s", kevent_to_str(&ret[0]));

    close(kq2);
}

/*
 * EVFILT_VNODE on a pipe/socket fd: not a file, registration must
 * fail.  Native BSD checks fp->f_type == DTYPE_VNODE and returns
 * EINVAL; libkqueue should match.
 */
static void
test_kevent_vnode_non_file_rejected(struct test_context *ctx)
{
    struct kevent kev;
    int           pipefd[2];
    int           rv;

    (void) ctx;
    if (pipe(pipefd) < 0)
        die("pipe");

    EV_SET(&kev, pipefd[0], EVFILT_VNODE, EV_ADD,
           NOTE_ATTRIB | NOTE_DELETE, 0, NULL);
    rv = kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
    if (rv == 0)
        die("EVFILT_VNODE on pipe accepted; expected EINVAL");
    if (errno != EINVAL)
        die("EVFILT_VNODE on pipe failed with errno=%d (%s); expected EINVAL",
            errno, strerror(errno));

    close(pipefd[0]);
    close(pipefd[1]);
}

/*
 * Flag-behaviour tests.
 */

/*
 * BSD overwrites kn->kev.udata on every modify.
 */
static void
test_kevent_vnode_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           marker = 0xab;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, &marker);
    /* Re-EV_ADD modify with udata=NULL. */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, NULL);

    testfile_touch(ctx->testfile);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);
}

/*
 * modify_disarms: re-EV_ADD with fflags switched away from the
 * triggering note must tear down the kernel watch so the original
 * file event no longer fires.
 */
static void
test_kevent_vnode_modify_disarms(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_ATTRIB, 0, NULL);
    /* Switch to NOTE_DELETE - we'll only touch, not delete. */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_DELETE, 0, NULL);

    testfile_touch(ctx->testfile);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
}

/*
 * disable_drains: kernel queues a port_event_t for the file change;
 * EV_DISABLE must drop it before the drain.
 */
static void
test_kevent_vnode_disable_drains(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_ATTRIB, 0, NULL);

    /* Trigger: touch returns when kernel has noted the change. */
    testfile_touch(ctx->testfile);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_DISABLE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
}

/*
 * delete_drains: same as disable_drains but with EV_DELETE; also
 * exercises the "queued event for a freed knote" UAF class.
 */
static void
test_kevent_vnode_delete_drains(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_ATTRIB, 0, NULL);

    testfile_touch(ctx->testfile);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);
}

/*
 * EV_DELETE on a never-registered fd returns ENOENT.
 */
static void
test_kevent_vnode_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;
    char          path[1024];
    int           fd;

    snprintf(path, sizeof(path), "%s.del_ne", ctx->testfile);
    (void) unlink(path);
    testfile_create(path);
    fd = open(path, O_RDWR);
    if (fd < 0) die("open");

    EV_SET(&kev, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added knote should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));

    close(fd);
    (void) unlink(path);
}

/*
 * udata round-trips through delivery.
 */
static void
test_kevent_vnode_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    void         *marker = (void *) 0x55AA55AAUL;

#if defined(LIBKQUEUE_BACKEND_POSIX)
    usleep(2000);
#endif
    EV_SET(&kev, ctx->vnode_fd, EVFILT_VNODE,
           EV_ADD | EV_ONESHOT, NOTE_ATTRIB, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    testfile_touch(ctx->testfile);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p expected %p", ret[0].udata, marker);
}

/*
 * EV_RECEIPT echoes the kev with EV_ERROR=0.
 */
#ifdef EV_RECEIPT
static void
test_kevent_vnode_receipt_preserved(struct test_context *ctx)
{
    struct kevent kev[1];

    EV_SET(&kev[0], ctx->vnode_fd, EVFILT_VNODE,
           EV_ADD | EV_RECEIPT, NOTE_ATTRIB, 0, NULL);
    if (kevent(ctx->kqfd, kev, 1, kev, 1, NULL) != 1)
        die("EV_RECEIPT should return one echo entry");
    if (!(kev[0].flags & EV_ERROR) || kev[0].data != 0)
        die("EV_RECEIPT echo missing EV_ERROR=0 marker");

    EV_SET(&kev[0], ctx->vnode_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, kev, 1, NULL, 0, NULL);
}
#endif

/*
 * note_extend_ftruncate: VOP_SETATTR doesn't fire NOTE_EXTEND on size grow
 * on native BSDs (only the write path does).  libkqueue's stat-poll backends
 * handle this correctly, so skip on native kqueue and Windows.
 */
static const struct lkq_test_gate vnode_gate_extend_ftruncate[] =
{
    GATE(LKQ_PLATFORM_BACKEND_NATIVE, "VOP_SETATTR doesn't fire NOTE_EXTEND on ftruncate-up on native BSD kqueue"),
    GATE(LKQ_PLATFORM_OS_WINDOWS,     "ftruncate not available on Windows"),
    { 0, NULL }
};

/*
 * note_link (file): FindFirstChangeNotification doesn't reliably fire on
 * CreateHardLink; skip on Windows until tracked issue is resolved.
 */
static const struct lkq_test_gate vnode_gate_note_link[] =
{
    GATE(LKQ_PLATFORM_OS_WINDOWS, "CreateHardLink doesn't reliably fire NOTE_LINK via FindFirstChangeNotification"),
    { 0, NULL }
};

/*
 * fflag_accumulation: stat-snapshot polling only sees the end state after
 * both touch and ftruncate-up; only the Linux inotify backend drain-and-unions
 * both NOTE_ATTRIB and NOTE_EXTEND.  Also fails on native BSDs for the same
 * VOP_SETATTR/NOTE_EXTEND reason as note_extend_ftruncate.
 */
static const struct lkq_test_gate vnode_gate_fflag_accumulation[] =
{
    GATE(LKQ_PLATFORM_NOT_BACKEND_LINUX, "stat-snapshot and native-BSD backends don't accumulate NOTE_ATTRIB + NOTE_EXTEND across touch + ftruncate-up"),
    { 0, NULL }
};

/*
 * rename_overwrite_ordering: NtSetInformationFile(FileRenameInformation,
 * Replace=TRUE) hits STATUS_ACCESS_DENIED on this open-watched-target shape.
 * Tracked in https://github.com/mheily/libkqueue/issues/172
 */
static const struct lkq_test_gate vnode_gate_rename_overwrite[] =
{
    GATE(LKQ_PLATFORM_OS_WINDOWS, "NtSetInformationFile rename-over hits STATUS_ACCESS_DENIED on Windows (issue #172)"),
    { 0, NULL }
};

/*
 * Tests that require POSIX-only syscalls (fchmod, fchown, pwrite,
 * O_DIRECTORY open of a directory, rename over open target).
 */
static const struct lkq_test_gate vnode_gate_posix_only[] =
{
    GATE(LKQ_PLATFORM_OS_WINDOWS, "POSIX-only syscall not available on Windows"),
    { 0, NULL }
};

const struct lkq_test_case lkq_vnode_tests[] =
{
    {
        .name  = "kevent_vnode_add",
        .desc  = "Add EVFILT_VNODE watch",
        .func  = test_kevent_vnode_add,
    },
    {
        .name  = "kevent_vnode_del",
        .desc  = "Remove EVFILT_VNODE watch",
        .func  = test_kevent_vnode_del,
    },
    {
        .name  = "kevent_vnode_del_nonexistent",
        .desc  = "EV_DELETE on never-registered fd returns ENOENT",
        .func  = test_kevent_vnode_del_nonexistent,
    },
    {
        .name  = "kevent_vnode_udata_preserved",
        .desc  = "udata round-trips through delivery",
        .func  = test_kevent_vnode_udata_preserved,
    },
#ifdef EV_RECEIPT
    {
        .name  = "kevent_vnode_receipt_preserved",
        .desc  = "EV_RECEIPT echoes kev with EV_ERROR=0",
        .func  = test_kevent_vnode_receipt_preserved,
    },
#endif
    {
        .name  = "kevent_vnode_disable_and_enable",
        .desc  = "EV_DISABLE suppresses events; EV_ENABLE restores delivery",
        .func  = test_kevent_vnode_disable_and_enable,
    },
#ifdef EV_DISPATCH
    {
        .name  = "kevent_vnode_dispatch",
        .desc  = "EV_DISPATCH auto-disables after first delivery",
        .func  = test_kevent_vnode_dispatch,
    },
#endif
#ifdef NOTE_WRITE
    {
        .name  = "kevent_vnode_note_write",
        .desc  = "NOTE_WRITE fires on file data append",
        .func  = test_kevent_vnode_note_write,
    },
#endif
    {
        .name  = "kevent_vnode_note_attrib",
        .desc  = "NOTE_ATTRIB fires on utimes",
        .func  = test_kevent_vnode_note_attrib,
    },
#ifdef NOTE_RENAME
    {
        .name  = "kevent_vnode_note_rename",
        .desc  = "NOTE_RENAME fires on file rename",
        .func  = test_kevent_vnode_note_rename,
    },
#endif
    {
        .name  = "kevent_vnode_note_attrib_chmod",
        .desc  = "NOTE_ATTRIB fires on fchmod",
        .func  = test_kevent_vnode_note_attrib_chmod,
        .gates = vnode_gate_posix_only,
    },
    {
        .name  = "kevent_vnode_note_attrib_chown",
        .desc  = "NOTE_ATTRIB fires on fchown",
        .func  = test_kevent_vnode_note_attrib_chown,
        .gates = vnode_gate_posix_only,
    },
#ifdef NOTE_EXTEND
    {
        .name  = "kevent_vnode_note_extend",
        .desc  = "NOTE_EXTEND fires on file size growth via write",
        .func  = test_kevent_vnode_note_extend,
    },
    {
        .name  = "kevent_vnode_note_extend_ftruncate",
        .desc  = "NOTE_EXTEND fires on ftruncate-up (size grow without write)",
        .func  = test_kevent_vnode_note_extend_ftruncate,
        .gates = vnode_gate_extend_ftruncate,
    },
#endif
#ifdef NOTE_LINK
    {
        .name  = "kevent_vnode_note_link",
        .desc  = "NOTE_LINK fires on hardlink create and remove",
        .func  = test_kevent_vnode_note_link,
        .gates = vnode_gate_note_link,
    },
#endif
    {
        .name  = "kevent_vnode_fflag_accumulation",
        .desc  = "Multiple NOTE_* bits coalesce into a single event",
        .func  = test_kevent_vnode_fflag_accumulation,
        .gates = vnode_gate_fflag_accumulation,
    },
#ifdef NOTE_RENAME
    {
        .name  = "kevent_vnode_rename_overwrite_ordering",
        .desc  = "rename(A,B) fires NOTE_DELETE on B then NOTE_RENAME on A",
        .func  = test_kevent_vnode_rename_overwrite_ordering,
        .gates = vnode_gate_rename_overwrite,
    },
#endif
#ifdef NOTE_LINK
    {
        .name  = "kevent_vnode_note_link_directory",
        .desc  = "NOTE_LINK fires on parent dir when a subdirectory is created",
        .func  = test_kevent_vnode_note_link_directory,
        .gates = vnode_gate_posix_only,
    },
#endif
#ifdef NOTE_TRUNCATE
    {
        .name  = "kevent_vnode_note_truncate",
        .desc  = "NOTE_TRUNCATE fires on ftruncate shrink",
        .func  = test_kevent_vnode_note_truncate,
    },
#endif
    {
        .name  = "kevent_vnode_note_delete_rename_over",
        .desc  = "NOTE_DELETE fires on target vnode when renamed over",
        .func  = test_kevent_vnode_note_delete_rename_over,
        .gates = vnode_gate_posix_only,
    },
#if defined(NOTE_WRITE) && !defined(_WIN32)
    {
        .name  = "kevent_vnode_note_write_directory",
        .desc  = "NOTE_WRITE fires on parent dir for child namespace mutations",
        .func  = test_kevent_vnode_note_write_directory,
        .gates = vnode_gate_posix_only,
    },
    {
        .name  = "kevent_vnode_note_write_inplace",
        .desc  = "NOTE_WRITE fires on same-size pwrite overwrite",
        .func  = test_kevent_vnode_note_write_inplace,
        .gates = vnode_gate_posix_only,
    },
#endif
    {
        .name  = "kevent_vnode_ev_clear",
        .desc  = "EV_CLEAR resets the fflags accumulator after delivery",
        .func  = test_kevent_vnode_ev_clear,
    },
    {
        .name  = "kevent_vnode_fd_close",
        .desc  = "Closing watched fd while knote is armed doesn't crash",
        .func  = test_kevent_vnode_fd_close,
    },
    {
        .name  = "kevent_vnode_multi_kqueue",
        .desc  = "Two kqueues watching the same fd both receive the event",
        .func  = test_kevent_vnode_multi_kqueue,
    },
    {
        .name  = "kevent_vnode_non_file_rejected",
        .desc  = "EVFILT_VNODE on a pipe fd is rejected with EINVAL",
        .func  = test_kevent_vnode_non_file_rejected,
    },
    {
        .name  = "kevent_vnode_modify_clobbers_udata",
        .desc  = "Re-EV_ADD with new udata overwrites the previous udata",
        .func  = test_kevent_vnode_modify_clobbers_udata,
    },
    {
        .name  = "kevent_vnode_modify_disarms",
        .desc  = "Re-EV_ADD with different fflags tears down the old watch",
        .func  = test_kevent_vnode_modify_disarms,
    },
    {
        .name  = "kevent_vnode_disable_drains",
        .desc  = "EV_DISABLE drops a queued but undelivered event",
        .func  = test_kevent_vnode_disable_drains,
    },
    {
        .name  = "kevent_vnode_delete_drains",
        .desc  = "EV_DELETE drops a queued but undelivered event",
        .func  = test_kevent_vnode_delete_drains,
    },
    /*
     * note_delete must run last: it unlinks ctx->testfile and the
     * Linux backend resolves the watched path via /proc/self/fd/N,
     * which after unlink reports "<path> (deleted)" causing
     * inotify_add_watch to fail for any subsequent tests.
     */
    {
        .name  = "kevent_vnode_note_delete",
        .desc  = "NOTE_DELETE fires on unlink",
        .func  = test_kevent_vnode_note_delete,
    },
    LKQ_SUITE_END
};

void
test_evfilt_vnode(struct test_context *ctx)
{
#if (defined(__sun) && !defined(HAVE_PORT_SOURCE_FILE))
    puts("**NOTE** EVFILT_VNODE is not supported on this version of Solaris");
    return;
#endif

    snprintf(ctx->testfile, sizeof(ctx->testfile), "%s/kqueue-test%d.tmp",
            test_tmpdir(), testing_make_uid());

    run_test_suite(ctx, lkq_vnode_tests);

    /* TODO: test r590 corner case where a descriptor is closed and
             the associated knote is automatically freed. */
    unlink(ctx->testfile);
    close(ctx->vnode_fd);
    ctx->vnode_fd = -1;
}
