/*
 * Copyright (c) 2021 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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
/*
 * glibc hides the syscall(2) prototype unless _GNU_SOURCE or
 * _DEFAULT_SOURCE is defined; the project compiles with
 * _XOPEN_SOURCE=600 which suppresses it.  We need syscall(SYS_gettid)
 * inside the __linux__ block below, so opt the file in to the GNU
 * namespace before any header is included.
 */
#ifdef __linux__
#  define _GNU_SOURCE 1
#endif

#include <err.h>
#include <signal.h>
#include <inttypes.h>
#ifdef __linux__
#  include <sys/prctl.h>
#  include <sys/syscall.h>     /* SYS_gettid for the wait-thread tid */
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <limits.h>

#include "../common/private.h"

/*
 * An entry in the proc_pid tree
 *
 * This contains a list of PIDs all kqueues are interested in
 * and a list of knotes that are waiting on notifications for
 * those PIDs.
 */
struct proc_pid {
    RB_ENTRY(proc_pid)            ppd_entry;   //!< Entry in the proc_pid_index tree.
    pid_t                         ppd_pid;     //!< PID we're waiting on.
    LIST_HEAD(pid_waiters, knote) ppd_proc_waiters; //!< knotes that are waiting on this PID.
};

/*
 * Synchronisation for wait thread information
 *
 * This ensures that the number of process being monitored is correct
 * and stops multiple threads attempting to start a monitor thread
 * at the same time.
 */
static tracing_mutex_t     proc_init_mtx = TRACING_MUTEX_INITIALIZER;
static int                 proc_count = 0;
static pthread_t           proc_wait_thread;
static pid_t               proc_wait_tid; /* Monitoring thread; set inside wait_thread_loop. */
static bool                proc_wait_thread_created;
                                          /*
                                           * Set true after pthread_create succeeds; reset
                                           * after pthread_join in destroy.  Protected by
                                           * proc_init_mtx.  Distinguishes "thread alive,
                                           * needs join" from "thread never started" so
                                           * destroy doesn't pthread_cancel garbage and
                                           * fork-child can clear it without leaving stale
                                           * state.
                                           */

/*
 * Synchronisation for thread start
 *
 * This prevents any threads from continuing whilst the wait thread
 * is started.  proc_wait_thread_started is the loop predicate the
 * cond_wait spins on so spurious wakes don't release the init
 * caller before the wait thread has actually entered its sigwaitinfo
 * loop.
 */
static pthread_mutex_t     proc_wait_thread_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      proc_wait_thread_cond = PTHREAD_COND_INITIALIZER;
static bool                proc_wait_thread_started;   /* protected by proc_wait_thread_mtx */

/*
 * The global PID tree
 *
 * Contains all the PIDs any kqueue is interested in waiting on
 */
static RB_HEAD(pid_index, proc_pid) proc_pid_index;
static tracing_mutex_t    proc_pid_index_mtx = TRACING_MUTEX_INITIALIZER;
#ifndef _WIN32
pthread_mutexattr_t       proc_pid_index_mtx_attr;
#endif

static int
proc_pid_cmp(struct proc_pid *a, struct proc_pid *b)
{
    return (a->ppd_pid > b->ppd_pid) - (a->ppd_pid < b->ppd_pid);
}

RB_GENERATE(pid_index, proc_pid, ppd_entry, proc_pid_cmp)

/*
 * Notify all the waiters on a PID
 *
 * @note This must be called with the proc_pid_index_mtx held
 *       to prevent knotes/filters/kqueues being freed out from
 *       beneath us.  When a filter is freed it will attempt
 *       to free all associated knotes, and will attempt to
 *       lock the proc_pid_index_mtx in evfilt_proc_knote_delete.
 *       If we hold this mutex then the deletion cannot proceed.
 */
