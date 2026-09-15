#!/bin/sh
set -eu

rawmstat=${1:-./rawmstat}
tmp=${TMPDIR:-/tmp}/rawmstat-config-test.$$
trap 'kill ${pid:-} 2>/dev/null || true; rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

fail()
{
  echo "rawmstat config test failed: $*" >&2
  exit 1
}

expect_ok()
{
  "$rawmstat" -C -c "$1" >/dev/null 2>"$tmp/stderr" || {
    cat "$tmp/stderr" >&2
    fail "expected $1 to validate"
  }
}

expect_fail()
{
  if "$rawmstat" -C -c "$1" >/dev/null 2>"$tmp/stderr"; then
    fail "expected $1 to fail validation"
  fi
}

wait_for_output()
{
  file=$1
  n=0
  while [ ! -s "$file" ]; do
    n=$((n + 1))
    [ "$n" -lt 100 ] || return 1
    sleep 0.02
  done
}

expect_ok rawmstat.conf

cat >"$tmp/valid.conf" <<'EOC'
status = {
  protocol = "rawm-v1";
  delimiter = " :: ";
};
blocks = (
  {
    name = "left";
    prefix = "[";
    command = ("printf", "one]");
    interval = 0;
    signal = 0;
  },
  {
    name = "right";
    command = ("printf", "two");
    interval = 60;
    signal = 1;
  }
);
EOC
expect_ok "$tmp/valid.conf"

cat >"$tmp/unknown.conf" <<'EOC'
status = { protcol = "rawm-v1"; };
EOC
expect_fail "$tmp/unknown.conf"

cat >"$tmp/protocol.conf" <<'EOC'
status = { protocol = "wm-name"; };
EOC
expect_fail "$tmp/protocol.conf"
grep -q "unsupported status protocol" "$tmp/stderr" ||
  fail "unsupported protocol diagnostic missing"

cat >"$tmp/duplicate.conf" <<'EOC'
blocks = (
  { name = "same"; command = ("true"); },
  { name = "same"; command = ("true"); }
);
EOC
expect_fail "$tmp/duplicate.conf"

cat >"$tmp/empty-command.conf" <<'EOC'
blocks = ({ name = "bad"; command = (); });
EOC
expect_fail "$tmp/empty-command.conf"

cat >"$tmp/command-type.conf" <<'EOC'
blocks = ({ name = "bad"; command = ("printf", 1); });
EOC
expect_fail "$tmp/command-type.conf"

cat >"$tmp/negative-interval.conf" <<'EOC'
blocks = ({ name = "bad"; command = ("true"); interval = -1; });
EOC
expect_fail "$tmp/negative-interval.conf"

cat >"$tmp/negative-signal.conf" <<'EOC'
blocks = ({ name = "bad"; command = ("true"); signal = -1; });
EOC
expect_fail "$tmp/negative-signal.conf"

cat >"$tmp/include-body.conf" <<'EOC'
blocks = ({ name = "included"; command = ("printf", "included"); interval = 0; });
EOC
cat >"$tmp/include.conf" <<'EOC'
@include "include-body.conf"
EOC
expect_ok "$tmp/include.conf"

mkdir -p "$tmp/xdg/rawm"
cat >"$tmp/xdg/rawm/rawmstat.conf" <<'EOC'
blocks = ({ name = "user"; command = ("printf", "user-config"); interval = 0; });
EOC
XDG_CONFIG_HOME="$tmp/xdg" DISPLAY= "$rawmstat" -p >"$tmp/stdout" 2>"$tmp/user.err" &
pid=$!
wait_for_output "$tmp/stdout" || {
  cat "$tmp/user.err" >&2
  fail "user configuration did not produce output"
}
kill -TERM "$pid"
wait "$pid" || fail "rawmstat did not terminate cleanly"
pid=
[ "$(sed -n '1p' "$tmp/stdout")" = "user-config" ] ||
  fail "user block list did not replace built-in blocks"

cat >"$tmp/xdg/rawm/rawmstat.conf" <<'EOC'
this_is_invalid = true;
EOC
if XDG_CONFIG_HOME="$tmp/xdg" "$rawmstat" -C >/dev/null 2>"$tmp/stderr"; then
  fail "normal configuration lookup ignored invalid user configuration"
fi
XDG_CONFIG_HOME="$tmp/xdg" "$rawmstat" -C -c rawmstat.conf >/dev/null 2>"$tmp/stderr" || {
  cat "$tmp/stderr" >&2
  fail "explicit configuration was not isolated from user configuration"
}

echo "rawmstat config tests: ok"
