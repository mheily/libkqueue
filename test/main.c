/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

#include <limits.h>

#ifndef _WIN32
#include <getopt.h>
#else
#include <crtdbg.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__sun) || defined(__APPLE__)
#include <sys/time.h>
#include <sys/resource.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "common.h"

#ifdef _WIN32
static void test_iph_noop(const wchar_t *expr, const wchar_t *func,
                          const wchar_t *file, unsigned int line,
                          uintptr_t reserved)
{
    (void) expr; (void) func; (void) file; (void) line; (void) reserved;
}
#endif

/*
 * Watchdog: a tiny child process that fires a stack dump and
 * aborts the parent if no test sends a heartbeat within
 * --watchdog-timeout.
 *
 * Heartbeat transport is a pipe: parent writes "<test_name>\n" before
 * each test; the watchdog poll(2)s the read end with the timeout as
 * its deadline.  poll returning 0 means the deadline expired with no
 * data == bark; any data == reset and poll again.  No polling tick,
 * no shared memory, deterministic fire time at exactly the timeout.
 *
 * Disabled until --watchdog-timeout is given on the command line.
 * The dump command is set with --watchdog-cmd; the parent pid is
 * appended as the final argv entry.  Run via execvp(3): bare names
 * resolve via $PATH, paths with a slash are taken literally.
 * Examples:
 *   --watchdog-cmd=pstack
 *   --watchdog-cmd='/usr/bin/eu-stack -p'
 */
#ifndef _WIN32
# include <poll.h>
# include <signal.h>
# include <sys/wait.h>

#define WATCHDOG_TEST_NAME_MAX 64
#define WATCHDOG_CMD_ARGS_MAX  16
#define WATCHDOG_CMD_BUF       256

static int    watchdog_timeout_s     = 0; /* 0 = disabled */
static int    watchdog_pipe[2]       = { -1, -1 };
static pid_t  watchdog_pid           = -1;
static char   watchdog_cmd_buf[WATCHDOG_CMD_BUF];
static char  *watchdog_cmd_argv[WATCHDOG_CMD_ARGS_MAX];

static void
watchdog_parse_cmd(const char *cmd)
{
    size_t n = strlen(cmd);
    if (n >= sizeof(watchdog_cmd_buf)) n = sizeof(watchdog_cmd_buf) - 1;
    memcpy(watchdog_cmd_buf, cmd, n);
    watchdog_cmd_buf[n] = '\0';

    int i = 0;
    char *tok = strtok(watchdog_cmd_buf, " \t");
    while (tok && i < WATCHDOG_CMD_ARGS_MAX - 2) {
        watchdog_cmd_argv[i++] = tok;
        tok = strtok(NULL, " \t");
    }
    watchdog_cmd_argv[i] = NULL;
}

/* Async-signal-safe int-to-decimal, returns bytes written. */
static int
itoa_safe(char *buf, size_t cap, long v)
{
    char tmp[32];
    int n = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long) -v : (unsigned long) v;
    int i;

    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u && n < (int) sizeof(tmp));
    if (neg && n < (int) sizeof(tmp)) tmp[n++] = '-';
    if ((size_t) n > cap) n = (int) cap;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void
