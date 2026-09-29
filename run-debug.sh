#!/bin/sh
#
# Run the dedicated server under lldb and capture a real backtrace on SIGSEGV.
#
# The engine's own crash handler cannot unwind the xash3d binary (it ships
# without debug info), so it prints "no debug info in Mach-O executable" for
# every frame. lldb uses the real unwinder and the symbol table in the server
# dylib, so it names the actual function instead.
#
# Usage:
#   ./run-debug.sh
#   ./run-debug.sh +map crossfire
#
# On a crash the backtrace lands in /tmp/zpmod_crash.txt and the full session
# log in /tmp/zpmod_lldb.log. Both are safe to paste.

set -e

cd "$(dirname "$0")/game"

# Let the process dump core on a fault.
ulimit -c unlimited 2>/dev/null || true

# The dedicated server cannot run two instances on one port: the second exits
# with "Host_ErrorInit: Couldn't allocate IPv4 and IPv6 server ports" and exit
# status 1. That is NOT a crash, so pick a port that is free right now and fail
# early with a clear message rather than half-starting.
PORT=""
for candidate in 27015 27016 27017 27018 27019; do
  if ! lsof -nP -iTCP:$candidate -sTCP:LISTEN >/dev/null 2>&1; then
    PORT=$candidate
    break
  fi
done

if [ -z "$PORT" ]; then
  echo "All of 27015-27019 are in use. Stop the other server first:" >&2
  pgrep -fl xash3d >&2 || true
  exit 1
fi

if [ "$PORT" != "27015" ]; then
  echo "port 27015 busy, using $PORT"
  echo "connect your client to 127.0.0.1:$PORT"
fi

rm -f /tmp/zpmod_crash.txt /tmp/zpmod_bt.txt /tmp/zpmod_regs.txt
mv /tmp/zpmod_debug.log /tmp/zpmod_debug.log.prev 2>/dev/null || true

# -b runs the given -o commands in order, then quits. On a fault lldb stops, the
# backtrace commands run, and we get a real frame list with real symbol names.
# The engine's own handler cannot do this: it ships without debug info and
# prints "no debug info in Mach-O executable" for every frame.
#
# The backtrace and register dumps go to their own files as well as stdout, so
# the frame list survives even if the terminal is cleared or the session is
# interrupted before the output scrolls past.
lldb -b -o \
  "settings set target.process.stop-on-sharedlibrary-events false" \
  -o "process handle SIGSEGV -s true -n false -p true" \
  -o "process handle SIGBUS -s true -n false -p true" \
  -o "process handle SIGABRT -s true -n false -p true" \
  -o "run" \
  -o "thread backtrace all -e true" \
  -o "image lookup -a \$pc -v" \
  -o "register read" \
  -o "quit" \
  -- ./xash3d -dedicated -port $PORT +map crossfire \
  2>&1 | tee /tmp/zpmod_lldb.log || true

# Slice the post-stop output out into standalone files. The engine dylib and
# libxash.dylib both ship __unwind_info, so the unwinder can cross the engine
# boundary and name the real caller; only xash3d itself is a 34KB stub.
sed -n '/thread backtrace all/,$p' /tmp/zpmod_lldb.log > /tmp/zpmod_bt.txt 2>/dev/null || true
sed -n '/register read/,$p' /tmp/zpmod_lldb.log > /tmp/zpmod_regs.txt 2>/dev/null || true

# Distinguish a real fault from an ordinary exit.
#
# Do NOT grep for "SIGSEGV" here. lldb echoes this script's own
# "process handle SIGSEGV ..." setup command back into the log, along with a
# table of signal names, so any grep for SIGSEGV/SIGBUS/SIGABRT matches on a
# perfectly clean run and reports a crash that never happened. Strip everything
# up to and including the "run" command first, then look only at what lldb
# printed while the process was actually running.
sed -n '/^(lldb) run$/,$p' /tmp/zpmod_lldb.log > /tmp/zpmod_lldb.run.log

# "Process N stopped" is lldb announcing it caught a signal, and "stop reason"
# is how it reports one. Both only appear on a real fault.
if grep -qE 'Process [0-9]+ stopped|stop reason|EXC_BAD_ACCESS' /tmp/zpmod_lldb.run.log; then
  {
    echo "=== xash3d crash capture $(date) ==="
    echo
    echo "--- server debug log (tail) ---"
    tail -60 /tmp/zpmod_debug.log 2>/dev/null || echo "(no debug log)"
    echo
    echo "--- guarded MODEL_INDEX hits ---"
    cat /tmp/zpmod_idx.log 2>/dev/null || echo "(none: no MODEL_INDEX call was given a bad name)"
    echo
    echo "--- backtrace ---"
    cat /tmp/zpmod_bt.txt 2>/dev/null || echo "(no backtrace captured)"
  } > /tmp/zpmod_crash.txt
  echo
  echo "CRASH CAPTURED -> /tmp/zpmod_crash.txt"
else
  rm -f /tmp/zpmod_crash.txt
  echo "no crash. full session log in /tmp/zpmod_lldb.log"
  if grep -q "Couldn't allocate IPv4 and IPv6 server ports" /tmp/zpmod_lldb.run.log; then
    echo "the server exited because port $PORT was still taken - stop the old one and retry"
  fi
fi
