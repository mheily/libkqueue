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
 * Tiny POSIX-flavour shim for the parts of <pthread.h> and
 * <semaphore.h> the libkqueue test suite uses.  Backed by Win32
 * threads + semaphores; not a complete pthreads / sysv-sem
 * emulation.  Header-only, included from test/common.h on _WIN32.
 */

#ifndef _KQUEUE_TEST_WIN32_COMPAT_H
#define _KQUEUE_TEST_WIN32_COMPAT_H

#include <process.h>

/*
 * pthread_t on Win32 is the HANDLE returned by _beginthreadex.
 * The test code stores it in a `pthread_t` and feeds it to
 * pthread_join/pthread_cancel; nothing else dereferences it.
 */
typedef HANDLE pthread_t;

typedef struct {
    int dummy;
} pthread_attr_t;

/*
 * Bridge the (start_routine: void *(*)(void *)) signature the
 * tests use over to _beginthreadex's (unsigned __stdcall (*)(void *))
 * via a small trampoline allocated per-create.  The trampoline
 * stashes the return value where the join can fish it out.
 */
struct _kq_thread_arg {
    void *(*start)(void *);
    void *arg;
    void *retval;
};

static unsigned __stdcall
_kq_thread_trampoline(void *raw)
{
    struct _kq_thread_arg *ta = (struct _kq_thread_arg *)raw;
    ta->start(ta->arg);
    free(ta);
    return 0;
}

static __inline int
pthread_create(pthread_t *th, pthread_attr_t *attr,
               void *(*start)(void *), void *arg)
{
    struct _kq_thread_arg *ta;
    uintptr_t h;

    (void) attr;
    ta = (struct _kq_thread_arg *) malloc(sizeof(*ta));
    if (ta == NULL) return -1;
    ta->start = start;
    ta->arg = arg;
    ta->retval = NULL;

    h = _beginthreadex(NULL, 0, _kq_thread_trampoline, ta, 0, NULL);
    if (h == 0) {
        free(ta);
        return -1;
    }
    /*
     * Leak the trampoline cookie: pthread_join can't get at it
     * cleanly without a side table, and the test suite never
     * inspects retval.  These threads are short-lived and the
     * test process exits soon after, so the leak is bounded.
     */
    *th = (HANDLE) h;
    return 0;
}

static __inline int
pthread_join(pthread_t th, void **retval)
{
    if (WaitForSingleObject(th, INFINITE) != WAIT_OBJECT_0)
        return -1;
    if (retval) *retval = NULL;
    CloseHandle(th);
    return 0;
}

/*
 * Best-effort cancel: TerminateThread is brutal but sufficient
 * for the test paths that use pthread_cancel (mostly
 * "tear down a stuck waiter and move on").  Don't model
 * cancellation points or cleanup handlers.
 */
static __inline int
pthread_cancel(pthread_t th)
{
    if (!TerminateThread(th, 0)) return -1;
    return 0;
}

/*
 * POSIX semaphore shim backed by Win32 named semaphores.
 * sem_open with O_CREAT|O_EXCL maps onto CreateSemaphoreA; we
 * bound the count at LONG_MAX and rely on the test suite to
 * release fewer times than that.
 */
typedef HANDLE sem_t;
#define SEM_FAILED ((sem_t *) NULL)

static __inline sem_t *
sem_open(const char *name, int oflag, ...)
{
    sem_t s;
    sem_t *out;
    LONG initial = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        (void) va_arg(ap, int); /* mode */
        initial = (LONG) va_arg(ap, unsigned int);
        va_end(ap);
    }
    s = CreateSemaphoreA(NULL, initial, 0x7fffffff, name);
    if (s == NULL) return SEM_FAILED;
    if ((oflag & O_EXCL) && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(s);
        return SEM_FAILED;
    }
    out = (sem_t *) malloc(sizeof(*out));
    if (out == NULL) { CloseHandle(s); return SEM_FAILED; }
    *out = s;
    return out;
}

static __inline int
sem_close(sem_t *s)
{
    if (s == NULL) return -1;
    CloseHandle(*s);
    free(s);
    return 0;
}