static void
waiter_notify(struct proc_pid *ppd, int status)
{
    struct knote *kn;
    struct filter *filt;

    while ((kn = LIST_FIRST(&ppd->ppd_proc_waiters))) {
        kn->kn_proc_status = status;

        /*
         * This operates in the context of the kqueue
         * that the knote belongs to.  We may up
         * signalling multiple kqueues that the
         * specified process exited, and for each one
         * we need to call eventfd_raise.
         */
        filt = knote_get_filter(kn);
        dbg_printf("pid=%u exited, notifying kq=%u filter=%p kn=%p",
                   (unsigned int)ppd->ppd_pid, kn->kn_kq->kq_id, filt, kn);
        kqops.eventfd_raise(&filt->kf_efd);
        LIST_INSERT_HEAD(&filt->kf_ready, kn, kn_ready); /* protected by proc_pid_index_mtx */

        LIST_REMOVE_ZERO(kn, kn_proc_waiter);
    }

    dbg_printf("pid=%u removing waiter list", (unsigned int)ppd->ppd_pid);
    RB_REMOVE(pid_index, &proc_pid_index, ppd);
    free(ppd);
}

static void
waiter_notify_error(struct proc_pid *ppd, int wait_errno)
{
    struct knote *kn;
    struct filter *filt;

    while ((kn = LIST_FIRST(&ppd->ppd_proc_waiters))) {
        kn->kev.flags |= EV_ERROR;
        kn->kev.data = wait_errno;

        filt = knote_get_filter(kn);
        dbg_printf("pid=%u errored (%s), notifying kq=%u filter=%p kn=%p",
                   (unsigned int)ppd->ppd_pid, strerror(errno), kn->kn_kq->kq_id, filt, kn);
        kqops.eventfd_raise(&filt->kf_efd);
        LIST_INSERT_HEAD(&filt->kf_ready, kn, kn_ready); /* protected by proc_pid_index_mtx */

        LIST_REMOVE_ZERO(kn, kn_proc_waiter);
    }

    dbg_printf("pid=%u removing waiter list", (unsigned int)ppd->ppd_pid);
    RB_REMOVE(pid_index, &proc_pid_index, ppd);
    free(ppd);
}

/*
 * Convert a CLD_* siginfo into the waitpid-style status word.
 *
 * Reconstructs the status code waitpid would have returned for the
 * same exit, matching what the OpenBSD man pages document and what
 * macOS native kqueue puts in the data field of the kevent.
 *
 * @param[out] status_out  populated only on a 0 return.
 * @param[in]  info        siginfo from sigwaitinfo / waitid.
 * @return  0 if info described a real exit (status_out is valid),
 *         -1 on a non-exit si_code (CLD_STOPPED etc) - the caller
 *         should ignore the notification.
 */
static int
waiter_siginfo_to_status(int *status_out, siginfo_t *info)
{
    int status;

    switch (info->si_code) {
    case CLD_EXITED:    /* WIFEXITED - High byte contains status, low byte zeroed */
        status = info->si_status << 8;
        dbg_printf("pid=%u exited, status %u", (unsigned int)info->si_pid, status);
        break;

    case CLD_DUMPED:    /* WIFSIGNALED/WCOREDUMP - Core flag set - Low 7 bits contains fatal signal */
        status = (info->si_status & 0x7f) | 0x80;
        dbg_printf("pid=%u dumped, status %u", (unsigned int)info->si_pid, status);
        break;

    case CLD_KILLED:    /* WIFSIGNALED - Low 7 bits contains fatal signal */
        status = info->si_status & 0x7f;
        dbg_printf("pid=%u signalled, status %u", (unsigned int)info->si_pid, status);
        break;

    default: /* CLD_STOPPED / CLD_TRAPPED / CLD_CONTINUED - not exit states */
        return (-1);
    }

    *status_out = status;
    return (0);
}

/*
 * This waiter thread serves all kqueues in a given process
 *
 */
