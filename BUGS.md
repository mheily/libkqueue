 # libkqueue

 This documents the current behavioural differences or defficiencies of
 libkqueue vs the native BSD kqueue implementations.

 ## Common

 * A handful of stub functions still silently fail or succeed instead of
   emitting `FIXME -- UNIMPLEMENTED`.  Run `grep STUB src/*/*.c` for the
   current list.

 * `fork(2)` without a subsequent `exec(3)` leaks libkqueue's internal
   heap allocations in the child.  POSIX restricts `pthread_atfork`
   child handlers to async-signal-safe functions, and `free(3)` is not
   AS-safe, so the child handler can only `close(2)` inherited file
   descriptors (kqueue's `epollfd` on Linux, `pipefd[0..1]`,
   `signalfd` for EVFILT_SIGNAL, etc.) but cannot release the
   per-knote / per-filter heap state.  Per-knote `pidfd`s and per-
   filter `eventfd`s are similarly orphaned because walking the
   knote tree to close them would require malloc-protected lock
   acquisitions.
   The accepted contract is: a forked child must `exec()` (which
   replaces the address space) or `_exit()` shortly afterward.  This
   matches FreeBSD native kqueue behaviour.  Running libkqueue in a
   long-lived forked child without `exec()` is unsupported.

 ## POSIX

 * `EVFILT_PROC` - The POSIX implmentation requires that `SIGCHLD`
    be delivered to its global waiter thread so that the waiter can discover a
    when child process exits.  To prevent `SIGCHLD` being delivered to another
    thread `sigprocmask(2)` is used to mask `SIGCHLD` at a process level.  
    If the application unmasks `SIGCHLD` or installs a handler for it,
    the POSIX `EVFILT_PROC` code will not function.

 * `EVFILT_PROC` - If using the POSIX `EVFILT_PROC` the number of monitored
    processes should be kept low (< 100).  Because the Linux kernel coalesces
    `SIGCHLD` (and other signals), the only way to reliably determine if a
    monitored process has exited, is to loop through all PIDs registered by any
    kqueue when we receive a `SIGCHLD`.  This involves many calls to `waitid(2)`
    and may have a negative performance impact.

 * `EVFILT_PROC` - The notification list of the global waiter thread and the
    ready lists of individual kqueues share the same mutex.  This may cause
    performance issues where large numbers of processes are monitored.

 ## Linux

 * `EVFILT_SIGNAL` - The signalfd-backed implementation requires a signal to
    be blocked on every thread that could otherwise take delivery, otherwise
    the kernel runs the disposition before the signal lands in the pending
    queue that signalfd reads from.  POSIX has no process-wide block
    primitive: `pthread_sigmask(2)` (and `sigprocmask(2)`, whose behaviour
    is undefined in multithreaded programs) only sets the mask on the
    calling thread.  When the first knote for a signal is registered,
    libkqueue blocks that signal on the registering thread; threads spawned
    afterwards inherit the mask, but pre-existing threads are unaffected.
    Applications that register `EVFILT_SIGNAL` after spawning worker
    threads must `pthread_sigmask(SIG_BLOCK, ...)` the monitored signals
    on those threads themselves.

 * `EVFILT_SIGNAL` - Thread-targeted signals (`pthread_kill`, `tgkill`)
    sent to a thread other than libkqueue's internal signal dispatch thread
    will NOT be observed via kqueue.  The kernel routes thread-targeted
    signals into the target thread's per-thread pending queue, which only
    that thread's signalfd reader can drain.  Process-targeted signals
    (`kill(getpid(), ...)`) land in the per-process pending queue where
    the dispatcher sees them normally.  Native BSD/macOS kqueue is
    in-kernel and observes both.

 * `EVFILT_SIGNAL` on `SIGCHLD` is rejected with `EINVAL` when libkqueue
    is built without `pidfd_open(2)` (kernels < 5.3, or builds where the
    syscall is unavailable).  In that configuration the POSIX `EVFILT_PROC`
    waiter `sigwaitinfo`s `SIGCHLD` to detect child exits; permitting
    `EVFILT_SIGNAL` to also watch it would race the two consumers for each
    fire, with the loser observing nothing.  Builds with `pidfd_open(2)`
    use the pidfd-based `EVFILT_PROC` backend, where this constraint
    doesn't apply.

 * `EVFILT_SIGNAL` on `SIGRTMIN+1` is rejected with `EINVAL`.  The
    monitoring thread `sigwaitinfo`s this signal to detect kqueue fd
    closures (set per-pipefd via `fcntl(F_SETSIG)`); a parallel
    `EVFILT_SIGNAL` reader would race it and leave kqueue cleanup blind
    to closures.

 * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.  On Linux this is pretty much impossible to
   fix as there's no mechanism to retrieve file descriptor reference
   counts and no way to be notified of when a file descriptor is closed.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.

 * `EVFILT_PROC` - Only `NOTE_EXIT` is currently supported.  Other
   functionality is possible, but it requires integrating with netlink to
   receive notifications.
   If building against Kernels < 5.3 (where `pidfd_open()` is not available)
   the POSIX `EVFILT_PROC` code is used.  See the POSIX section above for
   limitations of the POSIX `EVFILT_PROC` code.

  * `EVFILT_PROC` - Native kqueue provides notifications for any process that
   is visible to the application process.  On Linux/POSIX platforms only direct
   children of the application process can be monitored for exit.

 ## Solaris

 * `EVFILT_SIGNAL` - The signalfd-backed implementation requires a signal to
    be blocked on every thread that could otherwise take delivery, otherwise
    the kernel runs the disposition before the signal lands in the pending
    queue that signalfd reads from.  POSIX has no process-wide block
    primitive: `pthread_sigmask(2)` (and `sigprocmask(2)`, whose behaviour
    is undefined in multithreaded programs) only sets the mask on the
    calling thread.  When the first knote for a signal is registered,
    libkqueue blocks that signal on the registering thread; threads spawned
    afterwards inherit the mask, but pre-existing threads are unaffected.
    Applications that register `EVFILT_SIGNAL` after spawning worker
    threads must `pthread_sigmask(SIG_BLOCK, ...)` the monitored signals
    on those threads themselves.

 * `EVFILT_SIGNAL` - Thread-targeted signals (`pthread_kill`, `tgkill`)
    sent to a thread other than libkqueue's internal signal dispatch thread
    will NOT be observed via kqueue.  The kernel routes thread-targeted
    signals into the target thread's per-thread pending queue, which only
    that thread's signalfd reader can drain.  Process-targeted signals
    (`kill(getpid(), ...)`) land in the per-process pending queue where
    the dispatcher sees them normally.  Native BSD/macOS kqueue is
    in-kernel and observes both.

 * `EVFILT_SIGNAL` on `SIGCHLD` is rejected with `EINVAL`.  illumos has no
    `pidfd_open(2)` equivalent so the POSIX `EVFILT_PROC` waiter
    `sigwaitinfo`s `SIGCHLD` to detect child exits; permitting
    `EVFILT_SIGNAL` to also watch it would race the two consumers for each
    fire, with the loser observing nothing.

 * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.

 ## Windows
 
 * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.
