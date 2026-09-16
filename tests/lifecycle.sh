#!/bin/sh
set -eu

rawmstat=${1:-./rawmstat}
signal_number=${2:-./tests/signal-number}
tmp=${TMPDIR:-/tmp}/rawmstat-lifecycle-test.$$
mkdir -p "$tmp"
pid=
cleanup()
{
  [ -z "$pid" ] || kill -KILL "$pid" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

fail()
{
  echo "rawmstat lifecycle test failed: $*" >&2
  [ ! -f "$tmp/stderr" ] || cat "$tmp/stderr" >&2
  exit 1
}

wait_for()
{
  limit=$1
  shift
  n=0
  while ! "$@"; do
    n=$((n + 1))
    [ "$n" -lt "$limit" ] || return 1
    sleep 0.02
  done
}

wait_line()
{
  text=$1
  file=$2
  grep -Fxq "$text" "$file" 2>/dev/null
}

[ -x "$signal_number" ] || {
  echo "rawmstat lifecycle tests: skipped ($signal_number not built)"
  exit 0
}
if ! update_signal=$($signal_number 1); then
  echo "rawmstat lifecycle tests: skipped (realtime signals unavailable)"
  exit 0
fi

# Slow commands do not block independent blocks.
cat >"$tmp/async.conf" <<'EOC'
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "slow";
    command = ("/bin/sh", "-c", "sleep 2; printf slow");
    interval = 0;
    timeout_ms = 5000;
  },
  {
    name = "fast";
    command = ("printf", "fast");
    interval = 0;
    timeout_ms = 1000;
  }
);
EOC
: >"$tmp/stdout"
: >"$tmp/stderr"
"$rawmstat" -p -c "$tmp/async.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!
wait_for 50 wait_line fast "$tmp/stdout" || fail "slow block stalled fast block"
kill -TERM "$pid"
wait "$pid" || fail "async test did not terminate cleanly"
pid=

# Timeout owns and kills the complete child process group.
cat >"$tmp/timeout.conf" <<EOF2
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "hung";
    command = ("/bin/sh", "-c", "sleep 30 & echo \\\$! > '$tmp/child.pid'; wait");
    interval = 0;
    timeout_ms = 100;
  }
);
EOF2
: >"$tmp/stdout"
: >"$tmp/stderr"
"$rawmstat" -p -c "$tmp/timeout.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!
wait_for 50 test -s "$tmp/child.pid" || fail "hung block did not start child"
wait_for 100 grep -q "timed out" "$tmp/stderr" || fail "timeout was not reported"
child=$(cat "$tmp/child.pid")
wait_for 100 sh -c "! kill -0 '$child' 2>/dev/null" || fail "timeout left descendant process alive"
kill -TERM "$pid"
wait "$pid" || fail "timeout test did not terminate cleanly"
pid=

# A failed refresh retains the previous successful sample.
cat >"$tmp/sample.sh" <<EOF2
#!/bin/sh
value=\$(cat '$tmp/value')
if [ "\$value" = fail ]; then
  printf bad
  exit 7
fi
printf '%s' "\$value"
EOF2
chmod +x "$tmp/sample.sh"
printf '%s' good1 >"$tmp/value"
cat >"$tmp/failure.conf" <<EOF2
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "sample";
    command = ("$tmp/sample.sh");
    interval = 0;
    signal = 1;
    timeout_ms = 1000;
  }
);
EOF2
: >"$tmp/stdout"
: >"$tmp/stderr"
"$rawmstat" -p -c "$tmp/failure.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!
wait_for 100 wait_line good1 "$tmp/stdout" || fail "initial successful sample missing"
printf '%s' fail >"$tmp/value"
kill -"$update_signal" "$pid"
wait_for 100 grep -q "exited with status 7" "$tmp/stderr" || fail "failed sample was not reported"
if grep -Fq bad "$tmp/stdout"; then
  fail "failed sample replaced previous value"
fi
printf '%s' good2 >"$tmp/value"
kill -"$update_signal" "$pid"
wait_for 100 wait_line good2 "$tmp/stdout" || fail "successful recovery sample missing"
kill -TERM "$pid"
wait "$pid" || fail "failure retention test did not terminate cleanly"
pid=

# Repeated requests while a block is running coalesce into one pending rerun.
printf '0\n' >"$tmp/count"
cat >"$tmp/coalesce.sh" <<EOF2
#!/bin/sh
n=\$(cat '$tmp/count')
n=\$((n + 1))
printf '%s\n' "\$n" >'$tmp/count'
sleep 0.25
printf '%s' "\$n"
EOF2
chmod +x "$tmp/coalesce.sh"
cat >"$tmp/coalesce.conf" <<EOF2
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "coalesce";
    command = ("$tmp/coalesce.sh");
    interval = 0;
    signal = 1;
    timeout_ms = 2000;
  }
);
EOF2
: >"$tmp/stdout"
: >"$tmp/stderr"
"$rawmstat" -p -c "$tmp/coalesce.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!
wait_for 50 grep -qx 1 "$tmp/count" || fail "coalescing block did not start"
kill -"$update_signal" "$pid"
sleep 0.05
kill -"$update_signal" "$pid"
wait_for 100 wait_line 2 "$tmp/stdout" || fail "pending rerun did not execute"
sleep 0.35
[ "$(cat "$tmp/count")" = 2 ] || fail "refresh requests spawned more than one pending rerun"
kill -TERM "$pid"
wait "$pid" || fail "coalescing test did not terminate cleanly"
pid=

# SIGHUP restarts in place and reloads configuration.
cat >"$tmp/reload.conf" <<'EOC'
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = ({ name = "reload"; command = ("printf", "one"); interval = 0; timeout_ms = 1000; });
EOC
: >"$tmp/stdout"
: >"$tmp/stderr"
"$rawmstat" -p -c "$tmp/reload.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!
original_pid=$pid
wait_for 100 wait_line one "$tmp/stdout" || fail "pre-reload sample missing"
cat >"$tmp/reload.conf" <<'EOC'
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = ({ name = "reload"; command = ("printf", "two"); interval = 0; timeout_ms = 1000; });
EOC
kill -HUP "$pid"
wait_for 100 wait_line two "$tmp/stdout" || fail "SIGHUP did not reload configuration"
[ "$pid" = "$original_pid" ] || fail "SIGHUP changed process id"
kill -TERM "$pid"
wait "$pid" || fail "reload test did not terminate cleanly"
pid=

echo "rawmstat lifecycle tests: ok"
