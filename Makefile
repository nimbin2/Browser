CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

VERSION  := 1.1.2
SOURCES  := browser_core.c browser_core.h minibrowser.c bigbrowser.c Makefile
BUILD_ID := $(shell cat $(SOURCES) 2>/dev/null | md5sum | cut -c1-7)

CORE_PKGS = gtk4 webkitgtk-6.0 libsoup-3.0
BIG_PKGS  = $(CORE_PKGS) gstreamer-1.0

CORE_CFLAGS := $(shell pkg-config --cflags $(CORE_PKGS))
CORE_LIBS   := $(shell pkg-config --libs   $(CORE_PKGS))
BIG_CFLAGS  := $(shell pkg-config --cflags $(BIG_PKGS))
BIG_LIBS    := $(shell pkg-config --libs   $(BIG_PKGS))

VERFLAGS := -DBROWSER_VERSION='"$(VERSION)"' -DBROWSER_BUILD='"$(BUILD_ID)"'

BINS = minibrowser bigbrowser

all: $(BINS)

minibrowser: minibrowser.c browser_core.c browser_core.h
	$(CC) $(CFLAGS) $(VERFLAGS) -o $@ minibrowser.c browser_core.c $(CORE_CFLAGS) $(CORE_LIBS)

bigbrowser: bigbrowser.c browser_core.c browser_core.h
	$(CC) $(CFLAGS) $(VERFLAGS) -o $@ bigbrowser.c browser_core.c $(BIG_CFLAGS) $(BIG_LIBS)

version:
	@echo "$(VERSION) (build $(BUILD_ID))"

install: all
	install -Dm755 minibrowser $(DESTDIR)$(PREFIX)/bin/minibrowser
	install -Dm755 bigbrowser  $(DESTDIR)$(PREFIX)/bin/bigbrowser

clean:
	rm -f $(BINS)

.PHONY: all install clean version