static void *
wait_thread_loop(UNUSED void *arg)
{
    int status;
    int ret = 0;
    siginfo_t info;
    sigset_t sigmask;
    struct proc_pid *ppd, *ppd_tmp;

#ifdef __linux__
    /*
     * Set the thread's name to something descriptive so it shows up in gdb,
     * etc. Max name length is 16 bytes.
     */
    prctl(PR_SET_NAME, "libkqueue_wait", 0, 0, 0);
    proc_wait_tid = syscall(SYS_gettid);
#else
    /*
     *  There's no POSIX interface for getting a numeric thread ID
     */
    proc_wait_tid = 1;
#endif

    dbg_printf("tid=%u - waiter thread started", proc_wait_tid);

    /*
     * Native kqueue implementations leave processes monitored
     * with EVFILT_PROC + NOTE_EXIT un-reaped when they exit.
     *
     * Applications using kqueue expect to be able to retrieve
     * the process exit state using one of the *wait*() calls.
     *
     * So that these applications don't experience errors,
     * processes must remain un-reaped so those subsequent
     * *wait*() calls can succeed.
     *
     * Because processes must remain un-reaped we can only use
     * calls which accept the WNOWAIT flag, and do not reap
     * processes.
     *
     * The obvious solution is to use waitid(P_ALL,,,WNOWAIT),
     * and only notify on the PIDs we're interested in.
     * Unfortunately waitid() will return the same PID repeatedly
     * if the previously returned PID was not reaped.
     *
     * At first glance sigwaitinfo([SIGCHLD], ) appears to be
     * another promising option.  The siginfo_t populated by
     * sigwaitinfo() contains all the information we need to
     * raise a notification that a process has exited, and a
     * SIGCHLD signal is raised every time a process exits.
     *
     * Unfortunately on Linux, SIGCHLD signals, along with all
     * other signals, are coalesced.  This means if two children
     * exited before the wait thread was woken up from waiting
     * on sigwaitinfo(), we'd only get the siginfo_t structure
     * populated for one child.
     *
     * The only apparent fully kqueue compatible and POSIX
     * compatible solution is to use sigwatinfo() to determine
     * that _A_ child had exited, and then scan all PIDs we're
     * interested in, passing them to waitid() in turn.
     *
     * This isn't a great solution for the following reasons:
     *
     * - SIGCHLD must be delivered to the wait thread in order
     *   for notifications to be sent out.
     *   If the application installs its own signal handler
     *   for SIGCHLD, or changes the process signal mask,
     *   we may never get notified when a child exits.
     *
     * - Scanning through all the monitored PIDs doesn't scale
     *   well, and involves many system calls.
     *
     * - Because only one thread can receive the SIGCHLD
     *   signal, all monitoring must be done in a single
     *   waiter thread.  This introduces code complexity and
     *   contention around the global tree that holds the PIDs
     *   that are being monitored.
     *
     * Because of these limitations, on Linux >= 5.3 we use
     * pidfd_open() to get a FD bound to a PID that we can
     * monitor.  Testing shows that multiple pidfds bound to
     * the same PID will each receive a notification when
     * that process exits.  Using pidfd avoids the messiness
     * of signals, the linear scans, and the global structures.
     *
     * Unfortunately at the time of writing Linux 5.3 is still
     * relatively new, and the feasibility of writing native
     * solutions for platforms like Solaris hasn't been
     * investigated.
     *
     * Until Linux 5.3 becomes more widely used, and we have
     * a native solution for Solaris this POSIX EVFILT_PROC
     * code must remain to provide a fallback mechanism.
     */

    /*
     * Block all signals - sigwaitinfo isn't affected by this
     */
    sigfillset(&sigmask);
    pthread_sigmask(SIG_BLOCK, &sigmask, NULL);

    /*
     * Only listen on SIGCHLD wait sigwaitinfo
     */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGCHLD);

    /*
     * Inform the parent that we started correctly.  Conventional
     * cond_wait predicate pattern: write under the mutex so the
     * parent's predicate read (also under the mutex) is fully
     * synchronised.
     */
    pthread_mutex_lock(&proc_wait_thread_mtx);
    proc_wait_thread_started = true;
    pthread_cond_signal(&proc_wait_thread_cond);
    pthread_mutex_unlock(&proc_wait_thread_mtx);
    do {
        if (ret > 0)
            dbg_printf("sigwaitinfo() returned signo=%d si_pid=%u si_code=%d",
                       ret, (unsigned int) info.si_pid, info.si_code);
        /*
         * Check for errors before altering the cancellation
         * state, so we don't end up calling sigwaitinfo
         * with the cancellations disabled.
         */
        if (ret < 0) {
            dbg_printf("sigwaitinfo(2): %s", strerror(errno));
            continue;
        }

        /*
         * Don't allow the thread to be cancelled until we've
         * finished one loop in the monitoring thread.  This
         * ensures there are no nasty issue on exit.
         */
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        /*
         * The knote_* functions (i.e. those that could free a
         * knote).
         *
         * All attempt to take this lock before modifying the
         * knotes, wo we shouldn't have any issues here with
         * knotes being freed out from underneath the waiter
         * thread.
         */
        tracing_mutex_lock(&proc_pid_index_mtx);

        /*
         * Sig 0 is the NULL signal.  This means it's the first
         * iteration through the loop, and we just want to scan
         * any existing PIDs.
         *
         * This fixes a potential race between the thread
         * starting, EVFILT_PROC kevents being added, and a
         * process exiting.
         */
        if (ret > 0) {
            /*
             * Check if this is a process we want to monitor
             */
            ppd = RB_FIND(pid_index, &proc_pid_index, &(struct proc_pid){ .ppd_pid = info.si_pid });
            if (ppd && waiter_siginfo_to_status(&status, &info) == 0)
                waiter_notify(ppd, status);
        }

        /*
         * Scan the list of outstanding PIDs to see if
         * there are any we need to notify.
         */
        RB_FOREACH_SAFE(ppd, pid_index, &proc_pid_index, ppd_tmp) {
        again:
            if (waitid(P_PID, ppd->ppd_pid, &info, WEXITED | WNOWAIT | WNOHANG) < 0) {
                switch (errno) {
                case ECHILD:
                    dbg_printf("waitid(2): pid=%u reaped too early - %s", ppd->ppd_pid, strerror(errno));

                    waiter_notify_error(ppd, errno);
                    continue;

                case EINTR:
                    goto again;
                }
            }

            if (waiter_siginfo_to_status(&status, &info) == 0)
                waiter_notify(ppd, status);
        }
        tracing_mutex_unlock(&proc_pid_index_mtx);

        dbg_printf("waiting for SIGCHLD");
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); /* sigwaitinfo is the next cancellation point */
    } while ((ret = sigwaitinfo(&sigmask, &info)));

    dbg_printf("exited");

    return (NULL);
}

