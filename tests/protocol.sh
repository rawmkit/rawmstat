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
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "probe";
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
    value=$(xprop -notype -root _RAWM_STATUS_V1 2>/dev/null || true)
    printf '%s\n' "$value" | grep -Fq "\"$expected\"" && return 0
    n=$((n + 1))
    [ "$n" -lt 100 ] || return 1
    sleep 0.02
  done
}

wait_property one || {
  cat "$tmp/stderr" >&2
  fail "initial rawm-v1 property was not published"
}
xprop -root _RAWM_STATUS_V1 | grep -q '(UTF8_STRING)' ||
  fail "status property does not use UTF8_STRING"


printf '%s' two >"$tmp/value"
kill -"$update_signal" "$pid"
wait_property two || {
  cat "$tmp/stderr" >&2
  fail "signal-triggered block update was not published"
}

property_length()
{
  payload=$(xprop -notype -root _RAWM_STATUS_V1 2>/dev/null |
    sed -n 's/^[^"]*"\(.*\)"$/\1/p')
  [ "${#payload}" -eq "$1" ]
}

# rawm-v1 accepts its complete 4096-byte payload and rejects a larger sample
# without replacing the last successful value.
dd if=/dev/zero bs=4096 count=1 2>/dev/null | tr '\000' x >"$tmp/value"
kill -"$update_signal" "$pid"
wait_property "$(cat "$tmp/value")" || {
  cat "$tmp/stderr" >&2
  fail "4096-byte rawm-v1 payload was not published"
}
property_length 4096 || fail "published rawm-v1 boundary payload has wrong length"
printf x >>"$tmp/value"
kill -"$update_signal" "$pid"
wait_for_invalid=0
while ! grep -q "invalid rawm-v1 text (too long)" "$tmp/stderr"; do
  wait_for_invalid=$((wait_for_invalid + 1))
  [ "$wait_for_invalid" -lt 100 ] || fail "oversized block output was not rejected"
  sleep 0.02
done
property_length 4096 || fail "oversized sample replaced last valid rawm-v1 payload"

kill -TERM "$pid"
wait "$pid" || fail "rawmstat did not terminate cleanly"
pid=

echo "rawmstat protocol tests: ok"
