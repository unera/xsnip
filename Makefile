CC ?= cc
PKGS = x11 xft fontconfig zlib xinerama

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
	install -d $(DESTDIR)$(PREFIX)/share/man/ru/man1
	install -m 644 man/ru/xsnip.1 $(DESTDIR)$(PREFIX)/share/man/ru/man1/xsnip.1
	install -d $(DESTDIR)$(PREFIX)/share/man/zh_CN/man1
	install -m 644 man/zh_CN/xsnip.1 $(DESTDIR)$(PREFIX)/share/man/zh_CN/man1/xsnip.1

test-gui: xsnip
	bash test/c_e2e.sh

clean:
	rm -f xsnip

.PHONY: install test-gui clean
