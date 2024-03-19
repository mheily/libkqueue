/*
 * Copyright (c) 2024 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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


/** Test if we can setup an event to write to a regular file
 *
 * Setting up a write event on a regular file doesn't make much sense
 * but it is allowed by kqueue, so check we exhibit similar behaviour.
 */
void
test_kevent_write_regular_file(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int fd;

    fd = open(ctx->testfile, O_CREAT | O_WRONLY, S_IRWXU);
    if (fd < 0)
        abort();

    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, &fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

#ifdef __APPLE__
    /* macOS sets this high for some reason */
    kev.data = 1;
#endif

    /* File should appear immediately writable */
    kevent_get(NULL, 0, ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    if (write(fd, "test", 4) != 4) {
        printf("failed writing to set file: %s", strerror(errno));
        abort();
    }

    /* ...should still be writable */
    kevent_get(NULL, 0, ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    close(fd);
    unlink(ctx->testfile);
}

void
test_evfilt_write(struct test_context *ctx)
{
    char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL)
#ifdef __ANDROID__
        tmpdir = "/data/local/tmp";
#else
        tmpdir = "/tmp";
#endif

    snprintf(ctx->testfile, sizeof(ctx->testfile), "%s/kqueue-test%d.tmp",
            tmpdir, testing_make_uid());

    test(kevent_write_regular_file, ctx);
}
