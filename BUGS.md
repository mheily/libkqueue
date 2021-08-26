 # libkqueue (userland)
 
 This documents the current behavioural differences or defficiencies of
 libkqueue vs the native BSD kqueue implementations.
 
 ## Common
 
 * We need to uninitialize library after `fork()` `using pthread_atfork()`.
   BSD kqueue file descriptors are not inherited by the fork copy and
   will be closed automatically in the fork.  With libkqueue, because
   we don't unitialize the library, kqueue file descriptors will persist
   in the fork.
   
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
      
 * `kqueue()` should defer thread cancellation until the end.

 * `kevent()` should defer thread cancellation and call `pthread_testcancel()`
   before and after the call to `kevent_wait()`. This may require changing the
   way that `EINTR` is handled, to make sure that the `EINTR` is propagated up
   the call stack to `kevent()`.
      
 ## Linux
 
  * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up.  On Linux this is pretty much impossible to
   fix as there's no mechanism to retrieve file descriptor reference
   counts, and no way to be notified of when a file descriptor is closed.
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.
   
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
   
 ## Windows
 
 * On Windows, you need to supply `-DMAKE_STATIC` in `CFLAGS` when building the
   static library. This does not apply when using cmake.
   
 * If a file descriptor outside of kqueue is closed, the internal kqueue
   state is not cleaned up. 
   Applications should ensure that file descriptors are removed from
   the kqueue before they are closed.

# libkqueue (kernel)

 * When passing a knote pointer to the kernel, the reference count of
   the knote structure should be incremented. Conversely, when the pointer
   has been returned from the kernel and the event unregistered from the
   kernel, the reference count should be decremented.