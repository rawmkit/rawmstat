OVERVIEW
========

`rawmstat` is a small runtime-configurable status producer for `rawm(1)`.
It executes configured commands, composes their first output lines, and
publishes plain UTF-8 through rawm's `_RAWM_STATUS_V1` root-window property.
It can instead print changed status lines to stdout for testing or use with
text-oriented consumers.

`rawmstat` deliberately does not supervise rawm, discover consumers, create
sockets, or encode presentation controls in status text.  The connection is
the same simple X11 root-property model traditionally used by dwm status
producers, with an explicit rawm-owned property and payload contract.

This distribution is a fork of torrinfail's `dwmblocks` as of commit
a933ce0 (Thu Jan 6 2022).  The current implementation has diverged
substantially: configuration is loaded at runtime with libconfig, block
commands are direct argv vectors, scheduling uses a monotonic event loop,
realtime signals are handled through a self-pipe, and status transport uses
the `rawm-v1` protocol rather than the root `WM_NAME` convention.

See git log for the complete history.

The original sources can be downloaded from:
  1. https://github.com/torrinfail/dwmblocks
  2. https://github.com/torrinfail/dwmblocks/archive/a933ce0/dwmblocks-a933ce0.zip


REQUIREMENTS
============

Build time
----------
  * C99 compiler
  * POSIX `sh(1p)`, `make(1p)` and mandatory utilities
  * libconfig
  * libX11 (unless built with `NO_X`)
  * `scdoc(1)` to build manual pages


INSTALL
=======

The shell commands `make && make install` build and install `rawmstat`,
`rawmstat(1)`, `rawmstat.conf(5)`, and the default configuration at
`/etc/rawm/rawmstat.conf` unless that configuration file already exists.

See `config.mk` for installation paths and build parameters.


CONFIGURATION
=============

The built-in defaults are intentionally usable and minimal.  In normal mode
rawmstat layers optional system and user configuration over those defaults:

  1. `/etc/rawm/rawmstat.conf`
  2. `$XDG_CONFIG_HOME/rawm/rawmstat.conf`, or
     `$HOME/.config/rawm/rawmstat.conf`

`rawmstat -c FILE` instead loads built-in defaults followed by exactly FILE.
`rawmstat -C` validates configuration without opening X or executing blocks.

Example:

```
status = {
  protocol = "rawm-v1";
  delimiter = " | ";
};

blocks = (
  {
    name = "clock";
    command = ("date", "+%a %b %d %H:%M");
    interval = 5;
    signal = 0;
  }
);
```

Commands are argv vectors and are executed directly; there is no implicit
shell.  See `rawmstat.conf(5)` for the complete schema, composition rules,
and signal semantics.


SESSION
=======

A typical X session starts the producer independently of the window manager:

```
rawmstat &
exec rawm
```

rawmstat writes `_RAWM_STATUS_V1`; rawm watches that property and renders its
plain UTF-8 value according to rawm's own bar policy.


LICENSE
=======

`rawmstat` is licensed through the ISC License.
See LICENSE for copyright and license details.
