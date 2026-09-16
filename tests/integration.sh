#!/bin/sh
set -eu

rawmstat=${1:-./rawmstat}
signal_number=${2:-./tests/signal-number}
bar_checksum=${3:-./tests/bar-checksum}
rawm=${4:-}
tmp=${TMPDIR:-/tmp}/rawmstat-integration-test.$$
mkdir -p "$tmp"
wm=
producer=
xvfb=
cleanup()
{
  [ -z "$producer" ] || kill -KILL "$producer" 2>/dev/null || true
  [ -z "$wm" ] || kill -KILL "$wm" 2>/dev/null || true
  [ -z "$xvfb" ] || kill "$xvfb" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

skip()
{
  echo "rawm/rawmstat integration tests: skipped ($*)"
  exit 0
}

fail()
{
  echo "rawm/rawmstat integration test failed: $*" >&2
  [ ! -f "$tmp/rawm.log" ] || cat "$tmp/rawm.log" >&2
  [ ! -f "$tmp/rawmstat.log" ] || cat "$tmp/rawmstat.log" >&2
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

[ -n "$rawm" ] || skip "RAWM is not set"
[ -x "$rawm" ] || skip "$rawm is not executable"
[ -x "$signal_number" ] || skip "$signal_number not built"
[ -x "$bar_checksum" ] || skip "$bar_checksum not built"
command -v Xvfb >/dev/null 2>&1 || skip "Xvfb not found"
command -v xprop >/dev/null 2>&1 || skip "xprop not found"
if ! update_signal=$($signal_number 1); then
  skip "realtime signals unavailable"
fi

cat >"$tmp/rawm.conf" <<'EOC'
ui = {
  font = "monospace:size=9";
  bar = { show = true; position = "bottom"; height = 24; };
  systray = { show = false; };
};
status = { enabled = true; protocol = "rawm-v1"; };
input = { keys = (); buttons = (); };
EOC

printf '%s' one >"$tmp/value"
cat >"$tmp/rawmstat.conf" <<EOF2
status = { protocol = "rawm-v1"; delimiter = " | "; };
blocks = (
  {
    name = "hung";
    command = ("/bin/sh", "-c", "sleep 30");
    interval = 0;
    timeout_ms = 150;
  },
  {
    name = "probe";
    command = ("cat", "$tmp/value");
    interval = 0;
    signal = 1;
    timeout_ms = 1000;
  }
);
EOF2

display_file=$tmp/display
: >"$display_file"
Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp \
  >"$tmp/xvfb.log" 2>&1 3>"$display_file" &
xvfb=$!
wait_for 100 test -s "$display_file" || fail "Xvfb did not select a display"
DISPLAY=":$(cat "$display_file")"
export DISPLAY

"$rawm" -c "$tmp/rawm.conf" >"$tmp/rawm.log" 2>&1 &
wm=$!
wait_for 100 kill -0 "$wm" || fail "rawm exited before integration test"
wait_for 100 "$bar_checksum" >/dev/null 2>&1 || fail "rawm bar was not created"
baseline=$($bar_checksum)

"$rawmstat" -c "$tmp/rawmstat.conf" >"$tmp/rawmstat.out" 2>"$tmp/rawmstat.log" &
producer=$!

property_is()
{
  expected=$1
  xprop -notype -root _RAWM_STATUS_V1 2>/dev/null | grep -Fq "\"$expected\""
}
checksum_changed()
{
  old=$1
  current=$($bar_checksum 2>/dev/null || printf 0)
  [ "$current" != "$old" ]
}

wait_for 100 property_is one || fail "rawmstat did not publish initial status while another block was hung"
wait_for 100 checksum_changed "$baseline" || fail "rawm did not render rawmstat's initial status"
one_hash=$($bar_checksum)

printf '%s' two >"$tmp/value"
kill -"$update_signal" "$producer"
wait_for 100 property_is two || fail "signal update did not reach _RAWM_STATUS_V1"
wait_for 100 checksum_changed "$one_hash" || fail "rawm bar did not redraw after rawmstat update"
wait_for 100 grep -q "block 'hung' timed out" "$tmp/rawmstat.log" || fail "hung integration block did not time out"

kill -TERM "$producer"
wait "$producer" || fail "rawmstat did not terminate cleanly"
producer=
kill -TERM "$wm"
wait "$wm" || fail "rawm did not terminate cleanly"
wm=

echo "rawm/rawmstat integration tests: ok"