static void
evfilt_proc_libkqueue_init(void)
{
    /*
     * Initialise the global PID index tree.  This needs to be
     * recursive as the delete/enable/disable functions all
     * need to lock the mutex, and they may be called indirectly
     * in the copyout function (which already locks this mutex),
     * as well as by other kqueue code.
     */
#ifndef _WIN32
     pthread_mutexattr_init(&proc_pid_index_mtx_attr);
     pthread_mutexattr_settype(&proc_pid_index_mtx_attr, PTHREAD_MUTEX_RECURSIVE);
#endif
     tracing_mutex_init(&proc_pid_index_mtx, &proc_pid_index_mtx_attr);
}

static void
evfilt_proc_libkqueue_fork(void)
{
    /*
     * The wait thread isn't duplicated in the forked copy of the
     * process, and we need to prevent a cancellation/join request
     * being sent to it when all the knotes are freed.  Clear the
     * created flag so destroy's gate fails.  proc_count is reset
     * so the child's first init re-creates the dispatch thread
     * cleanly; proc_wait_tid is cleared for debug-print accuracy.
     */
    proc_wait_thread_created = false;
    proc_wait_tid = 0;
    proc_count = 0;
}

static int
evfilt_proc_init(struct filter *filt)
{
    if (kqops.eventfd_init(&filt->kf_efd, filt) < 0) {
    error_0:
        return (-1);
    }

    if (kqops.eventfd_register(filt->kf_kqueue, &filt->kf_efd) < 0) {
    error_1:
        kqops.eventfd_close(&filt->kf_efd);
        goto error_0;
    }

    /*
     * Initialise global resources (wait thread and PID tree).
     */
    tracing_mutex_lock(&proc_init_mtx);
    if (proc_count == 0) {
        sigset_t sigmask;

        /*
         * We do this at a process level to ensure
         * that the wait thread is the only thing
         * that receives SIGCHLD.
         *
         * The wait thread will not reliably receive
         * a SIGCHLD signal if SIGCHLD is not
         * blocked in all other threads.
         */
        sigemptyset(&sigmask);
        sigaddset(&sigmask, SIGCHLD);
        pthread_sigmask(SIG_BLOCK, &sigmask, NULL);

        dbg_printf("creating wait thread");

        pthread_mutex_lock(&proc_wait_thread_mtx);
        if (pthread_create(&proc_wait_thread, NULL, wait_thread_loop, NULL) != 0) {
            tracing_mutex_unlock(&proc_init_mtx);
            pthread_mutex_unlock(&proc_wait_thread_mtx);

            goto error_1;
        }
        /*
         * Mark the thread as created so destroy can later
         * pthread_cancel + pthread_join it.  Set BEFORE the
         * cond_wait so an early cancel from another path
         * (e.g., a signal-driven teardown that races init)
         * still finds a flag to clear.
         */
        proc_wait_thread_created = true;
        /*
         * Wait until the wait thread has started and is monitoring
         * signals.  Loop on the predicate so a spurious cond_wait
         * return doesn't release us before the wait thread actually
         * entered its sigwaitinfo loop.  proc_wait_thread_mtx
         * synchronises the predicate read against the wait thread's
         * write.
         */
        while (!proc_wait_thread_started)
            pthread_cond_wait(&proc_wait_thread_cond, &proc_wait_thread_mtx);
        pthread_mutex_unlock(&proc_wait_thread_mtx);
    }
    proc_count++;
    tracing_mutex_unlock(&proc_init_mtx);

    return (0);
}

