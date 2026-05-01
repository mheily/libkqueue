#!/bin/sh
#
# Per-OS thread-stack dump for the test watchdog.  Invoked as
# `test-stack-dump.sh <pid>` (the watchdog appends the parent pid).
#
# Requirements:
#   illumos/Solaris  pstack  (in /usr/bin, ships with the OS)
#   Linux            eu-stack from elfutils (apt: elfutils, dnf: elfutils)
#   FreeBSD          gdb (pkg: devel/gdb)
#   macOS            sample(1) (ships with the OS)
#
set -u
pid="${1:?usage: $0 <pid>}"

# ooce installs valgrind/vgdb under /opt/ooce/valgrind/bin and gdb
# under /opt/ooce/gdb-NN/bin; neither directory is on the default
# illumos PATH.  Add both unconditionally - on other OSes these
# directories don't exist so it's harmless.
PATH="/opt/ooce/valgrind/bin:/opt/ooce/bin:${PATH:-}"
for d in /opt/ooce/gdb-*/bin; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH

echo "test-stack-dump: pid=$pid os=$(uname -s) PATH=$PATH"

# Run "$@" in the background, kill it after $1 seconds.  Returns the
# command's exit status, or 124 if it timed out.  Avoids depending on
# GNU timeout(1) which isn't always present on illumos.
run_timeout() {
    secs=$1; shift
    "$@" &
    cmd_pid=$!
    ( sleep "$secs"; kill -KILL "$cmd_pid" 2>/dev/null ) &
    killer_pid=$!
    wait "$cmd_pid" 2>/dev/null
    rc=$?
    kill "$killer_pid" 2>/dev/null
    wait "$killer_pid" 2>/dev/null
    return $rc
}

# If the target process is running under valgrind, native stack
# dumpers (pstack, eu-stack) only show valgrind's scheduler frames.
# Use gdb via vgdb to walk the inferior's logical threads.  Both
# the probe and the attach can hang if valgrind's gdbserver is
# wedged, so cap each at 10 seconds.
if command -v vgdb >/dev/null 2>&1; then
    echo "test-stack-dump: vgdb found; listing known valgrind processes"
    vgdb -l 2>&1 || true
    echo "test-stack-dump: probing pid=$pid for gdbserver (vgdb stderr below)"
    run_timeout 10 vgdb --pid="$pid" v.info scheduler
    rc=$?
    echo "test-stack-dump: probe exit=$rc"
    if [ "$rc" -eq 0 ]; then
        echo "test-stack-dump: target is under valgrind, attaching gdb via vgdb"
        run_timeout 30 gdb -batch \
            -ex 'set pagination off' \
            -ex "target remote | vgdb --pid=$pid" \
            -ex 'thread apply all bt' \
            -ex 'detach' || \
            echo "test-stack-dump: gdb-via-vgdb failed (rc=$?)"
    fi
    # Fall through to pstack as well so we always get _something_;
    # pstack on a valgrind process at least shows which valgrind
    # threads are active and where they're parked.
else
    echo "test-stack-dump: vgdb not in PATH"
fi

case "$(uname -s)" in
    SunOS)
        exec /usr/bin/pstack "$pid"
        ;;
    Linux)
        if command -v eu-stack >/dev/null 2>&1; then
            exec eu-stack -p "$pid"
        fi
        exec gdb -batch -ex 'set pagination off' -ex 'thread apply all bt' -p "$pid"
        ;;
    FreeBSD|OpenBSD)
        exec gdb -batch -ex 'set pagination off' -ex 'thread apply all bt' -p "$pid"
        ;;
    Darwin)
        exec sample "$pid" 2 -mayDie
        ;;
    *)
        echo "test-stack-dump: unknown OS $(uname -s)" >&2
        exit 1
        ;;
esac
