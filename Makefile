CC ?= cc
PKGS = x11 xft fontconfig zlib
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local

shot: shot.c
	$(CC) $(CFLAGS) -o $@ shot.c $(LDLIBS)

install: shot
	install -m 755 shot $(DESTDIR)$(PREFIX)/bin/shot

clean:
	rm -f shot

.PHONY: install clean
