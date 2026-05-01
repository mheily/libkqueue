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

static void
testfile_touch(const char *path)
{
    char buf[1024];

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

    ctx->vnode_fd = open(ctx->testfile, O_RDWR);
    if (ctx->vnode_fd < 0)
        err(1, "open of %s", ctx->testfile);

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE, EV_ADD,
            NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_DELETE, 0, NULL);
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
     *  FIXME - macOS 11.5.2 also sets NOTE_LINK.
     *  This seems redundant, but behaviour should be
     *  checked on FreeBSD/OpenBSD to determine the
     *  correct behaviour for libkqueue.
     */
#ifdef __APPLE__
    kev.fflags |= NOTE_LINK;
#endif

    kevent_cmp(&kev, ret);
}

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
 */
static void
test_kevent_vnode_note_link(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    char          link_path[1024];

    snprintf(link_path, sizeof(link_path), "%s.link", ctx->testfile);
    (void) unlink(link_path);  /* tolerate leftover from a prior run */

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD, NOTE_LINK, 0, NULL);

    if (link(ctx->testfile, link_path) < 0)
        die("link(%s, %s)", ctx->testfile, link_path);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t) ctx->vnode_fd ||
        ret[0].filter != EVFILT_VNODE ||
        !(ret[0].fflags & NOTE_LINK))
        die("NOTE_LINK not delivered on link: %s",
            kevent_to_str(&ret[0]));

    if (unlink(link_path) < 0)
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
 * expect a NOTE_TRUNCATE delivery.  illumos has a dedicated FILE_TRUNC
 * kernel event so this is a direct mapping; Linux's vnode synthesis
 * only catches truncate-to-zero (a known limitation).
 */
static void
test_kevent_vnode_note_truncate(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* Make sure there's something to truncate. */
    if (ftruncate(ctx->vnode_fd, 4096) < 0)
        die("ftruncate(grow)");

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_TRUNCATE, 0, NULL);

    if (ftruncate(ctx->vnode_fd, 0) < 0)
        die("ftruncate(shrink)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t) ctx->vnode_fd ||
        ret[0].filter != EVFILT_VNODE ||
        !(ret[0].fflags & NOTE_TRUNCATE))
        die("NOTE_TRUNCATE not delivered: %s",
            kevent_to_str(&ret[0]));
}

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
 * BSD overwrites kn->kev.udata on every modify.  Solaris-only because
 * Linux's evfilt_vnode_knote_modify is a stub returning -1; macOS uses
 * native kqueue so this code path doesn't run there.
 */
#ifdef __sun
static void
test_kevent_vnode_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           marker = 0xab;

    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_WRITE, 0, &marker);
    /* Re-EV_ADD modify with udata=NULL. */
    kevent_add(ctx->kqfd, &kev, ctx->vnode_fd, EVFILT_VNODE,
               EV_ADD | EV_ONESHOT, NOTE_WRITE, 0, NULL);

    testfile_write(ctx->testfile);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);
}
#endif

void
test_evfilt_vnode(struct test_context *ctx)
{
#if (defined(__sun) && !defined(HAVE_PORT_SOURCE_FILE))
    puts("**NOTE** EVFILT_VNODE is not supported on this version of Solaris");
    return;
#endif

    char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL)
#ifdef __ANDROID__
        tmpdir = "/data/local/tmp";
#else
        tmpdir = "/tmp";
#endif

    snprintf(ctx->testfile, sizeof(ctx->testfile), "%s/kqueue-test%d.tmp",
            tmpdir, testing_make_uid());

    test(kevent_vnode_add, ctx);
    test(kevent_vnode_del, ctx);
    test(kevent_vnode_disable_and_enable, ctx);
#ifdef EV_DISPATCH
    test(kevent_vnode_dispatch, ctx);
#endif
    test(kevent_vnode_note_write, ctx);
    test(kevent_vnode_note_attrib, ctx);
    test(kevent_vnode_note_rename, ctx);
    test(kevent_vnode_note_delete, ctx);
#ifdef NOTE_EXTEND
    test(kevent_vnode_note_extend, ctx);
#endif
#ifdef NOTE_LINK
    test(kevent_vnode_note_link, ctx);
#endif
#ifdef NOTE_TRUNCATE
    test(kevent_vnode_note_truncate, ctx);
#endif
#ifdef __sun
    test(kevent_vnode_modify_clobbers_udata, ctx);
#endif
    /* TODO: test r590 corner case where a descriptor is closed and
             the associated knote is automatically freed. */
    unlink(ctx->testfile);
    close(ctx->vnode_fd);
    ctx->vnode_fd = -1;
}