static void
evfilt_proc_destroy(struct filter *filt)
{
    /*
     * Free global resources like the wait thread
     * and PID tree.
     */
    tracing_mutex_lock(&proc_init_mtx);
    assert(proc_count > 0);
    /*
     * Only cancel + join the wait thread if pthread_create
     * actually succeeded (proc_wait_thread_created) AND we're not
     * in a forked copy (where the thread doesn't exist post-fork).
     * The fork hook resets proc_wait_thread_created.
     */
    if ((--proc_count == 0) && proc_wait_thread_created) {
#ifndef NDEBUG
        pid_t tid = proc_wait_tid;
#endif
        void *retval;
        int ret;

        dbg_printf("tid=%u - cancelling", tid);
        ret = pthread_cancel(proc_wait_thread);
        if (ret != 0)
           dbg_printf("tid=%u - cancel failed: %s", tid, strerror(ret));

        ret = pthread_join(proc_wait_thread, &retval);
        if (ret == 0) {
            if (retval == PTHREAD_CANCELED) {
                dbg_printf("tid=%u - joined with exit_status=PTHREAD_CANCELED", tid);
            } else {
                dbg_printf("tid=%u - joined with exit_status=%" PRIdPTR, tid, (intptr_t)retval);
            }
        } else {
            dbg_printf("tid=%u - join failed: %s", tid, strerror(ret));
        }
        proc_wait_thread_created = false;
        /*
         * Wait thread is joined; no concurrent reader.  Reset the
         * predicate under the mutex so a subsequent
         * evfilt_proc_init (e.g. after the last filter is destroyed
         * and a new one created later) re-runs the cond_wait loop
         * from scratch.
         */
        pthread_mutex_lock(&proc_wait_thread_mtx);
        proc_wait_thread_started = false;
        pthread_mutex_unlock(&proc_wait_thread_mtx);
    }
    tracing_mutex_unlock(&proc_init_mtx);

    kqops.eventfd_unregister(filt->kf_kqueue, &filt->kf_efd);
    kqops.eventfd_close(&filt->kf_efd);
}