watchdog_loop(pid_t parent, int rfd)
{
    char          pidbuf[16];
    int           pidlen;
    char          last_name[WATCHDOG_TEST_NAME_MAX];
    size_t        last_len = 0;
    char          rdbuf[256];
    struct pollfd pfd = { .fd = rfd, .events = POLLIN };

    pidlen = itoa_safe(pidbuf, sizeof(pidbuf) - 1, (long) parent);
    pidbuf[pidlen] = '\0';
    last_name[0]   = '\0';

    /* Append pid to the dump command as its final argv entry. */
    int argc = 0;
    while (watchdog_cmd_argv[argc] != NULL) argc++;
    watchdog_cmd_argv[argc]     = pidbuf;
    watchdog_cmd_argv[argc + 1] = NULL;

    for (;;) {
        int rv = poll(&pfd, 1, watchdog_timeout_s * 1000);
        if (rv < 0) {
            if (errno == EINTR) continue;
            _exit(125);
        }
        if (rv == 0) break;  /* Timeout - bark. */

        ssize_t n = read(rfd, rdbuf, sizeof(rdbuf));
        if (n <= 0) _exit(0);  /* Parent closed the pipe == clean exit. */

        /* Capture the last newline-terminated record as the current
         * test name.  Multiple heartbeats may have batched into one
         * read; we only care about the most recent. */
        ssize_t last_nl = -1;
        for (ssize_t i = n - 1; i >= 0; i--)
            if (rdbuf[i] == '\n') { last_nl = i; break; }
        if (last_nl > 0) {
            ssize_t start = last_nl - 1;
            while (start >= 0 && rdbuf[start] != '\n') start--;
            start++;
            size_t len = (size_t)(last_nl - start);
            if (len >= sizeof(last_name)) len = sizeof(last_name) - 1;
            memcpy(last_name, rdbuf + start, len);
            last_name[len] = '\0';
            last_len = len;
        }
    }

    /* Bark. */
    const char *prefix = "\n=== test watchdog: timeout in ";
    write(STDERR_FILENO, prefix, strlen(prefix));
    if (last_len) write(STDERR_FILENO, last_name, last_len);
    else          write(STDERR_FILENO, "(unknown)", 9);
    write(STDERR_FILENO, " (pid ", 6);
    write(STDERR_FILENO, pidbuf, pidlen);
    write(STDERR_FILENO, "), dumping stacks ===\n", 22);

    /* Log the resolved argv so a reader can see exactly what we
     * tried to run.  Useful when the dump command itself misbehaves. */
    write(STDERR_FILENO, "watchdog: exec:", 15);
    for (int a = 0; watchdog_cmd_argv[a] != NULL; a++) {
        write(STDERR_FILENO, " ", 1);
        write(STDERR_FILENO, watchdog_cmd_argv[a], strlen(watchdog_cmd_argv[a]));
    }
    write(STDERR_FILENO, "\n", 1);

    pid_t child = fork();
    if (child == 0) {
        execvp(watchdog_cmd_argv[0], watchdog_cmd_argv);
        const char *fail = "watchdog: exec failed\n";
        write(STDERR_FILENO, fail, strlen(fail));
        _exit(127);
    }
    if (child > 0) {
        /* Don't wait forever for the dump.  pstack/gdb/vgdb can hang
         * attaching to a wedged inferior; the parent kill is what
         * actually matters here, so cap the dump at 30s and SIGKILL
         * the dumper if it overruns. */
        const int dump_timeout_s = 30;
        int       waited = 0;
        for (;;) {
            pid_t r = waitpid(child, NULL, WNOHANG);
            if (r == child || r < 0) break;
            if (waited >= dump_timeout_s) {
                write(STDERR_FILENO,
                      "watchdog: dump command exceeded 30s, killing\n", 45);
                kill_or_die(child, SIGKILL);
                waitpid(child, NULL, 0);
                break;
            }
            sleep(1);
            waited++;
        }
    }

    const char *abrt = "=== test watchdog: killing parent ===\n";
    write(STDERR_FILENO, abrt, strlen(abrt));
    kill_or_die(parent, SIGKILL);
    _exit(124);
}

static void
watchdog_start(void)
{
    if (watchdog_timeout_s <= 0)
        return;
    if (watchdog_cmd_argv[0] == NULL) {
        fprintf(stderr, "--watchdog-timeout requires --watchdog-cmd\n");
        return;
    }

    if (pipe(watchdog_pipe) < 0) {
        perror("pipe(watchdog)");
        return;
    }

    pid_t parent = getpid();
    pid_t pid    = fork();
    if (pid < 0) {
        perror("fork(watchdog)");
        close(watchdog_pipe[0]);
        close(watchdog_pipe[1]);
        watchdog_pipe[0] = watchdog_pipe[1] = -1;
        return;
    }
    if (pid == 0) {
        close(watchdog_pipe[1]);  /* Child reads only. */
        watchdog_loop(parent, watchdog_pipe[0]);
        _exit(0);
    }
    /* Parent writes only. */
    close(watchdog_pipe[0]);
    watchdog_pipe[0] = -1;
    watchdog_pid = pid;

    printf("watchdog active (timeout %ds, cmd '%s', pid %d)\n",
           watchdog_timeout_s, watchdog_cmd_argv[0], (int) pid);
}

void
watchdog_heartbeat(const char *test_name)
{
    /* No-op when the watchdog wasn't started.  Callers always pass
     * a non-NULL ut_name from the registered test table. */
    if (watchdog_pipe[1] < 0) return;

    /* "<name>\n" in one write so the watchdog reads a complete record.
     * Writes <= PIPE_BUF (4096) are atomic per POSIX; we cap the name
     * well below that. */
    char    buf[WATCHDOG_TEST_NAME_MAX + 1];
    size_t  n = strlen(test_name);
    if (n >= WATCHDOG_TEST_NAME_MAX) n = WATCHDOG_TEST_NAME_MAX - 1;
    memcpy(buf, test_name, n);
    buf[n++] = '\n';

    ssize_t w = write(watchdog_pipe[1], buf, n);
    (void) w;
}

static void
watchdog_stop(void)
{
    if (watchdog_pid > 0) {
        /* Murderise it - we want it gone now, not waiting on its
         * next poll deadline. */
        kill_or_die(watchdog_pid, SIGKILL);
        waitpid(watchdog_pid, NULL, 0);
        watchdog_pid = -1;
    }
    if (watchdog_pipe[1] >= 0) {
        close(watchdog_pipe[1]);
        watchdog_pipe[1] = -1;
    }
}
#else  /* _WIN32 */

