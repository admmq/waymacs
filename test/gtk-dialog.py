# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
"""A GTK3 window that opens windows which float by default.

Used by smoke.sh: SIGUSR1 opens a dialog of the window, SIGUSR2 a window
that cannot be resized.  They wait for a signal because GTK gives a
dialog made before its parent window is on screen no parent.
"""
import signal

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

window = Gtk.Window(title="dialog-test-main")
window.set_default_size(400, 300)
window.connect("destroy", Gtk.main_quit)


def open_dialog():
    Gtk.Window(title="dialog-test", transient_for=window).show_all()
    return GLib.SOURCE_REMOVE


def open_fixed_size_window():
    fixed = Gtk.Window(title="fixed-size-test")
    fixed.set_resizable(False)
    fixed.show_all()
    return GLib.SOURCE_REMOVE


GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, open_dialog)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR2, open_fixed_size_window)
window.show_all()
print("ready", flush=True)
Gtk.main()
