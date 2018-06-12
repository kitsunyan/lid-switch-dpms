CC=gcc
PKGCONFIG=pkg-config
DESTDIR=

PKGLIBDIR=/usr/lib/lid-switch-dpms
UNITDIR=$(shell $(PKGCONFIG) systemd --variable=systemdsystemunitdir)

.PHONY: all install

all: \
	daemon \
	systemd.service

daemon: daemon.c
	$(CC) $(CFLAGS) \
	$(shell $(PKGCONFIG) --libs --cflags libdrm) \
	-lX11 -lXext $(LDFLAGS) -o $@ $<

systemd.service: systemd.service.in
	cat $< | sed \
	-e "s,@PKGLIBDIR@,$(PKGLIBDIR),g" \
	> $@

install: all
	install -Dm755 daemon $(DESTDIR)$(PKGLIBDIR)/daemon
	install -Dm644 systemd.service $(DESTDIR)$(UNITDIR)/lid-switch-dpms.service
