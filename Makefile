# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
EMACS ?= emacs
# where the session file tells Emacs to load waymacs.el from
LISPDIR ?= $(CURDIR)/lisp
# wlroots 0.19 or 0.18, whichever is installed (0.19 if both are)
WLROOTS ?= $(shell $(PKG_CONFIG) --exists wlroots-0.19 && echo wlroots-0.19 || echo wlroots-0.18)
PKGS = $(WLROOTS) wayland-server xkbcommon libinput pixman-1 xcb

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra \
	-Wno-unused-parameter -Wno-missing-field-initializers \
	-DWLR_USE_UNSTABLE -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC = src/main.cpp src/ipc.cpp src/view.cpp src/input.cpp src/output.cpp src/lock.cpp \
	src/bar.cpp src/xwayland.cpp
# protocols waymacs implements itself (wlroots does not): their code too
OWN_PROTOCOLS = river-status-unstable-v1 river-control-unstable-v1
OBJ = $(SRC:src/%.cpp=build/%.o) $(OWN_PROTOCOLS:%=build/%-protocol.o)
# protocol headers that the wlroots headers include, and our own
PROTO = build/xdg-shell-protocol.h \
	build/wlr-layer-shell-unstable-v1-protocol.h \
	build/cursor-shape-v1-protocol.h \
	build/tablet-v2-protocol.h \
	$(OWN_PROTOCOLS:%=build/%-protocol.h)

all: waymacs

waymacs: $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.cpp src/waymacs.h $(PROTO)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/xdg-shell-protocol.h:
	@mkdir -p build
	$(WAYLAND_SCANNER) server-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
build/cursor-shape-v1-protocol.h:
	@mkdir -p build
	$(WAYLAND_SCANNER) server-header $(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
build/tablet-v2-protocol.h:
	@mkdir -p build
	$(WAYLAND_SCANNER) server-header $(WAYLAND_PROTOCOLS)/unstable/tablet/tablet-unstable-v2.xml $@
# protocol XML kept in protocols/
build/%-protocol.h: protocols/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) server-header $< $@
build/%-protocol.c: protocols/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) private-code $< $@
build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags wayland-server) -c -o $@ $<

# end-to-end test, headless (needs emacs-pgtk, foot, wtype, wlrctl, grim)
test: waymacs
	./test/smoke.sh

# byte-compile the elisp, warnings are errors
check:
	$(EMACS) -Q --batch --eval '(setq byte-compile-error-on-warn t)' \
		-f batch-byte-compile lisp/waymacs.el

install: waymacs
	install -Dm755 waymacs $(DESTDIR)$(PREFIX)/bin/waymacs
	@mkdir -p build
	sed -e 's|@BINDIR@|$(PREFIX)/bin|' contrib/waymacs-session.in > build/waymacs-session
	install -Dm755 build/waymacs-session $(DESTDIR)$(PREFIX)/bin/waymacs-session
	sed -e 's|@BINDIR@|$(PREFIX)/bin|' -e 's|@EMACS@|$(EMACS)|' \
	    -e 's|@LISPDIR@|$(LISPDIR)|' contrib/waymacs.desktop.in > build/waymacs.desktop
	install -Dm644 build/waymacs.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/waymacs.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/waymacs $(DESTDIR)$(PREFIX)/bin/waymacs-session \
		$(DESTDIR)$(PREFIX)/share/wayland-sessions/waymacs.desktop

clean:
	rm -rf build waymacs lisp/*.elc

.PHONY: all test check install uninstall clean
