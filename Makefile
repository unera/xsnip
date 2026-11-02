CC ?= cc
PKGS = x11 xft fontconfig zlib

# флаги pkg-config идут отдельно от CFLAGS: дистрибутивы передают свои CFLAGS
# в окружении, а переменные командной строки перебили бы += в этом файле
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=c99
LDLIBS += $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local

xsnip: xsnip.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ xsnip.c $(LDLIBS)

install: xsnip
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xsnip $(DESTDIR)$(PREFIX)/bin/xsnip
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 man/xsnip.1 $(DESTDIR)$(PREFIX)/share/man/man1/xsnip.1

check: xsnip
	bash test/c_e2e.sh

clean:
	rm -f xsnip

.PHONY: install check clean
