.POSIX:

include config.mk

all: config.h rawmstat rawmstat.1

config.h:
	cp config.def.h $@

rawmstat: rawmstat.o

rawmstat.1: rawmstat.1.scdoc
	scdoc < rawmstat.1.scdoc > rawmstat.1

install: all
	mkdir -p        ${DESTDIR}${PREFIX}/bin/
	mkdir -p        ${DESTDIR}${MANPREFIX}/man1
	cp -f rawmstat   ${DESTDIR}${PREFIX}/bin/
	cp -f rawmstat.1 ${DESTDIR}${MANPREFIX}/man1/
	chmod 0755      ${DESTDIR}${PREFIX}/bin/rawmstat
	chmod 0644      ${DESTDIR}${MANPREFIX}/man1/rawmstat.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/rawmstat
	rm -f ${DESTDIR}${MANPREFIX}/man1/rawmstat.1

clean:
	rm -f rawmstat rawmstat.o rawmstat.1

release:
	git tag -a v${VERSION} -m v${VERSION}

.PHONY: all install uninstall clean release