#define WATCHDOG_TEST_NAME_MAX 64
#define WATCHDOG_CMD_BUF       512

static int              watchdog_timeout_s   = 0;
static char             watchdog_cmd_buf[WATCHDOG_CMD_BUF];
static HANDLE           watchdog_event       = NULL;  /* auto-reset, signalled by heartbeat */
static HANDLE           watchdog_stop_event  = NULL;  /* manual-reset, signalled by stop() */
static HANDLE           watchdog_thread      = NULL;
static CRITICAL_SECTION watchdog_lock;
static char             watchdog_last_name[WATCHDOG_TEST_NAME_MAX];

static void
watchdog_parse_cmd(const char *cmd)
{
    size_t n = strlen(cmd);
    if (n >= sizeof(watchdog_cmd_buf)) n = sizeof(watchdog_cmd_buf) - 1;
    memcpy(watchdog_cmd_buf, cmd, n);
    watchdog_cmd_buf[n] = '\0';
}

/*
 * Windows watchdog thread: parks on watchdog_event for the timeout.
 * - WAIT_OBJECT_0: heartbeat from main, reset and wait again.
 * - WAIT_TIMEOUT:  test wedged; spawn the dump cmd, then ExitProcess.
 *
 * watchdog_stop_event lets watchdog_stop() interrupt the wait
 * cleanly when the suite finishes within budget.
 */
static DWORD WINAPI
watchdog_thread_proc(LPVOID unused)
{
    HANDLE waits[2] = { watchdog_event, watchdog_stop_event };
    char   pidbuf[32];
    DWORD  pid = GetCurrentProcessId();

    (void) unused;
    snprintf(pidbuf, sizeof(pidbuf), "%lu", (unsigned long) pid);

    for (;;) {
        DWORD rv = WaitForMultipleObjects(2, waits, FALSE,
                                          (DWORD) watchdog_timeout_s * 1000);
        if (rv == WAIT_OBJECT_0)        continue;     /* heartbeat */
        if (rv == WAIT_OBJECT_0 + 1)    return 0;     /* stop */
        if (rv != WAIT_TIMEOUT) {
            fprintf(stderr, "watchdog: WaitForMultipleObjects failed gle=%lu\n",
                    (unsigned long) GetLastError());
            return 1;
        }

        /* Bark. */
        EnterCriticalSection(&watchdog_lock);
        fprintf(stderr,
                "\n=== test watchdog: timeout in %s (pid %s), "
                "running dump command ===\n",
                watchdog_last_name[0] ? watchdog_last_name : "(unknown)",
                pidbuf);
        LeaveCriticalSection(&watchdog_lock);
        fflush(stderr);

        if (watchdog_cmd_buf[0]) {
            /*
             * Build the cmdline: "<watchdog_cmd> <pid>".  The dump
             * tool (cdb -pv -c '~*kb;qd' -p, procdump -ma, etc.)
             * sees the parent pid as its final argv entry, matching
             * the POSIX watchdog convention.
             */
            char  cmdline[WATCHDOG_CMD_BUF + 64];
            STARTUPINFOA si = { 0 };
            PROCESS_INFORMATION pi = { 0 };
            si.cb         = sizeof(si);
            si.dwFlags    = STARTF_USESTDHANDLES;
            si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
            si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

            snprintf(cmdline, sizeof(cmdline), "%s %s",
                     watchdog_cmd_buf, pidbuf);
            fprintf(stderr, "watchdog: spawning: %s\n", cmdline);
            fflush(stderr);

            if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                               CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                /*
                 * Cap the dump at 30s; cdb / procdump occasionally
                 * hang attaching to a deeply-wedged process.  After
                 * that, walk away and kill ourselves anyway.
                 */
                if (WaitForSingleObject(pi.hProcess, 30 * 1000) == WAIT_TIMEOUT) {
                    fprintf(stderr,
                            "watchdog: dump command exceeded 30s, killing\n");
                    fflush(stderr);
                    TerminateProcess(pi.hProcess, 1);
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                fprintf(stderr, "watchdog: CreateProcess failed gle=%lu\n",
                        (unsigned long) GetLastError());
                fflush(stderr);
            }
        }

        fprintf(stderr, "=== test watchdog: aborting parent ===\n");
        fflush(stderr);
        ExitProcess(124);
    }
}

