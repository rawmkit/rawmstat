# project metadata
NAME      = rawmstat
VERSION   = 0.1

# paths
PREFIX    = /usr/local
MANPREFIX = ${PREFIX}/share/man
SYSCONFDIR = /etc

# X support (uncomment to disable X11)
#NO_X     = -DNO_X

# DragonFlyBSD, FreeBSD
#X11INC   = /usr/local/include
#X11LIB   = /usr/local/lib

# NetBSD, OpenBSD
#X11INC   = /usr/X11R6/include
#X11LIB   = /usr/X11R6/lib

# Linux
X11INC    = /usr/include
X11LIB    = /usr/lib

# includes and libs
INCS      = -I${X11INC}
LIBS      = -L${X11LIB} -lX11 -lconfig

# flags
CPPFLAGS  = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
            -DVERSION=\"${VERSION}\" -DSYSCONFDIR=\"${SYSCONFDIR}\" \
            ${NO_X}
CFLAGS    = -std=c99 -pedantic -Wall -Wextra -Wformat ${CPPFLAGS} ${INCS}
LDFLAGS   = ${LIBS}
