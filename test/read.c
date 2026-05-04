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


/*
 * Create a connected TCP socket.
 */
static void
create_socket_connection(int *client_fd, int *server_fd, int *listen_fd)
{
    struct sockaddr_in sain;
    socklen_t sa_len = sizeof(sain);
    int one = 1;
    int clnt, srvr, accepted;
    short port;

    /* Create a passive socket */
    memset(&sain, 0, sizeof(sain));
    sain.sin_family = AF_INET;
    sain.sin_port = 0;
    if ((srvr = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    err(1, "socket");
    if (setsockopt(srvr, SOL_SOCKET, SO_REUSEADDR,
                (char *) &one, sizeof(one)) != 0)
    err(1, "setsockopt");
    if (bind(srvr, (struct sockaddr *) &sain, sa_len) < 0) {
        printf("unable to bind to auto-assigned port\n");
        err(1, "bind-1");
    }
    if (getsockname(srvr, (struct sockaddr *) &sain, &sa_len) < 0)
        err(1, "getsockname-1");
    port = ntohs(sain.sin_port);
    if (listen(srvr, 100) < 0)
    err(1, "listen");

    /* Simulate a client connecting to the server */
    sain.sin_family = AF_INET;
    sain.sin_port = htons(port);
    sain.sin_addr.s_addr = inet_addr("127.0.0.1");
    if ((clnt = socket(AF_INET, SOCK_STREAM, 0)) < 0)
       err(1, "clnt: socket");
    if (connect(clnt, (struct sockaddr *) &sain, sa_len) < 0)
       err(1, "clnt: connect");
    if ((accepted = accept(srvr, NULL, 0)) < 0)
       err(1, "srvr: accept");

    *client_fd = clnt;
    *server_fd = accepted;
    *listen_fd = srvr;
}

static void
kevent_socket_drain(struct test_context *ctx)
{
    char buf[1];

    /* Drain the read buffer, then make sure there are no more events. */
    if (recv(ctx->client_fd, buf, 1, 0) < 1)
        die("recv(2)");
}

static void
kevent_socket_fill(struct test_context *ctx, size_t len)
{
    uint8_t *data;

    data = malloc(len);
    memset(data, '.', len);

    if (send(ctx->server_fd, data, len, 0) < 1)
        die("send(2)");

    free(data);
}


void
test_kevent_socket_add(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD, 0, 0, &ctx->client_fd);
}

void
test_kevent_socket_add_without_ev_add(struct test_context *ctx)
{
    struct kevent kev;

    /* Try to add a kevent without specifying EV_ADD */
    EV_SET(&kev, ctx->client_fd, EVFILT_READ, 0, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(-1, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    kevent_socket_fill(ctx, 1);
    test_no_kevents(ctx->kqfd);
    kevent_socket_drain(ctx);

    /* Try to delete a kevent which does not exist */
    kev.flags = EV_DELETE;
    kevent_rv_cmp(-1, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

void
test_kevent_socket_get(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_ADD, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    kevent_socket_fill(ctx, 1);

    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kevent_socket_drain(ctx);
    test_no_kevents(ctx->kqfd);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

void
test_kevent_socket_clear(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    test_no_kevents(ctx->kqfd);
    kevent_socket_drain(ctx);

    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /*
     * write two bytes in one call.
     * This used to be two calls writing one byte, but macOS didn't
     * always reliably report the amount of pending data correctly
     * (1 byte instead of 2).
     *
     * Adding usleep(1000) on macOS also solved the issue, but this
     * seemed like a cleaner fix.
     */
    kevent_socket_fill(ctx, 2);

    /*
     * Modern Solaris/illumos reports the actual pending byte
     * count via ioctl(FIONREAD) on sockets, same as Linux/BSD,
     * so we expect 2 everywhere.  An older comment claimed
     * Solaris couldn't report this; that's no longer true.
     */
    kev.data = 2;

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1); /* data is pending, so we should get an event */
    kevent_cmp(&kev, ret);

    /* We filled twice, but drain once. Edge-triggered would not generate
       additional events.
     */
    kevent_socket_drain(ctx);   /* drain one byte, data is still pending... */
    test_no_kevents(ctx->kqfd); /* ...but because this is edge triggered we should get no events */

    kevent_socket_drain(ctx);
    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

void
test_kevent_socket_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* Add an event, then disable it. */
    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_ADD, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_DISABLE, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    kevent_socket_fill(ctx, 1);
    test_no_kevents(ctx->kqfd);

    /* Re-enable the knote, then see if an event is generated */
    kev.flags = EV_ENABLE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kev.flags = EV_ADD;
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kevent_socket_drain(ctx);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

void
test_kevent_socket_del(struct test_context *ctx)
{
    struct kevent kev;

    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_socket_fill(ctx, 1);
    test_no_kevents(ctx->kqfd);
    kevent_socket_drain(ctx);
}

void
test_kevent_socket_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* Re-add the watch and make sure no events are pending */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    kevent_socket_fill(ctx, 1);
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);

    /* Verify that the kernel watch has been deleted */
    kevent_socket_fill(ctx, 1);
    test_no_kevents(ctx->kqfd);
    kevent_socket_drain(ctx);

    /* Verify that the kevent structure does not exist. */
    kev.flags = EV_DELETE;
    kevent_rv_cmp(-1, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
}

/*
 * Test if the data field returns 1 when a listen(2) socket has
 * a pending connection.
 */
void
test_kevent_socket_listen_backlog(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct sockaddr_in sain;
    socklen_t sa_len = sizeof(sain);
    int one = 1;
    short port;
    int clnt, srvr;

    /* Create a passive socket */
    memset(&sain, 0, sizeof(sain));
    sain.sin_family = AF_INET;
    sain.sin_port = 0;
    if ((srvr = socket(PF_INET, SOCK_STREAM, 0)) < 0)
        err(1, "socket()");
    if (setsockopt(srvr, SOL_SOCKET, SO_REUSEADDR,
                (char *) &one, sizeof(one)) != 0)
        err(1, "setsockopt()");
    if (bind(srvr, (struct sockaddr *) &sain, sa_len) < 0)
        err(1, "bind-2");
    if (getsockname(srvr, (struct sockaddr *) &sain, &sa_len) < 0)
        err(1, "getsockname-2");
    port = ntohs(sain.sin_port);
    if (listen(srvr, 100) < 0)
        err(1, "listen()");

    /* Watch for events on the socket */
    test_no_kevents(ctx->kqfd);
    kevent_add(ctx->kqfd, &kev, srvr, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    /* Simulate a client connecting to the server */
    sain.sin_family = AF_INET;
    sain.sin_port = htons(port);
    sain.sin_addr.s_addr = inet_addr("127.0.0.1");
    if ((clnt = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        err(1, "socket()");
    if (connect(clnt, (struct sockaddr *) &sain, sa_len) < 0)
        err(1, "connect()");

    /* Verify that data=1 */
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    closesock(clnt);
    closesock(srvr);
}

#ifdef EV_DISPATCH
void
test_kevent_socket_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret[1];

    /* Re-add the watch and make sure no events are pending */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_DISPATCH, 0, 0, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    /* The event will occur only once, even though EV_CLEAR is not
       specified. */
    kevent_socket_fill(ctx, 1);
    kev.data = 1;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* Re-enable the kevent */
    /* FIXME- is EV_DISPATCH needed when rearming ? */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ENABLE | EV_DISPATCH, 0, 0, &ctx->client_fd);
    kev.data = 1;
    kev.flags = EV_ADD | EV_DISPATCH;   /* FIXME: may not be portable */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    test_no_kevents(ctx->kqfd);

    /* Since the knote is disabled, the EV_DELETE operation succeeds. */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, &ctx->client_fd);

    kevent_socket_drain(ctx);
}
#endif  /* EV_DISPATCH */

#if BROKEN_ON_LINUX
void
test_kevent_socket_lowat(struct test_context *ctx)
{
    struct kevent kev, buf;

    test_begin(test_id);

    /* Re-add the watch and make sure no events are pending */
    puts("-- re-adding knote, setting low watermark to 2 bytes");
    EV_SET(&kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_ONESHOT, NOTE_LOWAT, 2, &ctx->client_fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    test_no_kevents();

    puts("-- checking that one byte does not trigger an event..");
    kevent_socket_fill(ctx, 1);
    test_no_kevents();

    puts("-- checking that two bytes triggers an event..");
    kevent_socket_fill(ctx, 1);
    kevent_rv_cmp(1, kevent(ctx->kqfd, NULL, 0, &buf, 1, NULL));
    kevent_cmp(&kev, &buf);
    test_no_kevents();

    kevent_socket_drain(ctx);
    kevent_socket_drain(ctx);
}
#endif

void
test_kevent_socket_eof_clear(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    uint8_t buff[1024];

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    if (shutdown(ctx->server_fd, SHUT_RDWR) < 0)
        die("shutdown(2)");
    if (closesock(ctx->server_fd) < 0)
        die("close(2)");

    kev.flags |= EV_EOF;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1); /* edge triggered means no more events */
    kevent_cmp(&kev, ret);

    test_no_kevents(ctx->kqfd);;

    /* Delete the watch */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, &ctx->client_fd);

    closesock(ctx->client_fd);
    closesock(ctx->listen_fd);

    /* Recreate the socket pair */
    create_socket_connection(&ctx->client_fd, &ctx->server_fd, &ctx->listen_fd);
}

/*
 * Different from the pipe eof test, as we get EPOLLRDHUP with EPOLLIN on close
 * on Linux.
 */
void
test_kevent_socket_eof(struct test_context *ctx)
{
    struct kevent kev, ret[10];
    uint8_t buff[1024];

    kevent_add_with_receipt(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD, 0, 0, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    if (shutdown(ctx->server_fd, SHUT_RDWR) < 0)
        die("shutdown(2)");

    kev.flags |= EV_EOF;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Will repeatedly return EOF */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /*
     * The kqueue man page on FreeBSD states that EV_CLEAR
     * can be used to clear EOF, but in practice this appears
     * to do nothing with sockets...
     *
     * Additionally setting EV_CLEAR on a socket after it's
     * been added does nothing, even though kqueue returns
     * the flag with EV_RECEIPT.
     */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &ctx->client_fd);

    kev.flags |= EV_RECEIPT;
    kev.flags |= EV_EOF;
    kev.flags ^= EV_CLEAR;

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

    closesock(ctx->client_fd);
    closesock(ctx->server_fd);
    closesock(ctx->listen_fd);

    /* Recreate the socket pair */
    create_socket_connection(&ctx->client_fd, &ctx->server_fd, &ctx->listen_fd);
}

/*
 * Trigger TCP RST on the peer and confirm the kevent surfaces the
 * actual SO_ERROR value (not just a stamped 1) in fflags.
 *
 * SO_LINGER {l_onoff=1, l_linger=0} + close sends a RST instead of
 * a FIN, which raises POLLERR on the surviving end and sets
 * SO_ERROR to ECONNRESET (or similar, depending on kernel timing).
 * A correct backend reads SO_ERROR and propagates it as fflags.
 */
void
test_kevent_socket_so_error(struct test_context *ctx)
{
    struct kevent     kev, ret[1];
    struct linger     lin;

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_ADD, 0, 0, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    /* Force RST instead of graceful FIN on close. */
    lin.l_onoff = 1;
    lin.l_linger = 0;
    if (setsockopt(ctx->server_fd, SOL_SOCKET, SO_LINGER,
                   (const char *) &lin, sizeof(lin)) < 0)
        die("setsockopt(SO_LINGER)");
    if (closesock(ctx->server_fd) < 0)
        die("close(server_fd)");
    ctx->server_fd = -1;

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF on RST, got flags=0x%x", ret[0].flags);
    if (ret[0].fflags == 0)
        die("expected non-zero fflags (SO_ERROR) on RST, got 0");
    if (ret[0].fflags == 1)
        die("fflags is the placeholder 1; backend isn't reading SO_ERROR");
    /* Most kernels report ECONNRESET; some report EPIPE.  Accept any
     * real errno.  Just verify it's plausible. */
    if (ret[0].fflags != ECONNRESET && ret[0].fflags != EPIPE)
        printf("note: SO_ERROR reported as %u (%s); test passes anyway\n",
               (unsigned int) ret[0].fflags, strerror(ret[0].fflags));

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    closesock(ctx->client_fd);
    closesock(ctx->listen_fd);

    /* Recreate the socket pair for subsequent tests. */
    create_socket_connection(&ctx->client_fd, &ctx->server_fd, &ctx->listen_fd);
}

/*
 * NOTE_LOWAT: register EVFILT_READ with a low-water mark in kev.data.
 * Send fewer bytes than the threshold and confirm no event fires; send
 * enough to clear it and confirm one does.  Backend translates to
 * SO_RCVLOWAT setsockopt; the kernel does the gating.
 *
 * Skipped on Windows: the Win kernel doesn't honour SO_RCVLOWAT or
 * SO_SNDLOWAT (both setsockopt calls succeed silently with no effect).
 */
#ifndef _WIN32
void
test_kevent_socket_lowat_read(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct timespec timeout = { 0, 100L * 1000L * 1000L };  /* 100ms */
    int n;

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_ADD | EV_CLEAR, NOTE_LOWAT, 16, &ctx->client_fd);
    test_no_kevents(ctx->kqfd);

    /* 8 bytes: below threshold, no event. */
    kevent_socket_fill(ctx, 8);
    n = kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout);
    if (n != 0)
        die("expected no event with 8 bytes < threshold 16, got %d", n);

    /* 16 more bytes (24 total): above threshold, fires. */
    kevent_socket_fill(ctx, 16);
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected event after crossing threshold");
    if (ret[0].data < 16)
        die("expected data >= 16, got %ld", (long) ret[0].data);

    /* Drain the 24 bytes we sent. */
    {
        char drain[64];
        ssize_t got = recv(ctx->client_fd, drain, sizeof(drain), MSG_DONTWAIT);
        if (got < 0)
            die("recv");
    }

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
}

/*
 * NOTE_LOWAT for EVFILT_WRITE: setting the threshold above SO_SNDBUF
 * makes the threshold structurally unreachable (free space can never
 * exceed buffer size).  Verify no event fires while it's set, then
 * lower the threshold and verify we get one.
 */
void
test_kevent_socket_lowat_write(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct timespec timeout = { 0, 100L * 1000L * 1000L };  /* 100ms */
    int sndbuf;
    socklen_t slen = sizeof(sndbuf);
    int n;

    if (getsockopt(ctx->server_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &slen) < 0)
        die("getsockopt(SO_SNDBUF)");

    /* Threshold larger than the buffer ever has: cannot fire. */
    kevent_add(ctx->kqfd, &kev, ctx->server_fd, EVFILT_WRITE,
               EV_ADD | EV_CLEAR, NOTE_LOWAT, sndbuf + 4096, &ctx->server_fd);
    n = kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout);
    if (n != 0)
        die("expected no event with NOTE_LOWAT > SO_SNDBUF, got %d", n);

    /* Threshold = 1: fires immediately (buffer is empty / fully free). */
    kevent_add(ctx->kqfd, &kev, ctx->server_fd, EVFILT_WRITE,
               EV_ADD | EV_CLEAR, NOTE_LOWAT, 1, &ctx->server_fd);
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 1)
        die("expected event with NOTE_LOWAT=1");

    kevent_add(ctx->kqfd, &kev, ctx->server_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
}
#endif /* !_WIN32 (lowat) */

/*
 * Different from the socket eof test, as we get EPOLLHUP with no EPOLLIN on close
 * on Linux.  Win32 uses an overlapped 0-byte ReadFile attached to the kq's
 * IOCP via the test/win32_compat.h pipe() shim - same EOF semantics.
 */
void
test_kevent_pipe_eof(struct test_context *ctx)
{
    struct kevent kev, ret[256];
    int pipefd[2];
    uint8_t buff[1024];

    if (pipe(pipefd) < 0)
        die("pipe(2)");

    kevent_add_with_receipt(ctx->kqfd, &kev, pipefd[0], EVFILT_READ, EV_ADD, 0, 0, &pipefd[0]);
    test_no_kevents(ctx->kqfd);

    if (close(pipefd[1]) < 0)
        die("close(2)");
    pipefd[1] = -1;     /* avoid double-close on the trailing cleanup pair */

    kev.flags |= EV_EOF;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /* Will repeatedly return EOF */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    /*
     * The kqueue man page on FreeBSD states that EV_CLEAR
     * can be used to clear EOF, but in practice this appears
     * to do nothing with sockets...
     *
     * Additionally setting EV_CLEAR on a socket after it's
     * been added does nothing, even though kqueue returns
     * the flag with EV_RECEIPT.
     */
    kevent_add(ctx->kqfd, &kev, pipefd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &pipefd[0]);

    kev.flags |= EV_RECEIPT;
    kev.flags |= EV_EOF;
    kev.flags ^= EV_CLEAR;

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kevent_add(ctx->kqfd, &kev, pipefd[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);

    close(pipefd[0]);
    if (pipefd[1] != -1)
        close(pipefd[1]);
}

void
test_kevent_pipe_eof_multi(struct test_context *ctx)
{
    struct kevent kev, ret[256];
    int pipefd_a[2], pipefd_b[2];
    uint8_t buff[1024];

    if (pipe(pipefd_a) < 0)
        die("pipe(2)");

    if (pipe(pipefd_b) < 0)
        die("pipe(2)");

    kevent_add_with_receipt(ctx->kqfd, &kev, pipefd_a[0], EVFILT_READ, EV_ADD, 0, 0, &pipefd_a[0]);
    kevent_add_with_receipt(ctx->kqfd, &kev, pipefd_b[0], EVFILT_READ, EV_ADD, 0, 0, &pipefd_b[0]);
    test_no_kevents(ctx->kqfd);

    if (close(pipefd_a[1]) < 0)
        die("close(2)");
    pipefd_a[1] = -1;
    if (close(pipefd_b[1]) < 0)
        die("close(2)");
    pipefd_b[1] = -1;

    kev.flags |= EV_EOF;
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 2);

    kev.ident = ret[0].ident;
    kevent_cmp(&kev, &ret[0]);

    kev.ident = ret[1].ident;
    kevent_cmp(&kev, &ret[1]);

    /* Will repeatedly return EOF */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 2);

    kev.ident = ret[0].ident;
    kevent_cmp(&kev, &ret[0]);

    kev.ident = ret[1].ident;
    kevent_cmp(&kev, &ret[1]);

    kevent_add(ctx->kqfd, &kev, pipefd_a[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, pipefd_b[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);

    close(pipefd_a[0]);
    if (pipefd_a[1] != -1) close(pipefd_a[1]);
    close(pipefd_b[0]);
    if (pipefd_b[1] != -1) close(pipefd_b[1]);
}

/* Test if EVFILT_READ works with regular files */
void
test_kevent_regular_file(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    off_t curpos;
    int fd;

#ifdef _WIN32
    fd = open("C:\\Windows\\System32\\drivers\\etc\\hosts", O_RDONLY);
#else
    fd = open("/etc/hosts", O_RDONLY);
#endif
    if (fd < 0)
        abort();

    EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, &fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* Set file position to EOF-1 */
    ret->data--;
    if ((curpos = lseek(fd, ret->data, SEEK_SET)) != ret->data) {
        printf("seek to %u failed with rv=%lu\n",
                (unsigned int) ret->data, (unsigned long) curpos);
        abort();
    }

    /* Set file position to EOF */
    kevent_get(NULL, 0, ctx->kqfd, 1);
    ret->data = curpos + 1;
    if ((curpos = lseek(fd, ret->data, SEEK_SET)) != ret->data) {
        printf("seek to %u failed with rv=%lu\n",
                (unsigned int) ret->data, (unsigned long) curpos);
        abort();
    }

    test_no_kevents(ctx->kqfd);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    close(fd);
}

/*
 * BSD spec: an EVFILT_READ knote on a regular file is "active when
 * size > position".  After draining to EOF the knote is quiescent;
 * on a subsequent write that pushes size past the read position,
 * the knote must re-activate.  Tests the "another writer appended
 * to the file" case, which native BSD handles via a KNOTE() callback
 * from the vfs write path.
 */
void
test_kevent_regular_file_reactivate(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    char            path[1024];
    struct timespec timeout = { 2, 0 };
    int             rfd, wfd, tmpfd, i;
    const int       cycles = 3;

    snprintf(path, sizeof(path), "%s/libkqueue-test-XXXXXX", test_tmpdir());

    /* Empty file: mkstemp creates it with size 0. */
    {
        mode_t old_umask = umask(077);
        tmpfd = mkstemp(path);
        umask(old_umask);
    }
    if (tmpfd < 0) die("mkstemp");
    close(tmpfd);

    rfd = open(path, O_RDONLY);
    if (rfd < 0) die("open(rfd)");

    EV_SET(&kev, rfd, EVFILT_READ, EV_ADD, 0, 0, &rfd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    /*
     * Zero-size file: knote must not be active.  size == position
     * == 0, so data == 0 and BSD's "active when size > position"
     * rule says no fire.
     */
    test_no_kevents(ctx->kqfd);

    /*
     * Cycle: append, expect a fire, drain by seeking to EOF,
     * expect quiet.  Repeat to verify the re-activation path
     * works more than once (one-shot re-arm regressions would
     * pass cycle 1 and fail cycle 2).
     */
    wfd = open(path, O_WRONLY | O_APPEND);
    if (wfd < 0) die("open(wfd)");

    for (i = 1; i <= cycles; i++) {
        off_t expect_pos = (off_t) (4 * i);

        if (write(wfd, "abcd", 4) != 4) die("write cycle %d", i);

        if (kevent_get_timeout(ret, 1, ctx->kqfd, &timeout) != 1)
            die("cycle %d: no event after file grew", i);
        if (ret[0].data != 4)
            die("cycle %d: expected data=4, got %ld",
                i, (long) ret[0].data);

        /* Drain: seek to current EOF; knote must go quiet. */
        if (lseek(rfd, expect_pos, SEEK_SET) != expect_pos)
            die("lseek cycle %d", i);
        test_no_kevents(ctx->kqfd);
    }
    close(wfd);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    close(rfd);
    unlink(path);
}

/*
 * POSIX semantics: a file unlinked while open remains fully usable
 * through its still-open fds until the last one closes; the inode
 * persists, reads/writes work, fstat reports the live size.
 * EVFILT_READ on the read-fd should keep working through unlink, and
 * subsequent writes through the still-open write-fd must continue to
 * re-activate the knote.
 */
static void
test_kevent_regular_file_unlinked_continues(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    char            path[1024];
    struct timespec timeout = { 2, 0 };
    int             rfd, wfd, tmpfd, i;

    snprintf(path, sizeof(path), "%s/libkqueue-test-XXXXXX", test_tmpdir());

    {
        mode_t old_umask = umask(077);
        tmpfd = mkstemp(path);
        umask(old_umask);
    }
    if (tmpfd < 0) die("mkstemp");
    close(tmpfd);

    rfd = open(path, O_RDONLY);
    if (rfd < 0) die("open(rfd)");
    wfd = open(path, O_WRONLY | O_APPEND);
    if (wfd < 0) die("open(wfd)");

    /* Remove the directory entry while both fds are open. */
    if (unlink(path) != 0) die("unlink");

    EV_SET(&kev, rfd, EVFILT_READ, EV_ADD, 0, 0, &rfd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    test_no_kevents(ctx->kqfd);  /* size still 0 post-unlink */

    for (i = 1; i <= 2; i++) {
        off_t expect_pos = (off_t) (4 * i);

        if (write(wfd, "abcd", 4) != 4) die("write cycle %d", i);

        if (kevent_get_timeout(ret, 1, ctx->kqfd, &timeout) != 1)
            die("cycle %d: no event on unlinked file", i);
        if (ret[0].data != 4)
            die("cycle %d: expected data=4, got %ld",
                i, (long) ret[0].data);

        if (lseek(rfd, expect_pos, SEEK_SET) != expect_pos)
            die("lseek cycle %d", i);
        test_no_kevents(ctx->kqfd);
    }

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    close(rfd);
    close(wfd);
}

/*
 * rename(2) preserves the inode; the watcher's fd points at the same
 * underlying file at a different path.  EVFILT_READ should be
 * unaffected: subsequent writes through the still-open write-fd must
 * continue to re-activate.
 */
static void
test_kevent_regular_file_renamed_continues(struct test_context *ctx)
{
    struct kevent   kev, ret[1];
    char            path1[1024];
    char            path2[1024 + 16];
    struct timespec timeout = { 2, 0 };
    int             rfd, wfd, tmpfd, i;

    snprintf(path1, sizeof(path1), "%s/libkqueue-test-XXXXXX", test_tmpdir());

    {
        mode_t old_umask = umask(077);
        tmpfd = mkstemp(path1);
        umask(old_umask);
    }
    if (tmpfd < 0) die("mkstemp");
    close(tmpfd);
    snprintf(path2, sizeof(path2), "%s.renamed", path1);

    rfd = open(path1, O_RDONLY);
    if (rfd < 0) die("open(rfd)");
    wfd = open(path1, O_WRONLY | O_APPEND);
    if (wfd < 0) die("open(wfd)");

    EV_SET(&kev, rfd, EVFILT_READ, EV_ADD, 0, 0, &rfd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    if (rename(path1, path2) != 0) die("rename");

    test_no_kevents(ctx->kqfd);

    for (i = 1; i <= 2; i++) {
        off_t expect_pos = (off_t) (4 * i);

        if (write(wfd, "abcd", 4) != 4) die("write cycle %d", i);

        if (kevent_get_timeout(ret, 1, ctx->kqfd, &timeout) != 1)
            die("cycle %d: no event after rename+grow", i);
        if (ret[0].data != 4)
            die("cycle %d: expected data=4, got %ld",
                i, (long) ret[0].data);

        if (lseek(rfd, expect_pos, SEEK_SET) != expect_pos)
            die("lseek cycle %d", i);
        test_no_kevents(ctx->kqfd);
    }

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    close(rfd);
    close(wfd);
    unlink(path2);
}

#if defined(_WIN32) && _WIN32_WINNT >= 0x0A00
/*
 * Win10 1803+ supports AF_UNIX SOCK_STREAM natively.  Smoke
 * test: bind a path-based listener, accept a client, register
 * EVFILT_READ on the accepted server side, send 1 byte, expect a
 * read wakeup with data > 0.  Backed by the same WSAEventSelect
 * path as AF_INET sockets - no backend change required.
 *
 * Issue #146.  Gated on _WIN32_WINNT >= 0x0A00 so older SDKs
 * still compile.
 */
#include <afunix.h>     /* SOCKADDR_UN */

static void
test_kevent_socket_af_unix(struct test_context *ctx)
{
    SOCKET srv = INVALID_SOCKET, clt = INVALID_SOCKET, acc = INVALID_SOCKET;
    char tmp[MAX_PATH];
    SOCKADDR_UN addr = { 0 };
    struct kevent kev, ret[1];
    char buf;

    if (GetTempPathA(sizeof(tmp), tmp) == 0)
        err(1, "GetTempPathA");

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%skq-af-unix-%d.sock", tmp, testing_make_uid());
    DeleteFileA(addr.sun_path); /* idempotent: prior leftover */

    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    clt = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET || clt == INVALID_SOCKET)
        err(1, "socket(AF_UNIX)");

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        err(1, "bind(AF_UNIX, %s)", addr.sun_path);
    if (listen(srv, 1) != 0)
        err(1, "listen(AF_UNIX)");
    if (connect(clt, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        err(1, "connect(AF_UNIX)");

    acc = accept(srv, NULL, NULL);
    if (acc == INVALID_SOCKET)
        err(1, "accept(AF_UNIX)");

    kevent_add(ctx->kqfd, &kev, acc, EVFILT_READ,
               EV_ADD | EV_ONESHOT, 0, 0, NULL);

    if (send(clt, "x", 1, 0) != 1)
        err(1, "send(AF_UNIX)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].ident != (uintptr_t)acc ||
        ret[0].filter != EVFILT_READ ||
        ret[0].data < 1)
        die("af_unix: unexpected event %s", kevent_to_str(&ret[0]));

    /* Drain so a stray FD_READ re-record doesn't surface in the
     * next test. */
    if (recv(acc, &buf, 1, 0) != 1)
        die("recv(AF_UNIX) after fire");

    closesocket(acc);
    closesocket(clt);
    closesocket(srv);
    DeleteFileA(addr.sun_path);
}
#endif /* _WIN32 && Win10 */

/* Test transitioning a socket from EVFILT_WRITE to EVFILT_READ */
#ifndef _WIN32
void
test_transition_from_write_to_read(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int kqfd;
    int sd[2];

    (void) ctx;
    if ((kqfd = kqueue()) < 0)
        err(1, "kqueue");

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sd))
        err(1, "socketpair");

    EV_SET(&kev, sd[0], EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(kqfd, &kev, 1, NULL, 0, NULL));

    EV_SET(&kev, sd[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent_rv_cmp(0, kevent(kqfd, &kev, 1, NULL, 0, NULL));

    close(sd[0]);
    close(sd[1]);
    close(kqfd);
}
#endif /* !_WIN32 */

/*
 * Flag-behaviour tests for EVFILT_READ on sockets.
 */

/*
 * EV_DISABLE drops pending fires: write data so the kernel marks the
 * socket readable, then EV_DISABLE before the drain.  No event must
 * surface.
 */
static void
test_kevent_socket_disable_drains(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct timespec timeout = { 0, 100L * 1000L * 1000L };

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_ADD, 0, 0, &ctx->client_fd);

    /* Sync: write returns when data is in the receiver's kernel buffer. */
    kevent_socket_fill(ctx, 1);

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_DISABLE, 0, 0, &ctx->client_fd);
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 0)
        die("expected 0 events after EV_DISABLE on socket");

    /* Cleanup: drain the byte and remove the knote. */
    {
        char drain;
        (void) recv(ctx->client_fd, &drain, 1, MSG_DONTWAIT);
    }
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_DELETE, 0, 0, NULL);
}

/*
 * EV_DELETE drops pending fires: same shape via delete.
 */
static void
test_kevent_socket_delete_drains(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    struct timespec timeout = { 0, 100L * 1000L * 1000L };

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_ADD, 0, 0, &ctx->client_fd);
    kevent_socket_fill(ctx, 1);

    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_DELETE, 0, 0, &ctx->client_fd);
    if (kevent(ctx->kqfd, NULL, 0, ret, 1, &timeout) != 0)
        die("expected 0 events after EV_DELETE on socket");

    /* Cleanup: drain the byte. */
    {
        char drain;
        (void) recv(ctx->client_fd, &drain, 1, MSG_DONTWAIT);
    }
}

