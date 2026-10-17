# Message from HUMAN

Hi, Adam here. This project is almost entirely LLM-generated.
Works for me. Anyone, who wants to rewrite is welcome.

# waymacs

Emacs as a Wayland window manager, in the spirit of
[EXWM](https://github.com/emacs-exwm/exwm).

Every application window gets an Emacs buffer.  Showing that buffer in an
Emacs window shows the application there, so `split-window`,
`display-buffer-alist`, `winner-mode`, tab-bar workspaces and the rest of
your Emacs configuration are the window manager.  While an application's
window is selected it gets the keyboard, except for prefix keys such as
`C-x` and `M-x`, which still go to Emacs.

waymacs has two parts:

- `waymacs`, a small Wayland compositor written in C-style C++ on top of
  [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).  It shows
  Emacs frames, puts application windows where Emacs says, moves the
  keyboard focus and hands prefix keys to Emacs.  It makes no window
  management decisions of its own.
- `waymacs.el`, which makes all the decisions.

## Dependencies

waymacs was developed and tested on Debian 13 (trixie) with these
packages:

```sh
sudo apt install libwlroots-0.18-dev libwayland-dev wayland-protocols \
    libxkbcommon-dev libpixman-1-dev libinput-dev pkgconf emacs-pgtk \
    xwayland foot grim wtype wlrctl wayland-utils
```

Emacs must be a pgtk build: the session Emacs's frames must be Wayland
windows.

## Building

```sh
make            # builds ./waymacs
make check      # byte-compiles lisp/waymacs.el, warnings are errors
make test       # end-to-end test without any display (see below)
sudo make install    # /usr/local: waymacs, waymacs-session, the session file
```

`make install` puts `waymacs.desktop` in
`$(PREFIX)/share/wayland-sessions`, so display managers offer a "waymacs"
session.  It does not install `waymacs.el`: the session file starts Emacs
with this checkout's copy,

```
Exec=/usr/local/bin/waymacs-session -- emacs -l /path/to/waymacs/lisp/waymacs.el -f waymacs-enable
```

## Running

```sh
waymacs -- emacs -l /path/to/waymacs/lisp/waymacs.el -f waymacs-enable
waymacs -d -- emacs ...       # with debug logging
waymacs -s 'emacs ...'        # the command as one shell string
```

The words after `--` are the program to run and its arguments (no shell
involved); without them waymacs runs `-s`'s shell command, else
`$WAYMACS_EMACS`, else plain `emacs`.  The session ends when that program
exits.

Loading waymacs on the command line, not in your init file, means only
the session's Emacs loads it: an Emacs you start inside the session reads
the same init file but stays an ordinary application.

Additional settings can be put is ~/.emacs using with-eval-after-load:

```elisp
...

(with-eval-after-load 'waymacs
  ;; run waybar
  (start-process "waybar" nil "waybar")
  ;; set keyboard
  (setq waymacs-xkb-layout "us,ru"
        waymacs-xkb-options "grp:win_space_toggle"))

...
```

## Emacs configuration

The session's Emacs loads waymacs from its command line (see Running), so
your init file needs nothing.  To configure waymacs, wrap the settings in
`with-eval-after-load`; they then run only in the session's Emacs:

```elisp
(with-eval-after-load 'waymacs
  (menu-bar-mode -1)
  (tool-bar-mode -1)
  (setq waymacs-input-simulation-keys
        '(([?\C-b] . [left]) ([?\C-f] . [right])
          ([?\C-p] . [up]) ([?\C-n] . [down])
          ([?\C-a] . [home]) ([?\C-e] . [end])
          ([?\M-v] . [prior]) ([?\C-v] . [next])
          ([?\C-d] . [delete]) ([?\C-k] . [S-end delete])
          ([?\M-w] . [?\C-c]) ([?\C-y] . [?\C-v])))
  (add-to-list 'waymacs-input-char-mode-apps "\\`foot\\'")   ; terminals
  (setq waymacs-xkb-layout "us,ru"
        waymacs-xkb-options "ctrl:nocaps,grp:alt_shift_toggle"))
```

`waymacs-enable` runs after the init file, so these settings are in place
when it starts.  (Calling `(require 'waymacs)` and `(waymacs-enable)` from
the init file still works, but then every Emacs loads waymacs; one started
inside the session does not take over, it only carries the code.)


material).
