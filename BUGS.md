 # libkqueue

 This documents the current behavioural differences or defficiencies of
 libkqueue vs the native BSD kqueue implementations.

 ## Common

 * Some functions should crash instead of silently printing a debug
   message.. for example, `note_release()`.

 * There are a number of stub functions that silently fail or succeed.
   These need to be cleaned up; at a minimum, they should emit very loud
   debugging output saying "FIXME -- UNIMPLEMENTED".
   ```
   $ grep STUB src/*/*.c
   src/linux/read.c:    return (-1); /* STUB */
   src/linux/timer.c:    return (0); /* STUB */
   src/linux/vnode.c:    return (-1); /* FIXME - STUB */
   src/linux/write.c:    return (-1); /* STUB */
   src/posix/timer.c:    return (-1); /* STUB */
   src/solaris/socket.c:    return (-1); /* STUB */
   src/solaris/timer.c:    return (-1); /* STUB */
   src/windows/read.c:    return (-1); /* STUB */
   src/windows/timer.c:    return (0); /* STUB */
   ```

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

 * `EVFILT_SIGNAL` - On the signalfd-backed implementation (Linux/illumos)
    a signal must be blocked on every thread that could otherwise take
    delivery, otherwise the kernel runs the disposition before the signal
    lands in the pending queue that signalfd reads from.  POSIX has no
    process-wide block primitive: `pthread_sigmask(2)` (and `sigprocmask(2)`,
    whose behaviour is undefined in multithreaded programs) only sets the
    mask on the calling thread.  When the first knote for a signal is
    registered, libkqueue blocks that signal on the registering thread;
    threads spawned afterwards inherit the mask, but pre-existing threads
    are unaffected.  Applications that register `EVFILT_SIGNAL` after
    spawning worker threads must `pthread_sigmask(SIG_BLOCK, ...)` the
    monitored signals on those threads themselves.  Same constraint the
    POSIX `EVFILT_PROC` waiter has for `SIGCHLD`.

 * `EVFILT_SIGNAL` - On the signalfd-backed implementation, thread-targeted
    signals (`pthread_kill`, `tgkill`) sent to a thread other than libkqueue's
    internal signal dispatch thread will NOT be observed via kqueue.  The
    kernel routes thread-targeted signals into the target thread's per-thread
    pending queue, which only that thread's signalfd reader can drain.
    Process-targeted signals (`kill(getpid(), ...)`) land in the per-process
    pending queue where the dispatcher sees them normally.  Native BSD/macOS
    kqueue is in-kernel and observes both.

 ## Linux

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

 * Solaris unit test failure.
   ```
   LD_LIBRARY_PATH="..:/usr/sfw/lib/64" ./kqtest
   1: test_peer_close_detection()
   2: test_kqueue()
   3: test_kevent_socket_add()
   4: test_kevent_socket_del()
   5: test_kevent_socket_add_without_ev_add()
   6: test_kevent_socket_get()

   [read.c:84]: Unexpected event:_test_no_kevents(): [ident=7, filter=-1, flags = 1 (EV_ADD), fflags = 0, data=0, udata=fffffd7fff08c6b4]: Error 0
   ```
  * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.

 * We need to uninitialize library after `fork()` `using pthread_atfork()`.
   BSD kqueue file descriptors are not inherited by the fork copy and
   will be closed automatically in the fork.  With libkqueue, because
   we don't unitialize the library, kqueue file descriptors will persist
   in the fork.

 ## Windows
 
 * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.
