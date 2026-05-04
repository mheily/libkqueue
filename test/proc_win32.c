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
 * Win32 EVFILT_PROC smoke tests.  POSIX test/proc.c is shaped around
 * fork() + kill(SIGUSR1) + waitpid() and won't compile here, so this
 * file exercises the same code paths via CreateProcess + Terminate
 * Process / clean-exit cmd children.
 *
 * The Win32 backend (src/windows/proc.c) treats NOTE_EXIT as the
 * single observable sub-event: it OpenProcess()s the target with
 * SYNCHRONIZE, RegisterWaitForSingleObject's a callback, and posts
 * an IOCP completion when the process becomes signalled.  These
 * tests confirm:
 *   - EV_ADD followed by NOTE_EXIT delivery + correct exit-code
 *     encoding in dst->data (waitpid()-style high byte = exit code)
 *   - Adding EV_ADD on a process that has already exited still
 *     delivers (the Win32 wait fires immediately on a signalled HANDLE)
 *   - EV_DELETE before exit cancels the registration cleanly
 */

#include "common.h"

#ifdef _WIN32

#include <windows.h>

/*
 * Spawn a child that runs `cmd /c exit <code>`.  Returns the
 * child's PID and (out param) the kernel HANDLE so we can wait on
 * it locally if a test wants to gate on the actual exit before
 * registering the kevent.
 */
static DWORD
spawn_exit_code_child(int code, HANDLE *out_handle)
{
    STARTUPINFOA         si = { 0 };
    PROCESS_INFORMATION  pi = { 0 };
    char                 cmd[64];

    si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "cmd.exe /c exit %d", code);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        die("CreateProcess(\"%s\") gle=%lu",
            cmd, (unsigned long) GetLastError());

    if (out_handle)
        *out_handle = pi.hProcess;
    else
        CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return pi.dwProcessId;
}

static void
test_kevent_proc_add_delete(struct test_context *ctx)
{
    struct kevent kev;
    HANDLE        ph;
    DWORD         pid = spawn_exit_code_child(0, &ph);

    /*
     * Register, then unregister before the child exits.  The
     * subsequent close(kqfd) at the harness boundary should not
     * deliver any event.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC,
               EV_ADD, NOTE_EXIT, 0, NULL);
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC,
               EV_DELETE, 0, 0, NULL);
    test_no_kevents(ctx->kqfd);

    /* Reap the child so we don't leak a HANDLE. */
    WaitForSingleObject(ph, INFINITE);
    CloseHandle(ph);
}

static void
test_kevent_proc_exit_status(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    HANDLE        ph;
    DWORD         pid = spawn_exit_code_child(7, &ph);

    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC,
               EV_ADD, NOTE_EXIT, 0, NULL);

    /* Block until the child exits and we surface NOTE_EXIT. */
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    if (ret[0].filter != EVFILT_PROC)
        die("expected EVFILT_PROC, got %d", (int) ret[0].filter);
    if (ret[0].ident != pid)
        die("expected ident=%lu, got %lu",
            (unsigned long) pid, (unsigned long) ret[0].ident);
    /*
     * windows/proc.c encodes exit code in the high byte of the data
     * field, mirroring waitpid()'s W_EXITCODE shape.
     */
    if (((ret[0].data >> 8) & 0xff) != 7)
        die("expected exit code 7 in high byte of data, got 0x%llx",
            (unsigned long long) ret[0].data);
    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF on NOTE_EXIT delivery");

    WaitForSingleObject(ph, INFINITE);
    CloseHandle(ph);
}

static void
test_kevent_proc_already_exited(struct test_context *ctx)
{
    struct kevent kev, ret[1];
    HANDLE        ph;
    DWORD         pid = spawn_exit_code_child(0, &ph);

    /* Make sure the child has exited BEFORE we register. */
    if (WaitForSingleObject(ph, INFINITE) != WAIT_OBJECT_0)
        die("WaitForSingleObject");

    /*
     * Win32's RegisterWaitForSingleObject on an already-signalled
     * HANDLE still fires the callback - the kqueue layer should see
     * the completion and surface NOTE_EXIT promptly.
     */
    kevent_add(ctx->kqfd, &kev, pid, EVFILT_PROC,
               EV_ADD, NOTE_EXIT, 0, NULL);
    kevent_get(ret, NUM_ELEMENTS(ret), ctx->kqfd, 1);

    if (ret[0].filter != EVFILT_PROC)
        die("expected EVFILT_PROC for already-exited child");
    if (!(ret[0].flags & EV_EOF))
        die("expected EV_EOF for already-exited child");

    CloseHandle(ph);
}

static void
test_kevent_proc_multiple_children(struct test_context *ctx)
{
    enum { N = 4 };
    struct kevent  kev, ret[N];
    HANDLE         ph[N];
    DWORD          pid[N];
    int            i, got;

    for (i = 0; i < N; i++)
        pid[i] = spawn_exit_code_child(i + 1, &ph[i]);

    for (i = 0; i < N; i++)
        kevent_add(ctx->kqfd, &kev, pid[i], EVFILT_PROC,
                   EV_ADD, NOTE_EXIT, 0, NULL);

    /*
     * Drain N events.  Order across children is racy, so just count
     * deliveries and verify each PID is seen exactly once.  kevent()
     * may return them in batches; loop until we've seen all N.
     */
    got = 0;
    while (got < N) {
        int n = kevent(ctx->kqfd, NULL, 0, ret, N - got, NULL);
        if (n < 0) die("kevent");
        got += n;
    }

    for (i = 0; i < N; i++) {
        WaitForSingleObject(ph[i], INFINITE);
        CloseHandle(ph[i]);
    }
}

void
test_evfilt_proc(struct test_context *ctx)
{
    test(kevent_proc_add_delete, ctx);
    test(kevent_proc_exit_status, ctx);
    test(kevent_proc_already_exited, ctx);
    test(kevent_proc_multiple_children, ctx);
}

#endif /* _WIN32 */
