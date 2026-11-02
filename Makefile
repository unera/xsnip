CC ?= cc
PKGS = x11 xft fontconfig zlib
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local

xsnip: xsnip.c
	$(CC) $(CFLAGS) -o $@ xsnip.c $(LDLIBS)

install: xsnip
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xsnip $(DESTDIR)$(PREFIX)/bin/xsnip
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 man/xsnip.1 $(DESTDIR)$(PREFIX)/share/man/man1/xsnip.1

clean:
	rm -f xsnip

.PHONY: install clean