/*
 * Begin watching a pid for exit on behalf of a knote.
 *
 * Inserts the knote onto the pid's waiter list (creating the
 * proc_pid entry if no other knote was already watching), then
 * probes via waitid(WNOWAIT) for the un-reaped-zombie case so we
 * can fire NOTE_EXIT immediately (otherwise the SIGCHLD-scan model
 * would leave the knote stuck until an unrelated sibling exits and
 * forces the wait thread to scan the RB tree).
 *
 * The ECHILD path is intentionally NOT fired as EV_ERROR: waitid
 * returns ECHILD for both "was our child, already reaped" and
 * "was never our child / pid has since been reused by an unrelated
 * process".  We can't distinguish, so we can't safely surface a
 * synthetic error - the original process is gone in case 1, and
 * the user's intent is undefined in cases 2 and 3.  Stay
 * registered; if the pid does later exit (and is genuinely our
 * child) the wait thread will eventually deliver normally.
 *
 * Caller must NOT hold proc_pid_index_mtx; this function takes it
 * internally so that the RB / waiter / probe + notify steps are all
 * serialised against the wait thread.
 *
 * @return 0 on success, -1 on calloc failure.
 */
static int
proc_pid_arm(struct filter *filt, struct knote *kn)
{
    struct proc_pid *ppd;
    siginfo_t        info;

    (void) filt;

    tracing_mutex_lock(&proc_pid_index_mtx);

    ppd = RB_FIND(pid_index, &proc_pid_index, &(struct proc_pid){ .ppd_pid = kn->kev.ident });
    if (!ppd) {
        dbg_printf("pid=%u adding waiter list", (unsigned int)kn->kev.ident);
        ppd = calloc(1, sizeof(struct proc_pid));
        if (unlikely(!ppd)) {
            tracing_mutex_unlock(&proc_pid_index_mtx);
            return (-1);
        }
        ppd->ppd_pid = kn->kev.ident;
        {
            /*
             * RB_INSERT returns NULL on success, the existing entry
             * on duplicate.  We hold proc_pid_index_mtx and just
             * did RB_FIND, so duplicate is impossible by
             * construction; assert documents the invariant for
             * future refactors.  The call must be outside assert()
             * so it still runs under NDEBUG.
             */
            struct proc_pid *dup = RB_INSERT(pid_index, &proc_pid_index, ppd);
            assert(dup == NULL);
            (void) dup;
        }
    }
    LIST_INSERT_HEAD(&ppd->ppd_proc_waiters, kn, kn_proc_waiter);

    /*
     * waitid(WNOWAIT) inspects without reaping so an existing
     * application-level wait()/waitpid() consumer downstream is
     * unaffected on the live path.  Only fire if the kernel
     * positively confirms an exit (rv == 0 and si_pid matches);
     * waiter_notify frees ppd, so don't touch it afterwards.
     */
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t) kn->kev.ident, &info,
               WEXITED | WNOWAIT | WNOHANG) == 0
        && info.si_pid == (pid_t) kn->kev.ident) {
        int status;

        if (waiter_siginfo_to_status(&status, &info) == 0) {
            dbg_printf("pid=%u already exited, firing immediately", (unsigned int) kn->kev.ident);
            waiter_notify(ppd, status);
        }
        /*
         * waiter_siginfo_to_status returning -1 means a non-exit
         * si_code (CLD_STOPPED etc).  Stay registered and let the
         * wait thread handle the eventual exit normally.
         */
    }
    /*
     * else: alive, never-our-child, or already reaped - all
     * indistinguishable from waitid here.  Stay registered.
     */

    tracing_mutex_unlock(&proc_pid_index_mtx);
    return (0);
}

