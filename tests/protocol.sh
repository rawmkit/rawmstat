#!/bin/sh
set -eu

rawmstat=${1:-./rawmstat}
signal_number=${2:-./tests/signal-number}
tmp=${TMPDIR:-/tmp}/rawmstat-protocol-test.$$
mkdir -p "$tmp"
pid=
xvfb=
cleanup()
{
  [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
  [ -z "$xvfb" ] || kill "$xvfb" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

skip()
{
  echo "rawmstat protocol tests: skipped ($*)"
  exit 0
}

fail()
{
  echo "rawmstat protocol test failed: $*" >&2
  exit 1
}

for tool in Xvfb xprop; do
  command -v "$tool" >/dev/null 2>&1 || skip "$tool not found"
done

[ -x "$signal_number" ] || skip "$signal_number not built"
if ! update_signal=$("$signal_number" 1); then
  skip "realtime signals unavailable"
fi

printf '%s' one >"$tmp/value"
cat >"$tmp/rawmstat.conf" <<EOF2
status = { protocol = "rawm-v2"; };
blocks = (
  {
    name = "probe";
    prefix = "p:";
    command = ("cat", "$tmp/value");
    interval = 0;
    signal = 1;
  }
);
EOF2

display_file=$tmp/display
: >"$display_file"
Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp \
  >"$tmp/xvfb.log" 2>&1 3>"$display_file" &
xvfb=$!
n=0
while [ ! -s "$display_file" ]; do
  n=$((n + 1))
  [ "$n" -lt 100 ] || fail "Xvfb did not select a display"
  sleep 0.02
done
DISPLAY=":$(cat "$display_file")"
export DISPLAY

"$rawmstat" -c "$tmp/rawmstat.conf" >"$tmp/stdout" 2>"$tmp/stderr" &
pid=$!

wait_property()
{
  expected=$1
  n=0
  while :; do
    value=$(xprop -notype -root _RAWM_STATUS_V2 2>/dev/null || true)
    printf '%s\n' "$value" | grep -Fq "\"$expected\"" && return 0
    n=$((n + 1))
    [ "$n" -lt 100 ] || return 1
    sleep 0.02
  done
}

property_payload()
{
  xprop -notype -root _RAWM_STATUS_V2 2>/dev/null |
    sed -n 's/^[^\"]*\"\(.*\)\"$/\1/p'
}

wait_property 'probe\tnormal\tp:one\n' || {
  cat "$tmp/stderr" >&2
  fail "initial rawm-v2 property was not published"
}
xprop -root _RAWM_STATUS_V2 | grep -q '(UTF8_STRING)' ||
  fail "status property does not use UTF8_STRING"

# A recognized semantic prefix is consumed by rawmstat and becomes the
# segment state rather than presentation markup in the text.
printf 'warning\ttwo' >"$tmp/value"
kill -"$update_signal" "$pid"
wait_property 'probe\twarning\tp:two\n' || {
  cat "$tmp/stderr" >&2
  fail "warning state was not published"
}

printf 'critical\tthree' >"$tmp/value"
kill -"$update_signal" "$pid"
wait_property 'probe\tcritical\tp:three\n' || {
  cat "$tmp/stderr" >&2
  fail "critical state was not published"
}

# Unknown state-like prefixes leave a tab in the sample text and are invalid;
# the last successful sample must remain published.
printf 'unknown\tbad' >"$tmp/value"
kill -"$update_signal" "$pid"
n=0
while ! grep -q "sample text must be printable UTF-8" "$tmp/stderr"; do
  n=$((n + 1))
  [ "$n" -lt 100 ] || fail "invalid semantic sample was not rejected"
  sleep 0.02
done
wait_property 'probe\tcritical\tp:three\n' ||
  fail "invalid semantic sample replaced the last good value"

# One probe record has 14 bytes of wire framing, and this test block adds a
# two-byte prefix. 4080 command-output bytes therefore produce the exact
# 4096-byte rawm-v2 boundary payload.
dd if=/dev/zero bs=4080 count=1 2>/dev/null | tr '\000' x >"$tmp/value"
kill -"$update_signal" "$pid"
n=0
while :; do
  payload=$(property_payload)
  [ "${#payload}" -eq 4099 ] && break # xprop escapes two tabs and newline
  n=$((n + 1))
  [ "$n" -lt 100 ] || {
    cat "$tmp/stderr" >&2
    fail "4096-byte rawm-v2 payload was not published"
  }
  sleep 0.02
done

printf x >>"$tmp/value"
kill -"$update_signal" "$pid"
n=0
while ! grep -q "composed status exceeds rawm-v2 4096-byte limit" "$tmp/stderr"; do
  n=$((n + 1))
  [ "$n" -lt 100 ] || fail "oversized rawm-v2 payload was not rejected"
  sleep 0.02
done
payload=$(property_payload)
[ "${#payload}" -eq 4099 ] || fail "oversized sample replaced last valid rawm-v2 payload"

kill -TERM "$pid"
wait "$pid" || fail "rawmstat did not terminate cleanly"
pid=

echo "rawmstat protocol tests: ok"