/* Win32 named semaphores are reaped on last-handle close; sem_unlink is
 * effectively a noop here.  Returning 0 is what the tests assert. */
static __inline int
sem_unlink(const char *name) { (void) name; return 0; }

static __inline int
sem_wait(sem_t *s)
{
    if (WaitForSingleObject(*s, INFINITE) != WAIT_OBJECT_0) return -1;
    return 0;
}

static __inline int
sem_post(sem_t *s)
{
    if (!ReleaseSemaphore(*s, 1, NULL)) return -1;
    return 0;
}

/*
 * sem_trywait: poll the semaphore with a 0ms timeout.  Returns 0
 * if acquired, -1 with errno=EAGAIN if not (POSIX semantics).
 */
static __inline int
sem_trywait(sem_t *s)
{
    DWORD r = WaitForSingleObject(*s, 0);
    if (r == WAIT_OBJECT_0) return 0;
    errno = EAGAIN;
    return -1;
}

/*
 * nanosleep: Sleep is millisecond-granular, so round up.  Tests
 * use this for "wait at least N ms"; over-sleeping is harmless.
 */
static __inline int
nanosleep(const struct timespec *req, struct timespec *rem)
{
    DWORD ms;
    (void) rem;
    if (req == NULL) return -1;
    ms = (DWORD)(req->tv_sec * 1000) +
         (DWORD)((req->tv_nsec + 999999) / 1000000);
    if (ms == 0) ms = 1;
    Sleep(ms);
    return 0;
}

#include <stdarg.h>
#include <stdlib.h>

/*
 * clock_gettime shim, enough to satisfy the timer/threading tests
 * that need a CLOCK_REALTIME / CLOCK_MONOTONIC source.  Win32 has
 * native equivalents - GetSystemTimePreciseAsFileTime maps the
 * Windows FILETIME (100ns ticks since 1601-01-01) to Unix epoch
 * (1970-01-01); QueryPerformanceCounter gives a monotonic high-res
 * counter unaffected by clock adjustments.
 */
#ifndef CLOCK_REALTIME
# define CLOCK_REALTIME  0
#endif
#ifndef CLOCK_MONOTONIC
# define CLOCK_MONOTONIC 1
#endif

typedef int clockid_t;

static __inline int
clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME) {
        FILETIME       ft;
        ULARGE_INTEGER li;
        /* 100ns ticks between 1601-01-01 and 1970-01-01. */
        const ULONGLONG epoch_offset = 116444736000000000ULL;

        GetSystemTimePreciseAsFileTime(&ft);
        li.LowPart  = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        li.QuadPart -= epoch_offset;

        ts->tv_sec  = (time_t)(li.QuadPart / 10000000ULL);
        ts->tv_nsec = (long)((li.QuadPart % 10000000ULL) * 100);
        return 0;
    }
    if (id == CLOCK_MONOTONIC) {
        LARGE_INTEGER counter, freq;
        if (!QueryPerformanceFrequency(&freq)) return -1;
        if (!QueryPerformanceCounter(&counter)) return -1;
        ts->tv_sec  = (time_t)(counter.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((counter.QuadPart % freq.QuadPart) *
                               1000000000LL) / freq.QuadPart);
        return 0;
    }
    return -1;
}

/*
 * pipe() shim: anonymous _pipe creates handles that don't support
 * overlapped I/O, so they can't be attached to an IOCP - which the
 * libkqueue read filter requires for pipe HANDLEs.  Instead make a
 * unique named pipe with FILE_FLAG_OVERLAPPED on the server (read)
 * side and CreateFileA the client (write) side, then adopt both as
 * CRT fds via _open_osfhandle.  Per-call counter + getpid() keeps
 * the name unique within the test process.
 */
