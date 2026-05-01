# TODO

## kevent() cancellation cleanup handler is buggy

`src/common/kevent.c:kevent_release_kq_mutex` (cleanup handler pushed
via `pthread_cleanup_push`) has two known bugs that fire if a thread
gets cancelled mid-`kevent()`:

1. It does not call `kqueue_kevent_exit`, so the in-flight tracking
   entry stays in `kq_inflight` forever.  Permanently blocks the
   deferred-free sweep on that kqueue.
2. It always calls `kqueue_unlock`, but on `KEVENT_WAIT_DROP_LOCK`
   platforms (Linux, Solaris) the lock is dropped during
   `kevent_wait`, which is the only window where cancellation is
   enabled.  So the unlock is wrong every time it fires.

Workaround in tree today: the comment tells callers to avoid
pthread cancellation around `kevent()`.

Fix sketch:

```c
struct kevent_cleanup_state {
    struct kqueue *kq;
    struct kqueue_kevent_state *inflight; /* non-NULL between enter and exit */
    bool kq_locked;                       /* true while we hold kq_mtx */
};

static void kevent_cleanup_handler(void *arg) {
    struct kevent_cleanup_state *cs = arg;
    if (cs->inflight)
        kqueue_kevent_exit(cs->kq, cs->inflight);
    if (cs->kq_locked)
        kqueue_unlock(cs->kq);
}
```

In `kevent()`:
- Maintain `cs.kq_locked` around every `kqueue_lock` / `kqueue_unlock`.
- Maintain `cs.inflight` around `kqueue_kevent_enter` / `kqueue_kevent_exit`.
- Cleanup must call `kevent_exit` before `kqueue_unlock` (exit
  asserts kq_mtx held).

## Filters arm in `kn_create` before `EV_DISABLE` is honoured

`src/common/kevent.c:kevent_copyin_one` handles `EV_ADD | EV_DISABLE`
by calling `filt->kn_create` (which registers with the kernel) and
*then* `filt->kn_disable`.  An event can fire on a knote that was
registered "disabled" during the window between those two calls.

Real fix is a "starts disabled" flag threaded through `kn_create`
across all filters so the kernel registration is created in a
non-firing state from the start.

`EVFILT_USER` is unaffected: its `kn_create` doesn't arm anything
and the trigger path in `kn_modify` checks `EV_DISABLE`.

Affected `kn_create` implementations (all carry an inline `TODO`
pointing at this list):

- `src/common/evfilt_signal.h` `evfilt_signal_knote_create`
- `src/linux/proc.c` `evfilt_proc_knote_create`
- `src/linux/read.c` `evfilt_read_knote_create`
- `src/linux/timer.c` `evfilt_timer_knote_create`
- `src/linux/vnode.c` `evfilt_vnode_knote_create`
- `src/linux/write.c` `evfilt_write_knote_create`
- `src/posix/proc.c` `evfilt_proc_knote_create`
- `src/posix/read.c` `evfilt_read_knote_create`
- `src/posix/timer.c` `evfilt_timer_knote_create`
- `src/posix/write.c` `evfilt_write_knote_create`
- `src/solaris/socket.c` `evfilt_socket_knote_create` (covers `EVFILT_READ` + `EVFILT_WRITE`)
- `src/solaris/timer.c` `evfilt_timer_knote_create`
- `src/solaris/vnode.c` `evfilt_vnode_knote_create`

## knote comparator ignores `kev.udata` (no `EV_UDATA_SPECIFIC`)

`src/common/knote.c:knote_cmp` keys the per-filter knote index by
`kev.ident` only.  BSD's `EV_UDATA_SPECIFIC` flag distinguishes
knotes by `(ident, udata)` so the same ident can have multiple
distinct registrations.  Implementing it requires:

- Promoting the comparator to `(ident, udata)` lexicographic.
- Threading the flag through the lookup paths so callers that don't
  set it keep getting the legacy "first match by ident" semantics.
- Auditing every `knote_lookup` site for the right comparison key.