/*
 * BSD overwrites kn->kev.udata on every modify.  Socket modify is
 * gated on EV_CLEAR being set.
 */
static void
test_kevent_socket_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           marker = 0xab;

    /* Initial registration with EV_CLEAR + udata=&marker. */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_ADD | EV_CLEAR, 0, 0, &marker);

    /* Modify with udata=NULL.  EV_CLEAR required for socket modify. */
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_ADD | EV_CLEAR, 0, 0, NULL);

    kevent_socket_fill(ctx, 1);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("expected udata clobbered to NULL, got %p", ret[0].udata);

    /* Cleanup. */
    {
        char drain;
        (void) recv(ctx->client_fd, &drain, 1, MSG_DONTWAIT);
    }
    kevent_add(ctx->kqfd, &kev, ctx->client_fd, EVFILT_READ,
               EV_DELETE, 0, 0, NULL);
}

/*
 * EV_DELETE on a never-registered fd returns ENOENT.
 */
static void
test_kevent_read_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;
    int           pfd[2];

    if (pipe(pfd) < 0) die("pipe");
    EV_SET(&kev, pfd[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added knote should fail");
    if (errno != ENOENT)
        die("expected ENOENT, got %d (%s)", errno, strerror(errno));
    close(pfd[0]);
    close(pfd[1]);
}

/*
 * udata round-trips through delivery.
 */
static void
test_kevent_read_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    void         *marker = (void *) 0xABCDEF01UL;
    int           pfd[2];

    if (pipe(pfd) < 0) die("pipe");
    EV_SET(&kev, pfd[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    if (write(pfd[1], "x", 1) != 1) die("write");
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p expected %p", ret[0].udata, marker);

    close(pfd[0]);
    close(pfd[1]);
}

/*
 * Two kqueues on the same fd: change must fire on both, draining
 * one must not affect the other.
 */
static void
test_kevent_read_multi_kqueue(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           kq2;
    int           pfd[2];

    if (pipe(pfd) < 0) die("pipe");
    if ((kq2 = kqueue()) < 0) die("kqueue(2)");

    EV_SET(&kev, pfd[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent(kq1)");
    EV_SET(&kev, pfd[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    if (kevent(kq2, &kev, 1, NULL, 0, NULL) < 0) die("kevent(kq2)");

    if (write(pfd[1], "x", 1) != 1) die("write");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_get(ret, NUM_ELEMENTS(ret), kq2, 1);

    close(kq2);
    close(pfd[0]);
    close(pfd[1]);
}

/*
 * Listen socket with N pending connections: kev.data must be N,
 * not just 1.  FreeBSD filt_soread sets kn_data = sol_qlen
 * (uipc_socket.c:4599).
 */
static void
test_kevent_read_listen_backlog_count(struct test_context *ctx)
{
    struct kevent      kev, ret[1];
    struct sockaddr_in sain;
    socklen_t          slen = sizeof(sain);
    int                lst, c1, c2;
    int                one = 1;

    memset(&sain, 0, sizeof(sain));
    sain.sin_family      = AF_INET;
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((lst = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(listen)");
    if (setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) die("SO_REUSEADDR");
    if (bind(lst, (struct sockaddr *)&sain, slen) < 0) die("bind");
    if (getsockname(lst, (struct sockaddr *)&sain, &slen) < 0) die("getsockname");
    if (listen(lst, 8) < 0) die("listen");

    if ((c1 = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(c1)");
    if ((c2 = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(c2)");
    if (connect(c1, (struct sockaddr *)&sain, slen) < 0) die("connect c1");
    if (connect(c2, (struct sockaddr *)&sain, slen) < 0) die("connect c2");

    /* Brief pause so the listen queue settles. */
    usleep(10 * 1000);

    EV_SET(&kev, lst, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].data < 2)
        die("listen-backlog with 2 pending: kev.data=%lld, expected >= 2",
            (long long) ret[0].data);

    close(c1);
    close(c2);
    close(lst);
}

/*
 * Pipe with N bytes written: kev.data must equal N.  Two writes
 * before drain: data carries the total, single event delivered.
 */
static void
test_kevent_read_pipe_data_exact_count(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           pfd[2];
    const char    payload[] = "hello world";
    const size_t  len = sizeof(payload) - 1;

    if (pipe(pfd) < 0) die("pipe");
    EV_SET(&kev, pfd[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    /* Two writes between drains. */
    if (write(pfd[1], payload, len) != (ssize_t) len) die("write 1");
    if (write(pfd[1], payload, len) != (ssize_t) len) die("write 2");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].data != (intptr_t)(len * 2))
        die("pipe data: got %lld, expected %zu (2 writes coalesced)",
            (long long) ret[0].data, len * 2);

    EV_SET(&kev, pfd[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
    close(pfd[0]);
    close(pfd[1]);
}

/*
 * Peer writes N then shutdown(SHUT_WR): expect a single event
 * carrying both EV_EOF and kev.data == N.  FreeBSD filt_soread
 * gates EOF on SBS_CANTRCVMORE regardless of sb_cc, so EOF arrives
 * with the buffered bytes still drainable.
 */
static void
test_kevent_read_socket_eof_with_buffered(struct test_context *ctx)
{
    struct kevent      kev, ret[1];
    struct sockaddr_in sain;
    socklen_t          slen = sizeof(sain);
    int                lst, c, s;
    int                one = 1;
    const char         payload[] = "buffered";
    const size_t       len = sizeof(payload) - 1;

    memset(&sain, 0, sizeof(sain));
    sain.sin_family      = AF_INET;
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((lst = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket");
    if (setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) die("SO_REUSEADDR");
    if (bind(lst, (struct sockaddr *)&sain, slen) < 0) die("bind");
    if (getsockname(lst, (struct sockaddr *)&sain, &slen) < 0) die("getsockname");
    if (listen(lst, 8) < 0) die("listen");

    if ((c = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(c)");
    if (connect(c, (struct sockaddr *)&sain, slen) < 0) die("connect");
    if ((s = accept(lst, NULL, NULL)) < 0) die("accept");
    close(lst);

    /* Peer writes then shuts the write side. */
    if (send(s, payload, len, 0) != (ssize_t) len) die("send");
    if (shutdown(s, SHUT_WR) < 0) die("shutdown");

    /* Brief settle so the FIN reaches our side. */
    usleep(10 * 1000);

    EV_SET(&kev, c, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF: %s", kevent_to_str(&ret[0]));
    if (ret[0].data != (intptr_t) len)
        die("expected data=%zu with EV_EOF, got %lld",
            len, (long long) ret[0].data);

    close(c);
    close(s);
}

void
test_evfilt_read(struct test_context *ctx)
{
    create_socket_connection(&ctx->client_fd, &ctx->server_fd, &ctx->listen_fd);

    test(kevent_socket_add, ctx);
    test(kevent_socket_del, ctx);
    test(kevent_socket_add_without_ev_add, ctx);
    test(kevent_socket_get, ctx);
    test(kevent_socket_disable_and_enable, ctx);
    test(kevent_socket_oneshot, ctx);
    test(kevent_socket_clear, ctx);
#ifdef EV_DISPATCH
    test(kevent_socket_dispatch, ctx);
#endif
    test(kevent_socket_listen_backlog, ctx);
    test(kevent_socket_eof_clear, ctx);
    test(kevent_socket_eof, ctx);
    test(kevent_socket_so_error, ctx);
#if !defined(__sun) && !defined(_WIN32)
    /*
     * SO_RCVLOWAT setsockopt works on Linux glibc; not on Solaris
     * (illumos returns ENOPROTOOPT for both lowat options).
     */
    test(kevent_socket_lowat_read, ctx);
#endif
#if !defined(__sun) && !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)
    /*
     * SO_SNDLOWAT setsockopt is unsupported on Linux (socket(7): present
     * for BSD compat only).
     *
     * On macOS, XNU clamps SO_SNDLOWAT to sb_hiwat (SO_SNDBUF) in
     * setsockopt and again in EVFILT_WRITE.  A threshold above SO_SNDBUF
     * is silently treated as SO_SNDBUF, and sbspace() on an idle socket
     * equals SO_SNDBUF, so the "threshold > buffer = never fire" invariant
     * this test relies on is impossible to express on macOS.
     */
    test(kevent_socket_lowat_write, ctx);
#endif
    /* Flag-behaviour group */
    test(kevent_socket_disable_drains, ctx);
    test(kevent_socket_delete_drains, ctx);
    test(kevent_socket_modify_clobbers_udata, ctx);

    test(kevent_read_del_nonexistent, ctx);
    test(kevent_read_udata_preserved, ctx);
    test(kevent_read_multi_kqueue, ctx);
    /*
     * Listen-socket backlog count reporting requires a kernel
     * ioctl that exposes sol_qlen.  FreeBSD/macOS native kqueue
     * read it directly from the socket struct; Linux has no
     * portable equivalent for listen sockets, and the POSIX
     * backend hardcodes 1.  Gate on backends that can deliver.
     */
    /*
     * Solaris: illumos has no userspace API for the listen-queue
     * length (no SO_QLEN, no listen-queue ioctl exposed); the
     * solaris socket filter hardcodes data=1.  Gate alongside the
     * existing POSIX/Linux exclusions.
     */
#if !defined(LIBKQUEUE_BACKEND_POSIX) && !defined(LIBKQUEUE_BACKEND_LINUX) && \
    !defined(LIBKQUEUE_BACKEND_SOLARIS)
    test(kevent_read_listen_backlog_count, ctx);
#endif
    test(kevent_read_pipe_data_exact_count, ctx);
    /*
     * POSIX backend defers EV_EOF until the peer-buffered bytes
     * are drained (FIONREAD-zero is the only EOF signal pselect
     * exposes).  BSD/macOS/Linux deliver EOF + buffered-bytes
     * together because they observe the kernel's CANTRCVMORE flag
     * directly.
     */
#if !defined(LIBKQUEUE_BACKEND_POSIX)
    test(kevent_read_socket_eof_with_buffered, ctx);
#endif
    test(kevent_pipe_eof, ctx);
    test(kevent_pipe_eof_multi, ctx);
    test(kevent_regular_file, ctx);
    /*
     * BSD's "EVFILT_READ on a regular file is active when size >
     * position" rule re-fires the knote after a consumer drains to
     * EOF and a producer appends more.  The Linux backend uses an
     * eventfd-as-polling-trigger that DELs itself once size==
     * position is observed and never re-arms - implementing the
     * re-arm needs either inotify on /proc/self/fd/N (depends on
     * /proc being mounted and accessible, fragile under hardened
     * containers) or fanotify with CAP_SYS_ADMIN.  Gate until
     * either becomes acceptable; same outcome as upstream master.
     * The POSIX backend has its own size-grow gap.
     */
#if !defined(LIBKQUEUE_BACKEND_LINUX) && !defined(LIBKQUEUE_BACKEND_POSIX)
    test(kevent_regular_file_reactivate, ctx);
    test(kevent_regular_file_unlinked_continues, ctx);
    test(kevent_regular_file_renamed_continues, ctx);
#endif
#if defined(_WIN32) && _WIN32_WINNT >= 0x0A00
    test(kevent_socket_af_unix, ctx);
#endif
    closesock(ctx->client_fd);
    closesock(ctx->server_fd);
    closesock(ctx->listen_fd);

    create_socket_connection(&ctx->client_fd, &ctx->server_fd, &ctx->listen_fd);
#ifndef _WIN32
    test(transition_from_write_to_read, ctx);
#endif
    closesock(ctx->client_fd);
    closesock(ctx->server_fd);
    closesock(ctx->listen_fd);
}
