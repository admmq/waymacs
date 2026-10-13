# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
"""A GTK3 window that opens a popup menu when `m' is pressed.

Used by smoke.sh to check that menus (xdg popups with a grab) get the
keyboard.  Prints "activated <item>" when an item is chosen.
"""
import gi

gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, Gtk  # noqa: E402

window = Gtk.Window(title="gtk-menu-test")
window.set_default_size(400, 300)
window.connect("destroy", Gtk.main_quit)


def open_menu(trigger_event):
    menu = Gtk.Menu()
    for label in ("First item", "Second item"):
        item = Gtk.MenuItem(label=label)
        item.connect("activate", lambda _, text=label: print("activated", text, flush=True))
        menu.append(item)
    menu.show_all()
    anchor = Gdk.Rectangle()
    anchor.x, anchor.y, anchor.width, anchor.height = 50, 50, 1, 1
    menu.popup_at_rect(window.get_window(), anchor, Gdk.Gravity.NORTH_WEST,
                       Gdk.Gravity.NORTH_WEST, trigger_event)
    print("menu shown", flush=True)


def on_key_press(_, event):
    if event.keyval == Gdk.KEY_m:
        open_menu(event)
        return True
    return False


window.connect("key-press-event", on_key_press)
window.show_all()
print("ready", flush=True)
Gtk.main()