static void
watchdog_start(void)
{
    if (watchdog_timeout_s <= 0)
        return;
    if (watchdog_cmd_buf[0] == '\0') {
        fprintf(stderr, "--watchdog-timeout requires --watchdog-cmd\n");
        return;
    }

    InitializeCriticalSection(&watchdog_lock);
    watchdog_event      = CreateEventA(NULL, FALSE, FALSE, NULL);
    watchdog_stop_event = CreateEventA(NULL, TRUE,  FALSE, NULL);
    if (!watchdog_event || !watchdog_stop_event) {
        fprintf(stderr, "watchdog: CreateEvent failed gle=%lu\n",
                (unsigned long) GetLastError());
        return;
    }
    watchdog_thread = CreateThread(NULL, 0, watchdog_thread_proc, NULL,
                                   0, NULL);
    if (!watchdog_thread) {
        fprintf(stderr, "watchdog: CreateThread failed gle=%lu\n",
                (unsigned long) GetLastError());
        return;
    }

    printf("watchdog active (timeout %ds, cmd '%s', pid %lu)\n",
           watchdog_timeout_s, watchdog_cmd_buf,
           (unsigned long) GetCurrentProcessId());
}

void
watchdog_heartbeat(const char *test_name)
{
    if (watchdog_event == NULL) return;

    EnterCriticalSection(&watchdog_lock);
    {
        size_t n = strlen(test_name);
        if (n >= sizeof(watchdog_last_name))
            n = sizeof(watchdog_last_name) - 1;
        memcpy(watchdog_last_name, test_name, n);
        watchdog_last_name[n] = '\0';
    }
    LeaveCriticalSection(&watchdog_lock);

    SetEvent(watchdog_event);
}

static void
watchdog_stop(void)
{
    if (watchdog_thread == NULL) return;

    SetEvent(watchdog_stop_event);
    WaitForSingleObject(watchdog_thread, INFINITE);
    CloseHandle(watchdog_thread); watchdog_thread = NULL;
    CloseHandle(watchdog_event);  watchdog_event  = NULL;
    CloseHandle(watchdog_stop_event); watchdog_stop_event = NULL;
    DeleteCriticalSection(&watchdog_lock);
}
#endif

unsigned int
get_fd_limit(void)
{
#ifdef _WIN32
    /* actually windows should be able to hold
       way more, as they use HANDLEs for everything.
       Still this number should still be sufficient for
       the provided number of kqueue fds.
       */
    return 65536;
#else
    struct rlimit rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
        perror("getrlimit(2)");
        return (65536);
    } else {
        return (rlim.rlim_max);
    }
#endif
}

unsigned int
print_fd_table(void)
{
    unsigned int fd_max = get_fd_limit();
    unsigned int i;
    unsigned int used = 0;
    int our_errno = errno; /* Preserve errno */

#ifdef __linux__
    for (i = 0; i < fd_max; i++) {
        if (fcntl(i, F_GETFD) == 0) {
            printf("fd=%i used\n", i);
            used++;
        }
    }
#endif

    errno = our_errno;

    return used;
}

extern void watchdog_heartbeat(const char *test_name);

void
run_iteration(struct test_context *ctx)
{
    struct unit_test *test;

    for (test = ctx->tests; test->ut_name != NULL; test++) {
        if (test->ut_enabled) {
            ctx->test = test; /* Record the current test */
            watchdog_heartbeat(test->ut_name);
            test->ut_func(ctx);
        }
    }
    free(ctx);
}

void
test_harness(struct unit_test tests[MAX_TESTS], int iterations)
{
    int i, n, kqfd;
    struct test_context *ctx;

    printf("Running %d iterations\n", iterations);

    watchdog_start();
    testing_begin();
    if ((kqfd = kqueue()) < 0)
        die("kqueue()");
    n = 0;
    for (i = 0; i < iterations; i++) {
        ctx = calloc(1, sizeof(*ctx));
        if (ctx == NULL)
            abort();
        ctx->iteration = n++;
        ctx->kqfd = kqfd;
        memcpy(&ctx->tests, tests, sizeof(ctx->tests));
        ctx->iterations = iterations;

        run_iteration(ctx);
    }
    testing_end();

    close(kqfd);

    /*
     * Linux's monitoring thread reaps closed kqueues asynchronously
     * via the F_SETSIG-bound close-detect pipe.  If the test process
     * exits before the monitoring thread has processed the signal
     * for the kqueue we just closed, the kqueue stays in kq_list at
     * atexit, where libkqueue_free's cleanup pass walks it; by that
     * point the kernel may have reused fd N for an unrelated open,
     * making fcntl(F_GETFD) on the stale kq_id return success and
     * triggering the "is alive use_count=N.  Skipping, this is
     * likely a leak..." debug message.  Drain synchronously so the
     * monitoring thread retires the kq before atexit runs.
     */
#if defined(LIBKQUEUE_BACKEND_LINUX) || \
    (defined(__linux__) && !defined(LIBKQUEUE_BACKEND_POSIX))
    /*
     * The drain hook lives in the linux backend's monitoring
     * thread.  We need it on a default-Linux build (no backend
     * override -> linux backend), but not when the host is Linux
     * but we forced the POSIX backend.  CMake passes
     * LIBKQUEUE_BACKEND_<NAME>=1 into the test binary so the gate
     * tracks the actual library link.
     */
    {
        extern void libkqueue_drain_pending_close(void);
        libkqueue_drain_pending_close();
    }
#endif

    watchdog_stop();
}

