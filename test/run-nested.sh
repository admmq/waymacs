#!/bin/sh
# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
# Run waymacs in a window of your current Wayland (or X11) session.
set -eu
# -Q plus our own load-path; a site load path for another Emacs build
# (e.g. Guix with Debian's emacs-pgtk) would hide its standard lisp.
unset EMACSLOADPATH EMACSNATIVELOADPATH
top=$(cd "$(dirname "$0")/.." && pwd)
exec "$top/waymacs" "$@" -- "${EMACS_PGTK:-emacs-pgtk}" -Q -l "$top/test/init.el" \
	-l "$top/lisp/waymacs.el" -f waymacs-enable