Not a small change - parked here.

## `NOTE_LOWAT` divergence from BSD

Linux (`src/linux/read.c`, `src/linux/write.c`) implements `NOTE_LOWAT`
via `setsockopt(SO_RCVLOWAT|SO_SNDLOWAT)`.  That is a socket-wide
setting, so two knotes on the same fd with different thresholds will
clobber each other (last writer wins) and the threshold lingers on
the socket between unrelated knote registrations until `EV_DELETE`
restores it to 1.

Solaris does not implement `NOTE_LOWAT` at all.  illumos rejects
`setsockopt(SO_RCVLOWAT|SO_SNDLOWAT)` with `ENOPROTOOPT`, and
`PORT_SOURCE_FD` is level-triggered, so userspace gating in
`copyout` would busy-loop (re-arm wakes immediately because data
is still queued).  `evfilt_socket_knote_create` silently ignores
the flag.

BSD's native kqueue stores the threshold in `kn_sdata` and gates
delivery in `filt_soread` / `filt_sowrite`, so each knote has its
own independent threshold and the socket option is untouched.

## `EVFILT_READ` / `EVFILT_WRITE` on regular files broken on Solaris

`solaris_get_descriptor_type` (`src/solaris/socket.c`) accepts `S_IFREG`
and tags `KNFL_FILE`, then `evfilt_socket_knote_create` routes through
`port_associate(PORT_SOURCE_FD, fd, POLLIN/POLLOUT)`.  illumos's
`PORT_SOURCE_FD` is poll(2)-driven; regular files are always
poll-ready, so this either fires a storm of events or never wakes.
There's also no equivalent of Linux's "compute offset-to-EOF" path
for `EVFILT_READ` on regular files; `FIONREAD` on a regular fd
doesn't return that.

Linux uses a per-knote surrogate `eventfd` to represent regular-file
readiness; Solaris would need an equivalent (a sub-port that we
trigger after fstat says the file grew, or fall back to PORT_SOURCE_FILE
+ fobj baseline).  Untested in current tree.

## `EV_CLEAR` doesn't re-arm on Solaris

`PORT_SOURCE_FD` is one-shot per delivery (kernel auto-dissociates
on each fire).  `solaris_kevent_copyout` deliberately skips re-arm
for `EV_CLEAR` knotes (`src/solaris/platform.c`), so after the first
event an `EV_CLEAR` knote is permanently silent.

BSD's `EV_CLEAR` is edge-triggered: the knote still fires on
subsequent edges, just doesn't repeat for the same edge.  Linux
gets it right via `EPOLLET`.

Emulating this on Solaris would mean re-associating after each
delivery and suppressing duplicate fires for a still-ready fd,
but `port_associate` fires immediately if the condition is already
true, so a level-true fd would refire.  No clean fix without
tracking last-delivered state per knote.

## `EVFILT_VNODE` follows path, not fd, on Solaris

`src/solaris/vnode.c` resolves `/proc/self/path/<fd>` into a string
at arm time and watches the path via `PORT_SOURCE_FILE`.  If the
file is renamed after arm, re-arm uses the cached old path which
no longer exists and `port_associate` fails.  Unlinked-but-still-open
fds also don't resolve through `/proc/self/path`.

BSD's `EVFILT_VNODE` follows the inode through the fd; rename
keeps the watch alive.  illumos has no fd-based file-watch
primitive, so this is structural.

## Listening-socket `data` is hardcoded `1` on Solaris

`evfilt_socket_copyout` (`src/solaris/socket.c`) reports
`dst->data = 1` for `KNFL_SOCKET_PASSIVE` instead of the listen
backlog depth.  illumos has no accept-queue-depth ioctl.  BSD
reports the actual backlog.

## `kqops.filter_init` takes non-const filter pointer

`src/common/filter.c:filter_register` casts away `const` from `src`
when calling `kqops.filter_init(kq, dst)`.  The vtable signature
should take `const struct filter *` consistently.  Refactor across
all backends.
