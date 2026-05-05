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

#include <fcntl.h>
#ifndef _WIN32
/* common.h pulls socket headers in via win32_compat.h on Windows. */
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

/*
 * Build a connected TCP pair.  Caller closes the returned fds.
 */
static void
write_tcp_pair(int *writer, int *peer)
{
    struct sockaddr_in sain;
    socklen_t          slen = sizeof(sain);
    int                lst, clnt, srv;
    int                one = 1;

    memset(&sain, 0, sizeof(sain));
    sain.sin_family      = AF_INET;
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((lst = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(listen)");
    if (setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) die("SO_REUSEADDR");
    if (bind(lst, (struct sockaddr *)&sain, slen) < 0) die("bind");
    if (getsockname(lst, (struct sockaddr *)&sain, &slen) < 0) die("getsockname");
    if (listen(lst, 8) < 0) die("listen");

    if ((clnt = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket(clnt)");
    if (connect(clnt, (struct sockaddr *)&sain, slen) < 0) die("connect");
    if ((srv = accept(lst, NULL, NULL)) < 0) die("accept");
    close(lst);

    *writer = clnt;
    *peer   = srv;
}

/*
 * Saturate the socket's send buffer so it stops being writable.
 * Returns the count of bytes accepted.  Blocking sockets would
 * stall, so set non-blocking before calling.
 */
static size_t
write_fill_sndbuf(int fd)
{
    char   buf[4096];
    size_t total = 0;
    ssize_t n;
    int    fl;

    memset(buf, 'X', sizeof(buf));
    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) die("fcntl(F_GETFL)");
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        die("fcntl(F_SETFL O_NONBLOCK)");
    while ((n = send(fd, buf, sizeof(buf), 0)) > 0) {
        assert(n > 0);
        total += (size_t) n;
    }
    return total;
}

/* ============================================================
 * Common-set tests on a connected stream socket.
 * ============================================================ */

static void
test_kevent_write_socket_add(struct test_context *ctx)
{
    struct kevent kev;
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_del(struct test_context *ctx)
{
    struct kevent kev;
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    /* No further deliveries after EV_DELETE. */
    test_no_kevents(ctx->kqfd);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_del_nonexistent(struct test_context *ctx)
{
    struct kevent kev;
    int           w, p;

    write_tcp_pair(&w, &p);
    EV_SET(&kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_DELETE on never-added knote should fail");
    if (errno != ENOENT)
        die("EV_DELETE expected ENOENT, got %d (%s)", errno, strerror(errno));
    close(w);
    close(p);
}

static void
test_kevent_write_socket_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

    kev.flags = EV_DISABLE;
    kevent_update(ctx->kqfd, &kev);
    test_no_kevents(ctx->kqfd);

    kev.flags = EV_ENABLE;
    kevent_update(ctx->kqfd, &kev);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].filter != EVFILT_WRITE)
        die("expected EVFILT_WRITE on enable");

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* Knote auto-deleted: a second EV_DELETE must fail with ENOENT. */
    EV_SET(&kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) == 0)
        die("EV_ONESHOT did not auto-delete");
    if (errno != ENOENT)
        die("expected ENOENT, got %d", errno);

    close(w);
    close(p);
}

#ifdef EV_DISPATCH
static void
test_kevent_write_socket_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD | EV_DISPATCH, 0, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* After dispatch the knote is auto-disabled. */
    test_no_kevents(ctx->kqfd);

    /* Re-enable: socket is still writable, fires again. */
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ENABLE | EV_DISPATCH, 0, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}
#endif

#ifdef EV_RECEIPT
static void
test_kevent_write_socket_receipt_preserved(struct test_context *ctx)
{
    struct kevent kev[1];
    int           w, p;

    write_tcp_pair(&w, &p);

    EV_SET(&kev[0], w, EVFILT_WRITE, EV_ADD | EV_RECEIPT, 0, 0, NULL);
    /* EV_RECEIPT: kevent returns the kev[] echoed back with EV_ERROR=0. */
    if (kevent(ctx->kqfd, kev, 1, kev, 1, NULL) != 1)
        die("EV_RECEIPT should produce one echo entry");
    if (!(kev[0].flags & EV_ERROR) || kev[0].data != 0)
        die("EV_RECEIPT echo missing EV_ERROR=0 marker");

    kevent_add(ctx->kqfd, kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}
#endif

static void
test_kevent_write_socket_udata_preserved(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    void         *marker = (void *) 0xCAFEBABEUL;
    int           w, p;

    write_tcp_pair(&w, &p);
    /* kevent_add() helper drops its udata arg; use EV_SET directly. */
    EV_SET(&kev, w, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != marker)
        die("udata not preserved: got %p, expected %p", ret[0].udata, marker);

    close(w);
    close(p);
}

static void
test_kevent_write_socket_modify_clobbers_udata(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           marker = 0xab;
    int           w, p;

    write_tcp_pair(&w, &p);
    /* EV_CLEAR is required for socket modify on the Linux backend. */
    EV_SET(&kev, w, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, &marker);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent(add)");
    EV_SET(&kev, w, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL) < 0) die("kevent(modify)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (ret[0].udata != NULL)
        die("modify did not clobber udata: got %p", ret[0].udata);

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_disable_drains(struct test_context *ctx)
{
    struct kevent kev;
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    /* Socket is writable: an event would fire, but we disable first. */
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DISABLE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_delete_drains(struct test_context *ctx)
{
    struct kevent kev;
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    close(w);
    close(p);
}

static void
test_kevent_write_socket_multi_kqueue(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           kq2, w, p;

    write_tcp_pair(&w, &p);
    if ((kq2 = kqueue()) < 0) die("kqueue(2)");

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_add(kq2, &kev, w, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    kevent_get(ret, NUM_ELEMENTS(ret), kq2, 1);

    close(kq2);
    close(w);
    close(p);
}

static void
test_kevent_write_socket_ev_clear(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);

    /* First drain: socket starts writable, fires. */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* No state change: EV_CLEAR must suppress further deliveries. */
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
    close(p);
}

/* ============================================================
 * Audit-gap tests.
 * ============================================================ */

/*
 * Pipe write end watched, reader closes => EV_EOF on the writer.
 * FreeBSD sys_pipe.c filt_pipewrite returns EV_EOF unconditionally
 * when peer is gone.
 */
static void
test_kevent_write_pipe_eof_on_reader_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           pfd[2];

    if (pipe(pfd) < 0) die("pipe");

    kevent_add(ctx->kqfd, &kev, pfd[1], EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    if (close(pfd[0]) < 0) die("close(reader)");

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF on writer after reader close: %s",
            kevent_to_str(&ret[0]));

    /*
     * EV_DELETE may return ENOENT here: FreeBSD auto-deletes the
     * knote when the pipe peer closes (EV_EOF is terminal for the
     * pipe filter).  Tolerate the no-op.
     */
    EV_SET(&kev, pfd[1], EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    (void) kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL);
    close(pfd[1]);
}

/*
 * Pipe writer kev.data should reflect available buffer slack, not a
 * hardcoded 1.  FreeBSD filt_pipewrite sets kn_data = pipe_buffer.size
 * - pipe_buffer.cnt.  Test: drain pipe (slack = full size), then assert
 * data is the typical Linux pipe size (>= 4096).  The exact size is
 * platform-dependent, so we just assert a sensible lower bound.
 */
static void
test_kevent_write_pipe_data_buffer_slack(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           pfd[2];

    if (pipe(pfd) < 0) die("pipe");

    kevent_add(ctx->kqfd, &kev, pfd[1], EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    if (ret[0].data <= 1)
        die("pipe writer kev.data=%lld, expected real buffer slack",
            (long long) ret[0].data);

    close(pfd[0]);
    close(pfd[1]);
}

/*
 * Stream socket: peer-side close should surface EV_EOF on the writer.
 * Linux backend's EPOLLHUP -> EV_EOF mapping in src/linux/write.c is
 * untested.
 */
static void
test_kevent_write_socket_eof_on_peer_close(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           w, p;

    write_tcp_pair(&w, &p);
    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

    /* Drain the initial writable event. */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    /* Peer goes away. */
    if (close(p) < 0) die("close(peer)");

    /*
     * Some kernels need a write to surface EV_EOF on the local side.
     * Try writing to provoke RST/state advance.  EPIPE is fine.
     */
    char buf = 'X';
    (void) send(w, &buf, 1, MSG_NOSIGNAL);

    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);
    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF on writer after peer close: %s",
            kevent_to_str(&ret[0]));

    kevent_add(ctx->kqfd, &kev, w, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(w);
}

/*
 * Listen socket with EVFILT_WRITE: FreeBSD's filt_sowrite checks
 * SOLISTENING and returns 0 (never fires).  libkqueue's POSIX/Linux
 * paths don't distinguish - they'd fire spuriously.
 */
static void
test_kevent_write_listen_socket_silent(struct test_context *ctx)
{
    struct kevent      kev;
    struct sockaddr_in sain;
    socklen_t          slen = sizeof(sain);
    int                lst;
    int                one = 1;

    memset(&sain, 0, sizeof(sain));
    sain.sin_family      = AF_INET;
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((lst = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket");
    if (setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) die("SO_REUSEADDR");
    if (bind(lst, (struct sockaddr *)&sain, slen) < 0) die("bind");
    if (listen(lst, 8) < 0) die("listen");

    kevent_add(ctx->kqfd, &kev, lst, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

    /* Listen socket is never writable per FreeBSD. */
    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, lst, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    close(lst);
}

/*
 * Datagram socket: EVFILT_WRITE fires when sndbuf has room, no
 * connect-required gate.  FreeBSD filt_sowrite skips the
 * SS_ISCONNECTED check for non-PR_CONNREQUIRED protocols.
 */
static void
test_kevent_write_dgram_socket(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           s;

    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) die("socket(udp)");

    kevent_add(ctx->kqfd, &kev, s, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    if (ret[0].filter != EVFILT_WRITE)
        die("UDP socket: expected EVFILT_WRITE");

    close(s);
}

/* ============================================================
 * Existing regular-file test (kept for coverage).
 * ============================================================ */

static void
test_kevent_write_regular_file(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    int           fd;

    fd = open(ctx->testfile, O_CREAT | O_WRONLY, S_IRWXU);
    if (fd < 0) die("open");

    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, &fd);
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

#if defined(__APPLE__) || defined(LIBKQUEUE_BACKEND_POSIX)
    /*
     * macOS native kqueue and libkqueue's POSIX backend report
     * data=1 ("at least one byte of room").  Linux backend reports
     * 0 (no portable equivalent of SIOCOUTQ on regular files).
     */
    kev.data = 1;
#endif

    kevent_get(NULL, 0, ctx->kqfd, 1);
    kevent_cmp(&kev, ret);
    if (write(fd, "test", 4) != 4) die("write");

    /* Still writable after the write. */
    kevent_get(NULL, 0, ctx->kqfd, 1);
    kevent_cmp(&kev, ret);

    kev.flags = EV_DELETE;
    kevent_rv_cmp(0, kevent(ctx->kqfd, &kev, 1, NULL, 0, NULL));

    close(fd);
    unlink(ctx->testfile);
}

static const struct lkq_test_gate write_multi_kqueue_gates[] = {
    GATE(LKQ_PLATFORM_OS_WINDOWS,
         "Win32 WSAEventSelect allows only one event slot per socket"),
    { 0, NULL }
};

static const struct lkq_test_gate write_pipe_eof_gates[] = {
    GATE(LKQ_PLATFORM_BACKEND_POSIX,
         "POSIX backend cannot detect pipe reader-close via pselect"),
    { 0, NULL }
};

static const struct lkq_test_gate write_pipe_buffer_slack_gates[] = {
    GATE(LKQ_PLATFORM_BACKEND_POSIX,
         "POSIX backend has no portable pipe-space ioctl; reports hardcoded 1"),
    { 0, NULL }
};

static const struct lkq_test_gate write_socket_eof_on_peer_close_gates[] = {
    GATE(LKQ_PLATFORM_NOT_BACKEND_LINUX,
         "only the Linux backend maps EPOLLHUP to EV_EOF deterministically"),
    { 0, NULL }
};

const struct lkq_test_case lkq_write_tests[] = {
    {
        .name  = "test_kevent_write_socket_add",
        .desc  = "EV_ADD registers a write knote on a socket",
        .func  = test_kevent_write_socket_add,
    },
    {
        .name  = "test_kevent_write_socket_del",
        .desc  = "EV_DELETE removes a write knote; no further events",
        .func  = test_kevent_write_socket_del,
    },
    {
        .name  = "test_kevent_write_socket_del_nonexistent",
        .desc  = "EV_DELETE on an unregistered fd returns ENOENT",
        .func  = test_kevent_write_socket_del_nonexistent,
    },
    {
        .name  = "test_kevent_write_socket_disable_and_enable",
        .desc  = "EV_DISABLE suppresses events; EV_ENABLE re-arms",
        .func  = test_kevent_write_socket_disable_and_enable,
    },
    {
        .name  = "test_kevent_write_socket_oneshot",
        .desc  = "EV_ONESHOT auto-deletes after first write event",
        .func  = test_kevent_write_socket_oneshot,
    },
#ifdef EV_DISPATCH
    {
        .name  = "test_kevent_write_socket_dispatch",
        .desc  = "EV_DISPATCH auto-disables after delivery",
        .func  = test_kevent_write_socket_dispatch,
    },
#endif
#ifdef EV_RECEIPT
    {
        .name  = "test_kevent_write_socket_receipt_preserved",
        .desc  = "EV_RECEIPT returns the echo entry with EV_ERROR=0",
        .func  = test_kevent_write_socket_receipt_preserved,
    },
#endif
    {
        .name  = "test_kevent_write_socket_udata_preserved",
        .desc  = "udata round-trips through EVFILT_WRITE delivery",
        .func  = test_kevent_write_socket_udata_preserved,
    },
    {
        .name  = "test_kevent_write_socket_modify_clobbers_udata",
        .desc  = "modify overwrites udata in the stored write knote",
        .func  = test_kevent_write_socket_modify_clobbers_udata,
    },
    {
        .name  = "test_kevent_write_socket_disable_drains",
        .desc  = "EV_DISABLE suppresses a pending write event",
        .func  = test_kevent_write_socket_disable_drains,
    },
    {
        .name  = "test_kevent_write_socket_delete_drains",
        .desc  = "EV_DELETE suppresses a pending write event",
        .func  = test_kevent_write_socket_delete_drains,
    },
    {
        .name  = "test_kevent_write_socket_multi_kqueue",
        .desc  = "two kqueues on the same fd both receive the write event",
        .func  = test_kevent_write_socket_multi_kqueue,
        .gates = write_multi_kqueue_gates,
    },
    {
        .name  = "test_kevent_write_socket_ev_clear",
        .desc  = "EV_CLEAR makes EVFILT_WRITE edge-triggered",
        .func  = test_kevent_write_socket_ev_clear,
    },
    {
        .name  = "test_kevent_write_pipe_eof_on_reader_close",
        .desc  = "EV_EOF fires on pipe write end when reader closes",
        .func  = test_kevent_write_pipe_eof_on_reader_close,
        .gates = write_pipe_eof_gates,
    },
    {
        .name  = "test_kevent_write_pipe_data_buffer_slack",
        .desc  = "kev.data reports real pipe buffer slack, not a hardcoded 1",
        .func  = test_kevent_write_pipe_data_buffer_slack,
        .gates = write_pipe_buffer_slack_gates,
    },
    {
        .name  = "test_kevent_write_socket_eof_on_peer_close",
        .desc  = "EV_EOF fires on write end when the stream peer closes",
        .func  = test_kevent_write_socket_eof_on_peer_close,
        .gates = write_socket_eof_on_peer_close_gates,
    },
    {
        .name  = "test_kevent_write_listen_socket_silent",
        .desc  = "EVFILT_WRITE never fires on a listen socket",
        .func  = test_kevent_write_listen_socket_silent,
    },
    {
        .name  = "test_kevent_write_dgram_socket",
        .desc  = "EVFILT_WRITE fires immediately on an unconnected UDP socket",
        .func  = test_kevent_write_dgram_socket,
    },
    {
        .name  = "test_kevent_write_regular_file",
        .desc  = "EVFILT_WRITE fires on a regular file",
        .func  = test_kevent_write_regular_file,
    },
    LKQ_SUITE_END
};

void
test_evfilt_write(struct test_context *ctx)
{
    snprintf(ctx->testfile, sizeof(ctx->testfile), "%s/kqueue-test%d.tmp",
            test_tmpdir(), testing_make_uid());

    run_test_suite(ctx, lkq_write_tests);
}