/* Forward declarations for test-case arrays from each filter file. */
extern const struct lkq_test_case lkq_kqueue_tests[];
#ifdef EVFILT_READ
extern const struct lkq_test_case lkq_read_tests[];
#endif
#if defined(EVFILT_SIGNAL) && !defined(__ANDROID__)
extern const struct lkq_test_case lkq_signal_tests[];
#endif
#if defined(EVFILT_PROC)
extern const struct lkq_test_case lkq_proc_tests[];
#endif
#ifdef EVFILT_TIMER
extern const struct lkq_test_case lkq_timer_tests[];
#endif
#ifdef EVFILT_VNODE
extern const struct lkq_test_case lkq_vnode_tests[];
#endif
#ifdef EVFILT_WRITE
extern const struct lkq_test_case lkq_write_tests[];
#endif
#ifdef EVFILT_USER
extern const struct lkq_test_case lkq_user_tests[];
#endif
#if defined(EVFILT_LIBKQUEUE) && !defined(_WIN32)
extern const struct lkq_test_case lkq_libkqueue_tests[];
#endif
extern const struct lkq_test_case lkq_threading_tests[];

struct lkq_filter_entry {
    const char              *name;
    const struct lkq_test_case *cases;
};

static const struct lkq_filter_entry lkq_filter_table[] = {
    { "kqueue",    lkq_kqueue_tests    },
#ifdef EVFILT_READ
    { "socket",    lkq_read_tests      },
#endif
#if defined(EVFILT_SIGNAL) && !defined(__ANDROID__)
    { "signal",    lkq_signal_tests    },
#endif
#if defined(EVFILT_PROC)
    { "proc",      lkq_proc_tests      },
#endif
#ifdef EVFILT_TIMER
    { "timer",     lkq_timer_tests     },
#endif
#ifdef EVFILT_VNODE
    { "vnode",     lkq_vnode_tests     },
#endif
#ifdef EVFILT_WRITE
    { "write",     lkq_write_tests     },
#endif
#ifdef EVFILT_USER
    { "user",      lkq_user_tests      },
#endif
#if defined(EVFILT_LIBKQUEUE) && !defined(_WIN32)
    { "libkqueue", lkq_libkqueue_tests },
#endif
    { "threading", lkq_threading_tests },
    { NULL, NULL }
};

/* Known platform names for --list-gated=<name>. */
struct lkq_platform_name {
    const char     *name;
    lkq_platform_t  bits;
};

static const struct lkq_platform_name lkq_known_platforms[] = {
    { "windows",     LKQ_PLATFORM_OS_WINDOWS | LKQ_PLATFORM_BACKEND_WINDOWS  },
    { "linux",       LKQ_PLATFORM_OS_LINUX   | LKQ_PLATFORM_BACKEND_LINUX    },
    { "linux-posix", LKQ_PLATFORM_OS_LINUX   | LKQ_PLATFORM_BACKEND_POSIX    },
    { "freebsd",     LKQ_PLATFORM_OS_FREEBSD | LKQ_PLATFORM_BACKEND_NATIVE   },
    { "netbsd",      LKQ_PLATFORM_OS_NETBSD  | LKQ_PLATFORM_BACKEND_NATIVE   },
    { "openbsd",     LKQ_PLATFORM_OS_OPENBSD | LKQ_PLATFORM_BACKEND_NATIVE   },
    { "macos",       LKQ_PLATFORM_OS_MACOS   | LKQ_PLATFORM_BACKEND_NATIVE   },
    { "solaris",     LKQ_PLATFORM_OS_SOLARIS | LKQ_PLATFORM_BACKEND_SOLARIS  },
    { "posix",       LKQ_PLATFORM_BACKEND_POSIX                               },
    { NULL, 0 }
};