static __inline int
pipe(int fds[2])
{
    static volatile LONG counter = 0;
    char        name[64];
    HANDLE      rd, wr;
    LONG        n = InterlockedIncrement(&counter);

    snprintf(name, sizeof(name),
             "\\\\.\\pipe\\libkqueue-test-%lu-%ld",
             (unsigned long) GetCurrentProcessId(), (long) n);

    rd = CreateNamedPipeA(name,
                          PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                              FILE_FLAG_FIRST_PIPE_INSTANCE,
                          PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                          1, 4096, 4096, 0, NULL);
    if (rd == INVALID_HANDLE_VALUE) return -1;

    wr = CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                     0, NULL);
    if (wr == INVALID_HANDLE_VALUE) {
        CloseHandle(rd);
        return -1;
    }

    fds[0] = _open_osfhandle((intptr_t) rd, _O_BINARY);
    fds[1] = _open_osfhandle((intptr_t) wr, _O_BINARY);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) _close(fds[0]); else CloseHandle(rd);
        if (fds[1] >= 0) _close(fds[1]); else CloseHandle(wr);
        return -1;
    }
    return 0;
}

/*
 * Win32 doesn't have /tmp.  Resolve via the standard TEMP env-var
 * chain (GetTempPath does TMP -> TEMP -> USERPROFILE -> WinDir),
 * then build "<temp>\<tag>.<pid>" so concurrent CI shards don't
 * stomp on each other.  POSIX consumers stick with literal /tmp.
 */
static __inline int
kq_test_temp_path(char *out, size_t cap, const char *tag)
{
    char dir[MAX_PATH];
    DWORD n = GetTempPathA((DWORD) sizeof(dir), dir);
    if (n == 0 || n > sizeof(dir)) return -1;
    /* Trim a trailing backslash so the printf format is uniform. */
    if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[n - 1] = '\0';
    return snprintf(out, cap, "%s\\%s.%lu",
                    dir, tag, (unsigned long) GetCurrentProcessId());
}

/*
 * MSVC io.h ships unlink() / unlink-like deprecation aliases for
 * legacy POSIX names; existing vnode.c calls to unlink(testfile)
 * compile cleanly on the Windows build, so we don't redefine it
 * here.
 */

/*
 * fcntl(F_GETFL) / fcntl(F_SETFL, O_NONBLOCK) shim for the
 * test_kevent_write_*  send-buffer-fill helper in write.c.
 * The test only uses fcntl to flip a connected socket between
 * blocking and non-blocking; map onto ioctlsocket(FIONBIO).  Any
 * other (cmd, fd) shape is rejected with EINVAL.
 */
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0x4000
#endif
#ifndef F_GETFL
#  define F_GETFL 3
#endif
#ifndef F_SETFL
#  define F_SETFL 4
#endif

static __inline int
fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int     flags;
    u_long  enable;

    switch (cmd) {
    case F_GETFL:
        /* Win32 has no per-fd flag bag for sockets; pretend zero
         * and let callers OR in O_NONBLOCK as a no-op no-prior-flags
         * starting point. */
        return 0;
    case F_SETFL:
        va_start(ap, cmd);
        flags = va_arg(ap, int);
        va_end(ap);
        enable = (flags & O_NONBLOCK) ? 1 : 0;
        if (ioctlsocket((SOCKET) fd, FIONBIO, &enable) != 0)
            return -1;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

/*
 * mode_t / umask / mkstemp shims for the regular-file tests in
 * read.c.  MSVC doesn't ship mode_t (the CRT uses bare ints) and
 * doesn't ship mkstemp (Win32 has _mktemp_s + a separate _open).
 * The tests don't lean on the bit-pattern of mode_t for anything
 * meaningful - they call umask(077) / restore - so int suffices.
 */
typedef int mode_t;

static __inline mode_t
umask(mode_t mask)
{
    int prev;
    /*
     * MSVC's _umask_s sets *prev to the prior mask and applies
     * the new one.  POSIX umask(2) returns the prior value.
     */
    if (_umask_s((int) mask, &prev) != 0) return 0;
    return (mode_t) prev;
}

/*
 * mkstemp: replace the "XXXXXX" suffix in `template` in-place with
 * a unique name and open a fresh O_RDWR file.  _mktemp_s does the
 * substitution; _open does the create with the same fd-table-aware
 * semantics CRT uses elsewhere.
 */
static __inline int
mkstemp(char *template_)
{
    if (_mktemp_s(template_, strlen(template_) + 1) != 0)
        return -1;
    return _open(template_, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
                 _S_IREAD | _S_IWRITE);
}

#endif  /* ! _KQUEUE_TEST_WIN32_COMPAT_H */
