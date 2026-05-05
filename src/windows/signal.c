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

#include "../common/private.h"
#include "../../include/kqueue/signal.h"

/*
 * Win32 has no kqueue-shaped EVFILT_SIGNAL.  We map the
 * asynchronous user-signal subset onto Win32 named auto-reset
 * events:
 *
 *   - The receiver (this filter) opens "Local\\libkqueue-sig-<pid>-<sig>"
 *     and parks RegisterWaitForSingleObject on it.
 *   - The sender (kq_raise / kq_kill, see <kqueue/signal.h>) opens
 *     the same name and SetEvents.
 *   - SetConsoleCtrlHandler is hooked at library init so external
 *     CTRL_C_EVENT / CTRL_BREAK_EVENT / CTRL_CLOSE_EVENT also signal
 *     the matching named events (SIGINT / SIGBREAK / SIGTERM).
 *
 * Auto-reset matches POSIX's "signals coalesce while pending":
 * N SetEvents before a Wait yield exactly one wake.  Multiple
 * watchers on the same (pid,sig) get independent kn_handles via
 * OpenEventA -> the named kernel object is shared, but each
 * RegisterWait callback fires on its own.
 *
 * Synchronous signals (SIGSEGV / SIGFPE / SIGBUS / SIGILL),
 * kernel-issued signals (SIGCHLD / SIGPIPE / SIGALRM), and the
 * unblockable kill semantics of POSIX SIGKILL are NOT covered:
 *   - synchronous CPU exceptions need vectored handlers, not events;
 *   - SIGCHLD callers should use EVFILT_PROC, SIGALRM EVFILT_TIMER,
 *     SIGPIPE the syscall errno;
 *   - SIGKILL via the sender shim falls through to TerminateProcess.
 */

static const char KQ_SIG_NAME_PREFIX[] = "Local\\libkqueue-sig-";

/* "Local\\libkqueue-sig-<pid>-<sig>" sized for max DWORD + max signum */
#define KQ_SIG_NAME_MAX (sizeof(KQ_SIG_NAME_PREFIX) + 11 + 1 + 4 + 1)

static int
kq_signal_format_name(char *out, size_t cap, DWORD pid, int signum)
{
    int n = snprintf(out, cap, "%s%lu-%d",
                     KQ_SIG_NAME_PREFIX, (unsigned long) pid, signum);
    if (n < 0 || (size_t) n >= cap) return -1;
    return 0;
}

static VOID CALLBACK
evfilt_signal_callback(void *param, BOOLEAN fired)
{
    struct knote *kn = (struct knote *) param;
    struct kqueue *kq;

    (void) fired;
    assert(kn);

    if (kn->kn_flags & KNFL_KNOTE_DELETED) {
        dbg_puts("signal knote marked for deletion, dropping wake");
        return;
    }

    /*
     * Hold a ref for the queued completion so an EV_DELETE
     * arriving after this point doesn't free the knote out from
     * under copyout's later dispatch.  Released in
     * evfilt_signal_copyout once the kev has been consumed.
     */
    knote_retain(kn);
    atomic_fetch_add(&kn->kn_fire_count, 1);

    kq = kn->kn_kq;
    assert(kq);
    if (!PostQueuedCompletionStatus(kq->kq_iocp, 1, KQ_FILTER_KEY(kn->kev.filter),
                                    (LPOVERLAPPED) kn)) {
        dbg_lasterror("PostQueuedCompletionStatus()");
        knote_release(kn);
        return;
    }
}

int
evfilt_signal_copyout(struct kevent *dst, UNUSED int nevents,
                      struct filter *filt, struct knote *src,
                      UNUSED void *ptr)
{
    int count;

    /*
     * EV_DELETE arrived after the callback queued a completion:
     * the knote is alive only because outstanding posts hold refs,
     * but the consumer's view of it is gone.  Drop the completion
     * and zero fire_count so a sibling post that races in behind
     * us doesn't expose a stale count to the next test (caught
     * test_kevent_signal_console_ctrl_bridge ghosting a SIGUSR2
     * data=2 left over from signal_coalesce's late callbacks).
     */
    if (src->kn_flags & KNFL_KNOTE_DELETED) {
        atomic_store(&src->kn_fire_count, 0);
        dst->filter = 0;
        knote_release(src);                /* balance callback retain */
        return 0;
    }

    count = atomic_exchange(&src->kn_fire_count, 0);

    /*
     * The wait callback bumps kn_fire_count BEFORE
     * PostQueuedCompletionStatus, so a callback that fired N
     * times posts N completions but exposes a single coalesced
     * count to the first dispatch.  Subsequent dispatches see
     * count==0 and are stale ghost completions - discard.
     */
    if (count == 0) {
        dst->filter = 0;
        knote_release(src);                /* balance callback retain */
        return 0;
    }

    memcpy(dst, &src->kev, sizeof(*dst));
    dst->data = count;

    if (knote_copyout_flag_actions(filt, src) < 0) {
        knote_release(src);
        return -1;
    }
    knote_release(src);                    /* balance callback retain */
    return 1;
}