static void
init_platform(void)
{
    lkq_current_platform = 0;

    /* Backend */
#if defined(NATIVE_KQUEUE)
    lkq_current_platform |= LKQ_PLATFORM_BACKEND_NATIVE;
#elif defined(LIBKQUEUE_BACKEND_POSIX)
    lkq_current_platform |= LKQ_PLATFORM_BACKEND_POSIX;
#elif defined(LIBKQUEUE_BACKEND_LINUX)
    lkq_current_platform |= LKQ_PLATFORM_BACKEND_LINUX;
#elif defined(_WIN32)
    lkq_current_platform |= LKQ_PLATFORM_BACKEND_WINDOWS;
#elif defined(__sun)
    lkq_current_platform |= LKQ_PLATFORM_BACKEND_SOLARIS;
#endif

    /* OS */
#if defined(__linux__) && defined(__ANDROID__)
    lkq_current_platform |= LKQ_PLATFORM_OS_ANDROID;
#elif defined(__linux__)
    lkq_current_platform |= LKQ_PLATFORM_OS_LINUX;
#elif defined(__FreeBSD__)
    lkq_current_platform |= LKQ_PLATFORM_OS_FREEBSD;
#elif defined(__NetBSD__)
    lkq_current_platform |= LKQ_PLATFORM_OS_NETBSD;
#elif defined(__OpenBSD__)
    lkq_current_platform |= LKQ_PLATFORM_OS_OPENBSD;
#elif defined(__APPLE__)
    lkq_current_platform |= LKQ_PLATFORM_OS_MACOS;
#elif defined(__sun)
    lkq_current_platform |= LKQ_PLATFORM_OS_SOLARIS;
#elif defined(_WIN32)
    lkq_current_platform |= LKQ_PLATFORM_OS_WINDOWS;
#endif
}

static void
print_list_gated(lkq_platform_t target)
{
    const struct lkq_filter_entry *fe;
    const struct lkq_test_case *tc;
    const struct lkq_test_gate *g;
    int any = 0;

    for (fe = lkq_filter_table; fe->name != NULL; fe++) {
        for (tc = fe->cases; tc->name != NULL; tc++) {
            if (!tc->name[0] || !tc->gates) continue;
            for (g = tc->gates; g->reason != NULL; g++) {
                if (target & g->platform) {
                    printf("%s\t%s\t%s\n", fe->name, tc->name, g->reason);
                    any = 1;
                    break;
                }
            }
        }
    }
    if (!any)
        printf("# no gated tests for the given platform\n");
}

void
usage(void)
{
    printf("usage: [-hn] [--watchdog-timeout=SECONDS --watchdog-cmd=CMD] [testclass ...]\n"
           " -h                         This message\n"
           " -n                         Number of iterations (default: 1)\n"
           " --watchdog-timeout=N       Abort after N seconds with no test\n"
           "                            heartbeat (default: disabled)\n"
           " --watchdog-cmd=CMD         Run CMD <pid> for the stack dump on\n"
           "                            timeout, e.g. 'pstack' or\n"
           "                            '/usr/bin/eu-stack -p'\n"
           " --tmpdir=DIR               Directory for on-disk test files\n"
           "                            (override $TMPDIR, default /tmp).\n"
           " --list-gated[=PLATFORM]    Print tab-separated list of gated tests\n"
           "                            for PLATFORM (or current build if omitted).\n"
           "                            Output: suite TAB test TAB reason\n"
           "                            Known names: windows linux linux-posix\n"
           "                            freebsd netbsd openbsd macos solaris posix\n"
           " testclass[:<num>|:<start>-<end>] Tests suites to run:\n"
           "           ["
           "kqueue "
           "socket "
           "signal "
           "proc "
           "timer "
           "vnode "
           "user "
           "libkqueue]\n"
           "           All tests are run by default\n"
           "\n"
          );
    exit(1);
}

int
main(int argc, char **argv)
{
    /* Line-buffer stdout so a hung test in CI is visible in the live
     * log, instead of having the last 4KB of output sit in a block
     * buffer until the process exits (or aborts, which doesn't flush
     * stdio).  Pass our own static buffer so libc doesn't allocate
     * one internally; that allocation is never freed and shows up as
     * a "possibly lost" leak under valgrind.  Thread-safe: libc's
     * stdio serialises FILE access via the FILE's internal lock, so
     * the shared buffer is no different from libc's own.
     *
     * MSVC's CRT silently turns _IOLBF into _IOFBF when the stream
     * isn't a TTY, so on Win32 go fully unbuffered. */
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /*
     * Tests deliberately exercise CRT calls on closed / invalid fds
     * (double-close on the writer end of a pipe after the EOF-trigger
     * close, etc.).  MSVC's Debug CRT routes those through the
     * invalid-parameter handler, whose default action is to surface
     * a modal Watson dialog - on a headless CI runner that hangs the
     * process until the job timeout fires.  Install a no-op handler
     * for the whole test process so the call paths fall back to the
     * documented errno = EBADF return.
     */
    _set_invalid_parameter_handler(test_iph_noop);

    /*
     * MSVC Debug CRT also has a *separate* assertion path:
     * _CrtDbgReportW.  By default it pops a modal MessageBoxW for
     * any assert/error/warning - on a headless CI runner that just
     * hangs the process forever (we caught one stuck inside
     * _close_internal -> _CrtDbgReportW -> MessageBoxW with cdb).
     * Redirect all three report categories to stderr so the same
     * conditions surface as log lines and the call returns instead
     * of blocking on a UI dialog that nothing will ever click.
     */
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#else
    static char stdout_buf[BUFSIZ];
    setvbuf(stdout, stdout_buf, _IOLBF, sizeof(stdout_buf));
#endif

#if !defined(_WIN32)
    /*
     * The POSIX backend opens multiple FDs per kqueue (self-pipe pair plus
     * one eventfd pipe pair per filter that has one: signal, proc, user).
     * On macOS the default soft limit is 256, which can be too low for the
     * stress tests.  Raise to 4096 (capped at the hard limit) at startup so
     * the suite does not die with EMFILE mid-run.  This is best-effort;
     * if setrlimit fails we continue and let the individual tests handle it.
     */
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rlim_t want = 4096;
            if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
                want = rl.rlim_max;
            if (rl.rlim_cur < want) {
                rl.rlim_cur = want;
                (void) setrlimit(RLIMIT_NOFILE, &rl);
            }
        }
    }
