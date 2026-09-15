.POSIX:

include config.mk

all: rawmstat rawmstat.1 rawmstat.conf.5

rawmstat: rawmstat.o rawmstat-config.o
	${CC} -o $@ rawmstat.o rawmstat-config.o ${LDFLAGS}

rawmstat.o: rawmstat.c rawmstat-config.h
rawmstat-config.o: rawmstat-config.c rawmstat-config.h

rawmstat.1: rawmstat.1.scdoc
	scdoc < rawmstat.1.scdoc > rawmstat.1

rawmstat.conf.5: rawmstat.conf.5.scdoc
	scdoc < rawmstat.conf.5.scdoc > rawmstat.conf.5

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	mkdir -p ${DESTDIR}${MANPREFIX}/man5
	mkdir -p ${DESTDIR}${SYSCONFDIR}/rawm
	cp -f rawmstat ${DESTDIR}${PREFIX}/bin/
	cp -f rawmstat.1 ${DESTDIR}${MANPREFIX}/man1/
	cp -f rawmstat.conf.5 ${DESTDIR}${MANPREFIX}/man5/
	if [ ! -e ${DESTDIR}${SYSCONFDIR}/rawm/rawmstat.conf ]; then \
		cp rawmstat.conf ${DESTDIR}${SYSCONFDIR}/rawm/rawmstat.conf; \
	fi
	chmod 0755 ${DESTDIR}${PREFIX}/bin/rawmstat
	chmod 0644 ${DESTDIR}${MANPREFIX}/man1/rawmstat.1
	chmod 0644 ${DESTDIR}${MANPREFIX}/man5/rawmstat.conf.5
	chmod 0644 ${DESTDIR}${SYSCONFDIR}/rawm/rawmstat.conf

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/rawmstat
	rm -f ${DESTDIR}${MANPREFIX}/man1/rawmstat.1
	rm -f ${DESTDIR}${MANPREFIX}/man5/rawmstat.conf.5

clean:
	rm -f rawmstat rawmstat.o rawmstat-config.o rawmstat.1 rawmstat.conf.5 \
	      tests/signal-number tests/signal-number.o

tests/signal-number: tests/signal-number.o
	${CC} -o $@ tests/signal-number.o

check: rawmstat tests/signal-number
	./tests/config.sh ./rawmstat
	./tests/protocol.sh ./rawmstat ./tests/signal-number

release:
	git tag -a v${VERSION} -m v${VERSION}

.PHONY: all check install uninstall clean release
