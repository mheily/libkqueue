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

#include <limits.h>

#ifndef _WIN32
#include <getopt.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__sun) || defined(__APPLE__)
#include <sys/time.h>
#include <sys/resource.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "common.h"

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
                kill(child, SIGKILL);
                waitpid(child, NULL, 0);
                break;
            }
            sleep(1);
            waited++;
        }
    }

    const char *abrt = "=== test watchdog: killing parent ===\n";
    write(STDERR_FILENO, abrt, strlen(abrt));
    kill(parent, SIGKILL);
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
        kill(watchdog_pid, SIGKILL);
        waitpid(watchdog_pid, NULL, 0);
        watchdog_pid = -1;
    }
    if (watchdog_pipe[1] >= 0) {
        close(watchdog_pipe[1]);
        watchdog_pipe[1] = -1;
    }
}
#else  /* _WIN32 */
static void watchdog_start(void) {}
void        watchdog_heartbeat(const char *test_name) { (void) test_name; }
static void watchdog_stop(void)  {}
static void watchdog_parse_cmd(const char *cmd) { (void) cmd; }
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
     * the shared buffer is no different from libc's own. */
    static char stdout_buf[BUFSIZ];
    setvbuf(stdout, stdout_buf, _IOLBF, sizeof(stdout_buf));

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
#if defined(EVFILT_SIGNAL) && !defined(_WIN32) && !defined(__ANDROID__)
        // XXX-FIXME -- BROKEN ON LINUX WHEN RUN IN A SEPARATE THREAD
        { .ut_name = "signal",
          .ut_enabled = 1,
          .ut_func = test_evfilt_signal,
          .ut_end = INT_MAX },
#endif
#ifdef EVFILT_PROC
        { .ut_name = "proc",
          .ut_enabled = 1,
          .ut_func = test_evfilt_proc,
          .ut_end = INT_MAX },
#endif
/*
 * EVFILT_TIMER on the POSIX backend is currently flaky (the
 * sleeper-thread + ack-pipe machinery deadlocks on test_get and
 * onwards); skip the whole class until that path is reworked.
 */
#if defined(EVFILT_TIMER) && !defined(LIBKQUEUE_BACKEND_POSIX)
        { .ut_name = "timer",
          .ut_enabled = 1,
          .ut_func = test_evfilt_timer,
          .ut_end = INT_MAX },
#endif
#if defined(EVFILT_VNODE) && !defined(_WIN32)
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
#ifdef EVFILT_LIBKQUEUE
        { .ut_name = "libkqueue",
          .ut_enabled = 1,
          .ut_func = test_evfilt_libkqueue,
          .ut_end = INT_MAX },
#endif
/*
 * The threading suite exercises cross-thread NOTE_TRIGGER delivery,
 * close-wake (kevent unblocking when another thread close()s the
 * kq), and a bunch of EV_ADD/EV_DELETE-during-wait races.  The
 * POSIX backend's pselect dispatcher holds the kq lock across the
 * wait and has no close-wake mechanism, so all of these tests
 * either hang or trip stale-fd errors.  Skip the class until that
 * machinery lands.
 */
#if !defined(LIBKQUEUE_BACKEND_POSIX)
        { .ut_name = "threading",
          .ut_enabled = 1,
          .ut_func = test_threading,
          .ut_end = INT_MAX },
#endif
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

    iterations = 1;

/* Windows does not provide a POSIX-compatible getopt */
#ifndef _WIN32
    {
        enum {
            OPT_WATCHDOG_TIMEOUT = 256,
            OPT_WATCHDOG_CMD,
        };
        static const struct option long_opts[] = {
            { "watchdog-timeout", required_argument, NULL, OPT_WATCHDOG_TIMEOUT },
            { "watchdog-cmd",     required_argument, NULL, OPT_WATCHDOG_CMD     },
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
                default:
                    usage();
            }
        }
    }

    /* If specific tests are requested, disable all tests by default */
    if (optind < argc) {
        for (test = tests; test->ut_name != NULL; test++) {
            test->ut_enabled = 0;
        }
    }
    for (i = optind; i < argc; i++) {
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
#endif

    test_harness(tests, iterations);

    return (0);
}