int
evfilt_signal_knote_create(struct filter *filt, struct knote *kn)
{
    char   name[KQ_SIG_NAME_MAX];
    HANDLE evt;

    if (kn->kev.ident <= 0 || kn->kev.ident > 64) {
        dbg_printf("invalid signal number %lu",
                   (unsigned long) kn->kev.ident);
        errno = EINVAL;
        return -1;
    }
    if (kq_signal_format_name(name, sizeof(name),
                              GetCurrentProcessId(),
                              (int) kn->kev.ident) < 0)
        return -1;

    /*
     * Open the named auto-reset event, creating it if no one else
     * (sender or sibling watcher) has yet.  All watchers share one
     * named kernel object; each gets its own RegisterWait handle
     * via the wait callback closing over `kn`.
     */
    evt = CreateEventA(NULL, FALSE /*auto-reset*/, FALSE, name);
    if (evt == NULL) {
        dbg_lasterror("CreateEvent(signal)");
        return -1;
    }

    kn->kn_handle = evt;
    atomic_init(&kn->kn_fire_count, 0);

    if (RegisterWaitForSingleObject(&kn->kn_event_whandle, evt,
                                    evfilt_signal_callback, kn,
                                    INFINITE, 0) == 0) {
        dbg_lasterror("RegisterWaitForSingleObject(signal)");
        CloseHandle(evt);
        kn->kn_handle = NULL;
        return -1;
    }

    return 0;
}

int
evfilt_signal_knote_delete(UNUSED struct filter *filt, struct knote *kn)
{
    if (kn->kn_event_whandle != NULL) {
        if (!UnregisterWaitEx(kn->kn_event_whandle, INVALID_HANDLE_VALUE))
            dbg_lasterror("UnregisterWaitEx(signal)");
        kn->kn_event_whandle = NULL;
    }
    if (kn->kn_handle != NULL) {
        CloseHandle((HANDLE) kn->kn_handle);
        kn->kn_handle = NULL;
    }
    return 0;
}

int
evfilt_signal_knote_modify(struct filter *filt, struct knote *kn,
                           UNUSED const struct kevent *kev)
{
    /* Nothing to do beyond the common-layer kev.fflags/data merge. */
    (void) filt; (void) kn;
    return 0;
}

int
evfilt_signal_knote_enable(struct filter *filt, struct knote *kn)
{
    return evfilt_signal_knote_create(filt, kn);
}

int
evfilt_signal_knote_disable(struct filter *filt, struct knote *kn)
{
    return evfilt_signal_knote_delete(filt, kn);
}

const struct filter evfilt_signal = {
    .kf_id      = EVFILT_SIGNAL,
    .kf_copyout = evfilt_signal_copyout,
    .kn_create  = evfilt_signal_knote_create,
    .kn_modify  = evfilt_signal_knote_modify,
    .kn_delete  = evfilt_signal_knote_delete,
    .kn_enable  = evfilt_signal_knote_enable,
    .kn_disable = evfilt_signal_knote_disable,
};

/*
 * Console control handler bridge.  Installed once per process at
 * libkqueue_init time so external CTRL_C / CTRL_BREAK / window-
 * close / logoff / shutdown events fire the matching named signal
 * events, even when the originating code knew nothing about
 * libkqueue.
 */
static BOOL WINAPI
kq_signal_console_handler(DWORD ctrl)
{
    int sig;
    char   name[KQ_SIG_NAME_MAX];
    HANDLE h;

    switch (ctrl) {
    case CTRL_C_EVENT:
        sig = SIGINT;
        break;
    case CTRL_BREAK_EVENT:
        sig = SIGBREAK;
        break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sig = SIGTERM;
        break;
    default:
        return FALSE;
    }

    if (kq_signal_format_name(name, sizeof(name),
                              GetCurrentProcessId(), sig) < 0)
        return FALSE;
    /*
     * Use OpenEventA (not CreateEventA): if no kqueue watcher has
     * registered for this signum the named object doesn't exist,
     * and we want the standard runtime to handle the ctrl event
     * (default: terminate).  Returning FALSE chains to the next
     * handler, including the system default.
     */
    h = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (h == NULL) return FALSE;
    SetEvent(h);
    CloseHandle(h);
    return TRUE;
}

int
windows_signal_init(void)
{
    if (!SetConsoleCtrlHandler(kq_signal_console_handler, TRUE)) {
        dbg_lasterror("SetConsoleCtrlHandler");
        return -1;
    }
    return 0;
}

/*
 * Public sender shims: declared in <kqueue/signal.h>, implemented
 * here so internal users (e.g. tests) link against the same paths
 * external consumers do.
 */
int
kq_raise(int sig)
{
    return kq_kill((int) GetCurrentProcessId(), sig);
}

int
kq_kill(int pid, int sig)
{
    char   name[KQ_SIG_NAME_MAX];
    HANDLE h;

    /*
     * SIGKILL / SIGTERM map to TerminateProcess: the closest Win32
     * equivalent of "deliver this signal and end the process".
     * Loses the "let target handle gracefully" semantics POSIX has
     * for SIGTERM, but a co-operative listener can still bind
     * EVFILT_SIGNAL on SIGTERM in its own process - the shim
     * routes self-kill through the named event before falling
     * through to TerminateProcess.
     */
    if (kq_signal_format_name(name, sizeof(name),
                              (DWORD) pid, sig) < 0) {
        errno = EINVAL;
        return -1;
    }
    h = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (h != NULL) {
        SetEvent(h);
        CloseHandle(h);
    }

    if (sig == SIGKILL ||
#ifdef SIGTERM
        (sig == SIGTERM && (DWORD) pid != GetCurrentProcessId()) ||
#endif
        0)
    {
        HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD) pid);
        if (proc == NULL) {
            errno = ESRCH;
            return -1;
        }
        TerminateProcess(proc, 128 + sig);
        CloseHandle(proc);
    }

    /*
     * No listener and not a process-killing signal: indicate
     * "no such target", matching POSIX kill() returning -1 ESRCH
     * when the destination process can't be reached.
     */
    if (h == NULL && sig != SIGKILL) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}
