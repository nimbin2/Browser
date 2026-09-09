CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

VERSION  := 2.3.4
SOURCES  := browser_core.c browser_core.h browser-mini.c browser-big.c Makefile
BUILD_ID := $(shell cat $(SOURCES) 2>/dev/null | md5sum | cut -c1-7)

CORE_PKGS = gtk4 webkitgtk-6.0 libsoup-3.0
BIG_PKGS  = $(CORE_PKGS) gstreamer-1.0

CORE_CFLAGS := $(shell pkg-config --cflags $(CORE_PKGS))
CORE_LIBS   := $(shell pkg-config --libs   $(CORE_PKGS))
BIG_CFLAGS  := $(shell pkg-config --cflags $(BIG_PKGS))
BIG_LIBS    := $(shell pkg-config --libs   $(BIG_PKGS))

VERFLAGS := -DBROWSER_VERSION='"$(VERSION)"' -DBROWSER_BUILD='"$(BUILD_ID)"'

BINS = browser-mini browser-big

all: $(BINS)

browser-mini: browser-mini.c browser_core.c browser_core.h
	$(CC) $(CFLAGS) $(VERFLAGS) -o $@ browser-mini.c browser_core.c $(CORE_CFLAGS) $(CORE_LIBS)

browser-big: browser-big.c browser_core.c browser_core.h
	$(CC) $(CFLAGS) $(VERFLAGS) -o $@ browser-big.c browser_core.c $(BIG_CFLAGS) $(BIG_LIBS)

version:
	@echo "$(VERSION) (build $(BUILD_ID))"

install: all
	install -Dm755 browser-mini $(DESTDIR)$(PREFIX)/bin/browser-mini
	install -Dm755 browser-big  $(DESTDIR)$(PREFIX)/bin/browser-big

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/browser-mini $(DESTDIR)$(PREFIX)/bin/browser-big

clean:
	rm -f $(BINS) minibrowser bigbrowser

.PHONY: all install uninstall clean version