#endif

    /*
     * Each filter test is gated on the public header actually
     * defining its EVFILT_* macro.  CMake renders sys/event.h
     * (from event.h.in) with only the filters the linked
     * libkqueue implements; tests for absent filters are simply
     * not registered, so a backend that ships a partial filter
     * set still produces a clean run.
     */
    struct unit_test tests[MAX_TESTS] = {
        { .ut_name = "kqueue",
          .ut_enabled = 1,
          .ut_func = test_kqueue,
          .ut_end = INT_MAX },
#ifdef EVFILT_READ
        { .ut_name = "socket",
          .ut_enabled = 1,
          .ut_func = test_evfilt_read,
          .ut_end = INT_MAX },
#endif
#if defined(EVFILT_SIGNAL) && !defined(__ANDROID__)
        // XXX-FIXME -- BROKEN ON LINUX WHEN RUN IN A SEPARATE THREAD
        { .ut_name = "signal",
          .ut_enabled = 1,
          .ut_func = test_evfilt_signal,
          .ut_end = INT_MAX },
#endif
#if defined(EVFILT_PROC)
        { .ut_name = "proc",
          .ut_enabled = 1,
          .ut_func = test_evfilt_proc,
          .ut_end = INT_MAX },
#endif
#ifdef EVFILT_TIMER
        { .ut_name = "timer",
          .ut_enabled = 1,
          .ut_func = test_evfilt_timer,
          .ut_end = INT_MAX },
#endif
#ifdef EVFILT_VNODE
        { .ut_name = "vnode",
          .ut_enabled = 1,
          .ut_func = test_evfilt_vnode,
          .ut_end = INT_MAX },
#endif
#ifdef EVFILT_WRITE
        { .ut_name = "write",
          .ut_enabled = 1,
          .ut_func = test_evfilt_write,
          .ut_end = INT_MAX },
#endif
#ifdef EVFILT_USER
        { .ut_name = "user",
          .ut_enabled = 1,
          .ut_func = test_evfilt_user,
          .ut_end = INT_MAX },
#endif
#if defined(EVFILT_LIBKQUEUE) && !defined(_WIN32)
        { .ut_name = "libkqueue",
          .ut_enabled = 1,
          .ut_func = test_evfilt_libkqueue,
          .ut_end = INT_MAX },
#endif
        { .ut_name = "threading",
          .ut_enabled = 1,
          .ut_func = test_threading,
          .ut_end = INT_MAX },
        { NULL, 0, NULL },
    };
    struct unit_test *test;
    int c, i, iterations;
    char *arg;
    int match;

#ifdef _WIN32
    /* Initialize the Winsock library */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
        err(1, "WSAStartup failed");
#endif

    init_platform();
    iterations = 1;

    /*
     * Argument parsing.  POSIX uses getopt_long; Windows ships
     * without it so we hand-roll a tiny parser that understands the
     * same options the test suite actually consumes:
     *
     *     -h
     *     -n N            (number of iterations)
     *     --watchdog-timeout=N
     *     --watchdog-cmd=CMD
     *     <testclass>[:<num>|:<start>-<end>] ...
     *
     * After the option phase, `optind_local` points at the first
     * positional argument; the shared block below filters the
     * test table down to whatever the user asked for.
     */
    int optind_local = 1;