/*
 * Stop watching a pid for the given knote.
 *
 * Removes the knote from its waiter list and frees the proc_pid
 * entry if no other knote was watching the same pid.  Idempotent
 * (safe to call when the knote isn't on a list, e.g. because
 * waiter_notify already moved it to kf_ready).
 *
 * Caller must NOT hold proc_pid_index_mtx.
 */
/* Caller must hold proc_pid_index_mtx. */
static inline void
proc_pid_disarm(struct knote *kn)
{
    struct proc_pid *ppd;

    if (LIST_INSERTED(kn, kn_proc_waiter))
        LIST_REMOVE_ZERO(kn, kn_proc_waiter);

    ppd = RB_FIND(pid_index, &proc_pid_index, &(struct proc_pid){ .ppd_pid = kn->kev.ident });
    if (ppd && LIST_EMPTY(&ppd->ppd_proc_waiters)) {
        dbg_printf("pid=%u removing waiter list", (unsigned int)ppd->ppd_pid);
        RB_REMOVE(pid_index, &proc_pid_index, ppd);
        free(ppd);
    }
}

static int
evfilt_proc_knote_create(struct filter *filt, struct knote *kn)
{
    /* TODO: kn_create arms before EV_DISABLE - see kevent_copyin_one EV_ADD|EV_DISABLE race. */
    /*
     * Match native kqueue and the Linux pidfd backend: only register
     * if the caller asked for at least one event we can deliver.
     * Currently NOTE_EXIT is the only fflag this backend implements.
     */
    if (!(kn->kev.fflags & NOTE_EXIT)) {
        dbg_printf("not monitoring pid=%u as no NOTE_* fflags set",
                   (unsigned int) kn->kev.ident);
        return (0);
    }

    if (proc_pid_arm(filt, kn) < 0) return (-1);

    /*
     * EV_ONESHOT|EV_CLEAR are forced because process exit is an edge
     * event, not a level one.  See evfilt_proc_knote_modify for the
     * matching preserve-on-modify policy.
     */
    kn->kev.flags |= EV_ONESHOT;
    kn->kev.flags |= EV_CLEAR;

    return (0);
}

int
evfilt_proc_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent *kev)
{
    static const unsigned int preserve = EV_ONESHOT | EV_CLEAR | EV_RECEIPT;
    bool was_armed, want_armed;

    /*
     * Detect arm-state from the structural fact (knote is on its
     * pid's waiter list) rather than from a flag bit, so transient
     * states like "waiter_notify already moved this knote off the
     * waiter list onto kf_ready" don't masquerade as armed.
     */
    was_armed  = LIST_INSERTED(kn, kn_proc_waiter);
    want_armed = (kev->fflags & NOTE_EXIT) != 0;

    /*
     * Merge the new caller-visible flags onto kn->kev, preserving
     * EV_ONESHOT|EV_CLEAR (forced by proc_pid_arm because process
     * exit is an edge event) and EV_RECEIPT (sticky on BSD).
     * Mirror the linux/proc.c kn_modify policy exactly.
     */
    kn->kev.flags  = (kev->flags & ~preserve)
                   | (kn->kev.flags & preserve);
    kn->kev.fflags = kev->fflags;

    if (want_armed && !was_armed) {
        if (proc_pid_arm(filt, kn) < 0) return (-1);
        kn->kev.flags |= EV_ONESHOT | EV_CLEAR;
    } else if (!want_armed && was_armed) {
        /*
         * Caller stopped caring about NOTE_EXIT.  Disarm and drop
         * any already-fired delivery from kf_ready in one critical
         * section so we don't take proc_pid_index_mtx twice.
         */
        tracing_mutex_lock(&proc_pid_index_mtx);
        proc_pid_disarm(kn);
        if (LIST_INSERTED(kn, kn_ready))
            LIST_REMOVE_ZERO(kn, kn_ready);
        if (LIST_EMPTY(&filt->kf_ready))
            kqops.eventfd_lower(&filt->kf_efd);
        tracing_mutex_unlock(&proc_pid_index_mtx);
    }

    return (0);
}

