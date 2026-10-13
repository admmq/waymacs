#!/bin/sh
# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
# Run waymacs without any display (wlroots headless backend, software
# rendering).  Useful for tests: point grim, wtype, wlrctl or foot at the
# WAYLAND_DISPLAY printed in the log, and talk to Emacs with
#   emacsclient -s waymacs-test -e '(...)'
set -eu
# -Q plus our own load-path; a site load path for another Emacs build
# (e.g. Guix with Debian's emacs-pgtk) would hide its standard lisp.
unset EMACSLOADPATH EMACSNATIVELOADPATH
top=$(cd "$(dirname "$0")/.." && pwd)
export WLR_BACKENDS=headless WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS="${WLR_HEADLESS_OUTPUTS:-1}"
exec "$top/waymacs" "$@" -- "${EMACS_PGTK:-emacs-pgtk}" -Q -l "$top/test/init.el" \
	-l "$top/lisp/waymacs.el" -f waymacs-enable
