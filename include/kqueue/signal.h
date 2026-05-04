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
 * libkqueue signal-shim API.
 *
 * On POSIX, EVFILT_SIGNAL observes signals delivered through the
 * usual kill()/raise() machinery; you don't need libkqueue's help
 * to send them.  On Win32 there is no kqueue-shaped signal model,
 * so libkqueue carries asynchronous user signals on top of named
 * Win32 events: the sender SetEvents the named object, the
 * receiver's RegisterWaitForSingleObject callback fires.  These
 * shims are the portable entry points; they're a thin wrapper over
 * raise() / kill() on POSIX so cross-platform code can use the
 * same names.
 *
 * Caveats on Windows:
 *   - SIGKILL routes through TerminateProcess on the target.
 *   - Cross-process SIGTERM also TerminateProcess's the target;
 *     in-process SIGTERM stays cooperative (NamedEvent only).
 *   - Synchronous signals (SEGV/FPE/BUS/ILL) and kernel-issued
 *     signals (CHLD/PIPE/ALRM) are not bridged - use the
 *     corresponding kqueue filter (EVFILT_PROC / EVFILT_TIMER /
 *     errno from the syscall) instead.
 *   - External Win32 code that knows nothing about libkqueue won't
 *     reach the named events.  CTRL_C / CTRL_BREAK / window-close
 *     / logoff / shutdown ARE bridged through SetConsoleCtrlHandler
 *     installed at libkqueue init.
 */

#ifndef _KQUEUE_SIGNAL_H_
#define _KQUEUE_SIGNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

#include <signal.h>  /* MSVC defines SIGINT, SIGTERM, SIGSEGV, etc. */

/*
 * MSVC's <signal.h> ships a small subset.  The kqueue named-event
 * tunnel just needs a stable integer the sender and receiver agree
 * on, so define the missing POSIX values here using their
 * Linux/glibc numbers.  SIGBREAK=21 already matches MSVC.
 */
#ifndef SIGBREAK
#define SIGBREAK 21
#endif
#ifndef SIGHUP
#define SIGHUP   1
#endif
#ifndef SIGKILL
#define SIGKILL  9
#endif
#ifndef SIGUSR1
#define SIGUSR1  10
#endif
#ifndef SIGUSR2
#define SIGUSR2  12
#endif
#ifndef SIGPIPE
#define SIGPIPE  13
#endif
#ifndef SIGALRM
#define SIGALRM  14
#endif

/* Send `sig` to process `pid`.  See file header for caveats. */
__declspec(dllexport) int kq_kill(int pid, int sig);

/* Send `sig` to the calling process. */
__declspec(dllexport) int kq_raise(int sig);

#else  /* !_WIN32 */

#include <signal.h>
#include <sys/types.h>

static inline int kq_kill(int pid, int sig)  { return kill((pid_t) pid, sig); }
static inline int kq_raise(int sig)          { return raise(sig); }

#endif /* !_WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* _KQUEUE_SIGNAL_H_ */