int
evfilt_proc_knote_delete(struct filter *filt, struct knote *kn)
{
    /*
     * If the wait thread already linked this knote onto kf_ready
     * (process exited, no kevent() drain yet), unlink it before the
     * caller's knote_release frees the knote - otherwise the next
     * copyout walks a freed pointer.  Combine with the disarm so
     * we take proc_pid_index_mtx once.
     */
    tracing_mutex_lock(&proc_pid_index_mtx);
    proc_pid_disarm(kn);
    if (LIST_INSERTED(kn, kn_ready))
        LIST_REMOVE_ZERO(kn, kn_ready);
    if (LIST_EMPTY(&filt->kf_ready))
        kqops.eventfd_lower(&filt->kf_efd);
    tracing_mutex_unlock(&proc_pid_index_mtx);

    return (0);
}

int
evfilt_proc_knote_disable(struct filter *filt, struct knote *kn)
{
    /*
     * Remove the knote from the waiter list but don't free the
     * waiter list itself - re-enable will need it.  Also drop
     * any already-pending delivery from kf_ready: BSD semantics
     * say EV_DISABLE suppresses pending events, not just future
     * ones.  Lower the eventfd if kf_ready becomes empty so the
     * next kevent_wait doesn't fire on a stale level-triggered
     * raise.
     */
    tracing_mutex_lock(&proc_pid_index_mtx);
    if (LIST_INSERTED(kn, kn_proc_waiter))
        LIST_REMOVE_ZERO(kn, kn_proc_waiter);
    if (LIST_INSERTED(kn, kn_ready))
        LIST_REMOVE_ZERO(kn, kn_ready);
    if (LIST_EMPTY(&filt->kf_ready))
        kqops.eventfd_lower(&filt->kf_efd);
    tracing_mutex_unlock(&proc_pid_index_mtx);

    return (0);
}

static int
evfilt_proc_knote_copyout(struct kevent *dst, UNUSED int nevents, struct filter *filt,
    struct knote *kn, UNUSED void *ev)
{
    /*
     * Per-knote copyout, called once by the dispatcher for each
     * knote it dequeues from kf_ready (the dispatcher detaches
     * before invoking us, so we don't iterate kf_ready ourselves).
     * proc_pid_index_mtx still protects kn_proc_status against the
     * wait thread - waiter_notify writes it under the same mutex.
     */
    tracing_mutex_lock(&proc_pid_index_mtx);
    memcpy(dst, &kn->kev, sizeof(*dst));
    dst->fflags = NOTE_EXIT;
    dst->flags |= EV_EOF;
    dst->data   = kn->kn_proc_status;
    tracing_mutex_unlock(&proc_pid_index_mtx);

    if (knote_copyout_flag_actions(filt, kn) < 0)
        return (-1);
    return (1);
}

const struct filter evfilt_proc = {
    .libkqueue_init   = evfilt_proc_libkqueue_init,
    .libkqueue_fork   = evfilt_proc_libkqueue_fork,
    .kf_id            = EVFILT_PROC,
    .kf_init          = evfilt_proc_init,
    .kf_destroy       = evfilt_proc_destroy,
    .kf_copyout       = evfilt_proc_knote_copyout,
    .kn_create        = evfilt_proc_knote_create,
    .kn_modify        = evfilt_proc_knote_modify,
    .kn_enable        = evfilt_proc_knote_create,
    .kn_disable       = evfilt_proc_knote_disable,
    .kn_delete        = evfilt_proc_knote_delete
};