#ifndef _WIN32
    {
        enum {
            OPT_WATCHDOG_TIMEOUT = 256,
            OPT_WATCHDOG_CMD,
            OPT_TMPDIR,
            OPT_LIST_GATED,
        };
        static const struct option long_opts[] = {
            { "watchdog-timeout", required_argument, NULL, OPT_WATCHDOG_TIMEOUT },
            { "watchdog-cmd",     required_argument, NULL, OPT_WATCHDOG_CMD     },
            { "tmpdir",           required_argument, NULL, OPT_TMPDIR           },
            { "list-gated",       optional_argument, NULL, OPT_LIST_GATED       },
            { NULL,               0,                 NULL, 0                    },
        };
        while ((c = getopt_long(argc, argv, "hn:", long_opts, NULL)) != -1) {
            switch (c) {
                case 'h':
                    usage();
                    break;
                case 'n':
                    iterations = atoi(optarg);
                    break;
                case OPT_WATCHDOG_TIMEOUT:
                    watchdog_timeout_s = atoi(optarg);
                    if (watchdog_timeout_s <= 0) {
                        fprintf(stderr, "--watchdog-timeout must be > 0\n");
                        exit(1);
                    }
                    break;
                case OPT_WATCHDOG_CMD:
                    watchdog_parse_cmd(optarg);
                    break;
                case OPT_TMPDIR:
                    test_tmpdir_set(optarg);
                    break;
                case OPT_LIST_GATED: {
                    lkq_platform_t target = lkq_current_platform;
                    if (optarg && optarg[0]) {
                        const struct lkq_platform_name *pn;
                        int found = 0;
                        for (pn = lkq_known_platforms; pn->name != NULL; pn++) {
                            if (strcmp(optarg, pn->name) == 0) {
                                target = pn->bits;
                                found = 1;
                                break;
                            }
                        }
                        if (!found) {
                            fprintf(stderr, "unknown platform '%s'\n", optarg);
                            exit(1);
                        }
                    }
                    print_list_gated(target);
                    exit(0);
                }
                default:
                    usage();
            }
        }
        optind_local = optind;
    }
#else
    while (optind_local < argc) {
        const char *a = argv[optind_local];
        if (strcmp(a, "-h") == 0) {
            usage();
        } else if (strcmp(a, "-n") == 0 && optind_local + 1 < argc) {
            iterations = atoi(argv[++optind_local]);
        } else if (strncmp(a, "--watchdog-timeout=", 19) == 0) {
            watchdog_timeout_s = atoi(a + 19);
            if (watchdog_timeout_s <= 0) {
                fprintf(stderr, "--watchdog-timeout must be > 0\n");
                exit(1);
            }
        } else if (strncmp(a, "--watchdog-cmd=", 15) == 0) {
            watchdog_parse_cmd(a + 15);
        } else if (strncmp(a, "--list-gated", 12) == 0) {
            lkq_platform_t target = lkq_current_platform;
            const char *eq = strchr(a, '=');
            if (eq && eq[1]) {
                const struct lkq_platform_name *pn;
                int found = 0;
                for (pn = lkq_known_platforms; pn->name != NULL; pn++) {
                    if (strcmp(eq + 1, pn->name) == 0) {
                        target = pn->bits;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr, "unknown platform '%s'\n", eq + 1);
                    exit(1);
                }
            }
            print_list_gated(target);
            exit(0);
        } else if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage();
        } else {
            break;  /* first positional - rest go to test selection */
        }
        optind_local++;
    }
#endif

    /* If specific tests are requested, disable all tests by default */
    if (optind_local < argc) {
        for (test = tests; test->ut_name != NULL; test++) {
            test->ut_enabled = 0;
        }
    }
    for (i = optind_local; i < argc; i++) {
        match = 0;
        arg = argv[i];
        for (test = tests; test->ut_name != NULL; test++) {
            size_t namelen = strlen(test->ut_name);
            char const *p;
            char *q;

            if (strncmp(arg, test->ut_name, strlen(test->ut_name)) == 0) {
                test->ut_enabled = 1;
                match = 1;

                p = arg + namelen;

                /*
                 * Test name includes a test range
                 */
                if (*p == ':') {
                    p++;

                    test->ut_start = strtoul(p, &q, 10);
                    if (p == q)
                        goto invalid_option;
                    p = q;

                    /*
                     * Range is in the format <start>-<end>
                     */
                    if (*p == '-') {
                        p++;
                        test->ut_end = strtoul(p, &q, 10);
                        if (p == q)
                            goto invalid_option;
                        printf("enabled test: %s (%u-%u)\n", test->ut_name, test->ut_start, test->ut_end);
                    /*
                     * Range is in the format <num>
                     */
                    } else if (*p == '\0') {
                        test->ut_end = test->ut_start;
                        printf("enabled test: %s (%u)\n", test->ut_name, test->ut_start);
                    /*
                     * Range is invalid
                     */
                    } else
                        goto invalid_option;
                /*
                 * Test name does not include a range
                 */
                } else if (*p == '\0') {
                    test->ut_start = 0;
                    test->ut_end = INT_MAX;
                    printf("enabled test: %s\n", test->ut_name);
                } else
                    goto invalid_option;
                break;
            }
        }
        if (!match) {
        invalid_option:
            printf("ERROR: invalid option: %s\n", arg);
            exit(1);
        }
    }

    test_harness(tests, iterations);

    return (0);
}
