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

```
 ┌──────────────── waymacs (compositor) ────────────────┐
 │  wlroots: outputs, input, xdg-shell, layer-shell...  │
 │                                                       │
 │   ┌─────────── Emacs frame (pgtk) ────────────┐      │
 │   │ ┌─────────────┐ ┌───────────────────────┐ │      │
 │   │ │ *scratch*   │ │ firefox  (an Emacs     │ │      │
 │   │ │             │ │ window; the compositor │ │      │
 │   │ │             │ │ draws Firefox here)    │ │      │
 │   │ └─────────────┘ └───────────────────────┘ │      │
 │   └────────────────────────────────────────────┘      │
 └──────────────▲──────────────────────────┬──────────────┘
   layout, focus,│                          │ new window, title,
   keys to grab  │   $WAYMACS_SOCKET        │ closed, clicked...
                 │   (text lines /          ▼
              ┌──┴─── S-expressions) ─── waymacs.el (in Emacs)
```

The compositor starts Emacs and the session ends when Emacs exits.

## Status

waymacs is new.  Everything below was tested without a display (the
wlroots headless backend): by `make test` (application windows,
splitting, line and char mode, prefix, global and simulation keys, click
to focus, floating and fullscreen windows, menus, switching and moving
windows between workspaces, screen locking with swaylock, waybar, X11
applications through Xwayland, workspaces assigned to two monitors) and
by hand (the clipboard, two monitors, workspaces across monitors,
fullscreen windows giving way to the minibuffer).  Running from a TTY or a display manager on real
hardware, nested mode, monitor hotplug and layer-shell clients (bars,
launchers, wallpapers) are implemented but have not been tried yet.
Expect rough edges.

## Dependencies

waymacs was developed and tested on Debian 13 (trixie) with these
packages:

| Package | Version | Used for |
|---|---|---|
| `libwlroots-0.18-dev` | 0.18.2 | the compositor library (wlroots 0.18 or 0.19; not 0.20 yet) |
| `libwayland-dev` (with `libwayland-bin`) | 1.23.1 | Wayland server library, `wayland-scanner` |
| `wayland-protocols` | 1.44 | protocol XML files (xdg-shell, cursor-shape, tablet) |
| `libxkbcommon-dev` | 1.7.0 | keymaps and key handling |
| `libpixman-1-dev` | 0.44.0 | needed by the wlroots headers |
| `libinput-dev` | 1.28.1 | touchpad configuration (tap to click) |
| `libxcb1-dev`, `libxcb-icccm4-dev`, `libxcb-ewmh-dev` | 1.17.0, 0.4.2 | X11 window properties for Xwayland (installed with `libwlroots-0.18-dev`) |
| `xwayland` | 24.1.6 | runs X11 applications; started when the first one connects |
| `pkgconf` | 1.8.1 | finding the libraries above |
| `g++`, `make` | GCC 14.2, make 4.4.1 | building |
| `emacs-pgtk` | 30.1 | Emacs with the pure-GTK (Wayland) interface; any Emacs ≥ 29.1 built `--with-pgtk` works |
| `foot` | 1.21.0 | a terminal, used by the tests |
| `grim` | 1.4.0 | screenshots, used by the tests |
| `wtype` | 0.4 | typing keys, used by the tests |
| `wlrctl` | 0.2.2 | moving and clicking the pointer, used by the tests |
| `wayland-utils` | 1.2.0 | `wayland-info`, handy for checking the compositor |
| `python3-gi`, `gir1.2-gtk-3.0` | 3.50.0, 3.24.49 | optional: the GTK menu test |
| `xterm`, `zenity` | 398, 4.1.90 | optional: the X11 tests |

They were installed with:

```sh
sudo apt install libwlroots-0.18-dev libwayland-dev wayland-protocols \
    libxkbcommon-dev libpixman-1-dev libinput-dev pkgconf emacs-pgtk \
    xwayland foot grim wtype wlrctl wayland-utils
```

Emacs must be a pgtk build: the session Emacs's frames must be Wayland
windows.  (An X11 Emacs started inside the session is just another
application.)

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

`make install LISPDIR=... EMACS=emacs-pgtk` changes the directory and the
Emacs command written there.

`waymacs-session` runs `waymacs` through your login shell (`$SHELL -l`,
or `/bin/sh -l` for shells with another syntax such as fish), as GNOME
does.  Display managers start Wayland sessions without one, so otherwise
`/etc/profile`, `/etc/profile.d/*` and `~/.profile` would not be read and
Emacs, its shells (`M-x shell`, `ansi-term`) and the applications would
miss what they set up: for example Guix's `~/.config/guix/current/bin`
(the guix you pulled) and Guix Home.

waymacs builds with wlroots 0.18 or 0.19, whichever is installed (0.19
when both are; `make WLROOTS=wlroots-0.18` chooses).  The differences are
handled with `WLR_VERSION_MINOR` in the source.  On Guix, or to try 0.19
on another system with Guix installed:

```sh
guix shell wlroots@0.19 wayland wayland-protocols libxkbcommon libinput \
    pixman pkg-config gcc-toolchain make libxcb xcb-util-wm -- make
```

`make test` passes with both: 0.18.2 (Debian) and 0.19.3 (Guix, with
Guix's Xwayland).

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

Emacs reads its command line only after the init file and
`after-init-hook` have run without error.  If they fail (a package the
session's Emacs cannot find, say), waymacs is never loaded and Emacs
stays an ordinary window.  The compositor then logs "Emacs has not
connected after 15 s"; under a display manager its output goes to the
systemd journal (`journalctl --user -b | grep waymacs`).

From a text console waymacs drives the screens directly (it needs a seat
from logind or seatd, as every wlroots compositor does).  Inside an
existing Wayland or X11 session it opens a window instead, which is the
easiest way to try it:

```sh
./test/run-nested.sh          # waymacs + `emacs-pgtk -Q' with test/init.el
./test/run-headless.sh        # the same with no display at all
```

Keys handled by the compositor itself, always:

| Key | Action |
|---|---|
| `Ctrl+Alt+F1` … `F12` | switch virtual terminal (on a real console) |
| `Ctrl+Alt+Shift+BackSpace` | quit waymacs at once (emergency exit) |

Environment variables read by `waymacs`:

| Variable | Meaning |
|---|---|
| `WAYMACS_EMACS` | shell command to run when neither `--` nor `-s` is given |
| `WAYMACS_SCALE` | output scale, e.g. `2` for HiDPI screens |
| `XKB_DEFAULT_LAYOUT`, `XKB_DEFAULT_OPTIONS`, ... | keyboard layout (see also `waymacs-xkb-layout`) |
| `XCURSOR_THEME`, `XCURSOR_SIZE` | pointer theme and size |

waymacs sets `WAYLAND_DISPLAY`, `DISPLAY` (its Xwayland server),
`WAYMACS_SOCKET`, `XDG_SESSION_TYPE=wayland` and
`XDG_CURRENT_DESKTOP=waymacs` for Emacs and everything started from it.

If your environment sets `EMACSLOADPATH` for a different Emacs build (Guix
does, for its own Emacs), unset it when starting another Emacs, or that
Emacs will not find its own Lisp files.  The test scripts do this.

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

### All options

`M-x customize-group RET waymacs` shows them too.  The sections below
explain them in context.

| Option | Default | Meaning |
|---|---|---|
| **Keys** | | |
| `waymacs-input-prefix-keys` | `C-x C-u C-h M-x` `` M-` `` `M-& M-:` | keys that go to Emacs from an application in line mode |
| `waymacs-input-global-keys` | `s-r s-i s-d s-f`, `s-<arrows>`, `s-1`…`s-0`, `s-!`…`s-)` | bindings that work everywhere, char mode included (see Keyboard) |
| `waymacs-input-simulation-keys` | nil | keys typed in line-mode applications as other keys, e.g. `C-n` → `<down>` |
| `waymacs-input-char-mode-apps` | Emacs | regexps of app ids that start in char mode |
| **Keyboard** | | |
| `waymacs-xkb-layout` | nil (system default) | XKB layouts, e.g. `"us,ru"` |
| `waymacs-xkb-variant` | nil | XKB variants, e.g. `"dvorak"` |
| `waymacs-xkb-options` | nil | XKB options, e.g. `"ctrl:nocaps,grp:alt_shift_toggle"` |
| `waymacs-keyboard-repeat` | nil (25/s after 600 ms) | key repeat as `(RATE . DELAY)` |
| **Application buffers** | | |
| `waymacs-buffer-name-function` | `waymacs-buffer-name-default` | names an application's buffer |
| `waymacs-activate-function` | `waymacs-activate-default` | called when an application asks for attention or a taskbar entry is clicked |
| `waymacs-manage-finish-hook` | nil | runs in the buffer of a new application window |
| `waymacs-update-title-hook`, `waymacs-update-app-id-hook` | nil | run in an application's buffer when its title or app id changes |
| `waymacs-init-hook` | nil | runs once Emacs is connected (start waybar here) |
| **Workspaces and monitors** | | |
| `waymacs-workspace-initial-buffer` | `get-scratch-buffer-create` | function giving the buffer a new workspace shows |
| `waymacs-workspace-back-and-forth` | nil | `s-N` on the shown workspace goes back to the previous one |
| `waymacs-workspace-move-deletes-window` | t | moving a buffer to another workspace deletes its window |
| `waymacs-workspace-outputs` | `((internal 1 2 3 4 5) (external 6 7 8 9 10))` | the monitors workspaces belong on; nil: the focused one |
| `waymacs-internal-output-regexp` | names starting with `eDP`, `LVDS` or `DSI` | regexp matching built-in (laptop) screens, the `internal` above |
| `waymacs-lid-turns-off-screen` | t | closing the lid with another monitor connected turns the laptop screen off |
| `waymacs-workspace-show-in-mode-line` | `auto` | list the workspaces in the mode line: `auto` (unless waybar shows them), t, nil |

Other programs are set up outside Emacs: environment variables (see
Running), and the configurations in `contrib/` for waybar
(`contrib/waybar/`, see Status bar) and swaylock
(`contrib/swaylock/config`, a black lock screen).  Suspend on lid close is
systemd-logind's (or elogind's) `HandleLidSwitch*` settings.

### Application buffers

A new application window gets a buffer in `waymacs-mode`, named after its
title, and is shown in the selected window.  Dialogs and windows that
cannot be resized float above the frame instead.  Killing the buffer
closes the application (a second `kill-buffer` offers to kill a client
that does not close).  When an application closes, its buffer goes away.

The buffer-local variables `waymacs-app-id`, `waymacs-title` and
`waymacs-pid` describe the application; use them in
`display-buffer-alist` conditions or in the hooks
`waymacs-manage-finish-hook`, `waymacs-update-title-hook` and
`waymacs-update-app-id-hook`.  `waymacs-buffer-name-function` controls
the buffer name.

An application is drawn in one Emacs window only.  As in EXWM, when its
buffer ends up in a second window (after `C-x 3`, say) one window keeps
it, the selected one if possible, and the other shows the previous
buffer, so a split looks like any Emacs split.

Another Emacs started in the session (from a terminal, `s-d`...) is an
ordinary application: waymacs.el removes `WAYMACS_SOCKET` from the
environment of the programs Emacs starts, and the compositor refuses a
second Emacs while one is connected.

### Keyboard

- **Line mode** (the default): the application gets every key except
  `waymacs-input-prefix-keys` (`C-x C-u C-h M-x` ``M-` `` `M-& M-:`), the
  first keys of `waymacs-mode-map` bindings (`C-c`), and
  `waymacs-input-global-keys`.  Those go to Emacs, which keeps the
  keyboard until the command is done and then hands it back.
  `waymacs-input-simulation-keys` are translated by the compositor
  (holding them repeats).
- **Char mode**: the application gets every key except global keys.
  Applications matching `waymacs-input-char-mode-apps` start in char
  mode; by default that is Emacs itself, so `M-x`, `C-x`... typed in
  another Emacs's window reach that Emacs, not the session's.
- In a fullscreen window only global keys go to Emacs.

| Key | Command |
|---|---|
| `s-r` | `waymacs-reset`: line mode, leave fullscreen, resync |
| `s-i` | `waymacs-input-toggle-keyboard`: line / char mode |
| `s-d` | `waymacs-launch`: run a shell command |
| `s-f` | `waymacs-toggle-fullscreen` |
| `s-<left>` `s-<right>` `s-<up>` `s-<down>` | `windmove-*` |
| `s-1` … `s-9`, `s-0` | show workspace 1 … 9, 10 |
| `s-!` … `s-(`, `s-)` (Super+Shift+digit, US layout) | move the selected window to workspace 1 … 10 |
| `C-c C-k` | `waymacs-input-char-mode` |
| `C-c C-q` | `waymacs-input-send-next-key`: type the next key in the application |
| `C-c C-f` | `waymacs-toggle-fullscreen` |
| `C-c C-t C-f` | `waymacs-floating-toggle` |

The `s-` keys are `waymacs-input-global-keys`; change them with
`waymacs-input-set-key`.  The `C-c` keys are in `waymacs-mode-map`.

### Mouse

Clicking a window focuses it (and selects the Emacs window showing it).
`Super`+left drag moves a floating window, `Super`+right drag resizes it.

### Floating and fullscreen windows

`waymacs-floating-toggle` floats the application in the selected window,
or tiles the focused floating window into the selected window.  Showing a
floating window's buffer in an Emacs window tiles it as well.

Fullscreen windows cover their monitor.  When Emacs prompts for
something meanwhile (a global key such as `s-d`), the window stays on
screen and only its bottom is uncovered to show the minibuffer.

### Workspaces and monitors

Workspaces work as in i3 or sway.  Each workspace is an Emacs frame with
a number (its `waymacs-workspace` frame parameter) and its own window
layout; a monitor shows one workspace at a time.

- `s-N` shows workspace N.  If it does not exist it is made on the
  focused monitor, showing `*scratch*`
  (`waymacs-workspace-initial-buffer`).  If it is on another monitor,
  that monitor gets the focus.  With `waymacs-workspace-back-and-forth`,
  `s-N` on the shown workspace goes back to the previous one.
- `Super+Shift+N` moves the selected window's buffer (an application or
  any other buffer) to workspace N without following it.  If the target
  workspace's selected window already shows an application, that window
  is split so both stay visible.  The window it leaves is deleted, so the
  others take its space (`waymacs-workspace-move-deletes-window`); when it
  is the only window it shows the previous buffer instead.  The focused
  floating window moves the same way and stays floating.  A workspace
  made by a move stays hidden until it is shown.
- At start, and when a monitor is plugged in, every monitor without a
  workspace gets a new one, as does a monitor whose last workspace is
  deleted or moved away.  When a monitor is unplugged its workspaces move
  to another one.
- `waymacs-workspace-outputs` assigns workspaces to monitors, by name
  (`"HDMI-A-1"`; `waymacs-workspace-move-to-output` completes the names)
  or as `internal` (a laptop's own screen, `eDP-*`, `LVDS-*`, `DSI-*`)
  and `external` (any other).  A workspace is made on its monitor and
  moves back there when that monitor is plugged in; while it is unplugged
  the workspace lives on another monitor.  A monitor plugged in gets its
  workspaces, or the first of them is made.  By default workspaces 1-5
  are on the laptop screen and 6-0 on an external monitor:

  ```elisp
  (setq waymacs-workspace-outputs
        '((internal 1 2 3 4 5) (external 6 7 8 9 10)))
  ```

  With a single monitor all workspaces are on it.  Set it to nil to make
  every workspace on the focused monitor instead.
- Closing a laptop's lid while an external monitor is connected turns the
  laptop screen off; its workspaces move to the external monitor, so all
  of them are there.  Opening the lid turns the screen on again and its
  workspaces go back (`waymacs-lid-turns-off-screen`).  Whether closing
  the lid suspends the laptop is up to systemd-logind: by default it does
  not while an external monitor is connected (`HandleLidSwitchDocked`).
  `C-x 5 2` makes a workspace with the next free
  number, `C-x 5 o` cycles through them.
- `waymacs-workspace-move-to-output` moves the shown workspace to another
  monitor; `waymacs-workspace-delete` deletes it (its applications keep
  running, their buffers stay).
- The mode line lists the workspaces, the frame's own in brackets, unless
  a status bar such as waybar shows them
  (`waymacs-workspace-show-in-mode-line`: `auto`, `t` or `nil`).

The Super+Shift keys are what that combination types on a US layout
(`s-!` for 1, `s-@` for 2, ...).  With another layout bind
`waymacs-workspace-move-1` … `waymacs-workspace-move-10` to the
characters it types, for example `(waymacs-input-set-key (kbd "s-\"")
#'waymacs-workspace-move-2)`.  `waymacs-workspace-switch` and
`waymacs-workspace-move` take any number.

### Other commands and options

- `waymacs-exit` ends the session.
- `waymacs-input-send-key` types keys in an application from Lisp.
- `waymacs-activate-function` decides what happens when an application
  asks for attention through xdg-activation (by default its buffer is
  shown).
- `waymacs-keyboard-repeat` sets the key repeat rate and delay.
- `waymacs-init-hook` runs once Emacs is connected.

### Status bar (waybar)

waymacs speaks River's status protocols, so waybar's `river/tags` module
shows the workspaces (tag N is workspace N) and switches them with a
click, and `river/window` shows the focused window's title.  `wlr/taskbar`
works too (it lists the application windows; clicking one shows its
workspace and selects it).  waybar's default configuration
(`/etc/xdg/waybar/`) uses sway's modules, which show nothing here; a
configuration for waymacs is in `contrib/waybar/`: workspaces, the focused
window's title, the battery (hidden without one) and the clock.

```sh
waybar -c contrib/waybar/config.jsonc -s contrib/waybar/style.css
```

or copy it to `~/.config/waybar/` (`config.jsonc`, `style.css`) and run
`waybar`.  To start it with the session, from your init file:

```elisp
(add-hook 'waymacs-init-hook
          (lambda ()
            (start-process "waybar" nil "waybar"
                           "-c" "/path/to/waymacs/contrib/waybar/config.jsonc"
                           "-s" "/path/to/waymacs/contrib/waybar/style.css")))
```

While the bar shows the workspaces the mode line does not.  The bar's
space is taken from the Emacs frames (layer-shell exclusive zone).

## Supported Wayland protocols

xdg-shell, xdg-decoration and KDE server-decoration (applications are
asked not to draw title bars), wlr-layer-shell (bars, launchers,
wallpapers, notifications), xdg-activation, data-device and
primary-selection (clipboard), wlr-data-control (clipboard managers,
`wl-copy`), wlr-screencopy and export-dmabuf (screenshots, recording),
viewporter, fractional-scale, presentation-time, xdg-output,
single-pixel-buffer, cursor-shape, virtual-keyboard and virtual-pointer,
ext-session-lock (screen lockers such as `swaylock`), River's
river-status and river-control (waybar's `river/tags` and `river/window`),
wlr-foreign-toplevel-management (taskbars).  X11 applications run
through Xwayland, which starts when the first one connects.

While the screen is locked only the locker is shown and only it gets
input: keys are not passed to Emacs (global and prefix keys included),
Emacs cannot take the keyboard, and the emergency exit key is disabled.
If the locker dies without unlocking, the screen stays locked and turns
red; start a new locker (`swaylock`) to unlock it.  swaylock paints the
screen white unless told otherwise: `color=000000` in
`~/.config/swaylock/config` (or `swaylock -c 000000`) makes it black.
`contrib/swaylock/config` is such a file: copy it there, or run
`swaylock -C contrib/swaylock/config`.

Not supported yet: idle notification (`swayidle`), output management (`wlr-randr`, `kanshi`), gamma control,
pointer constraints and relative pointer (games), text input / input
methods, tablets and touch screens.

## Protocol between Emacs and the compositor

Emacs connects to the Unix socket `$WAYMACS_SOCKET`
(`$XDG_RUNTIME_DIR/waymacs-<display>.sock`).  Only one connection is
served: while Emacs is connected, only the same Emacs process may connect
again (replacing its old connection).

Emacs sends one command per line, words separated by spaces.  Keys are
written `MODIFIERS:cCODEPOINT` or `MODIFIERS:sKEYSYM-NAME`, MODIFIERS
being a sum of 1 Shift, 4 Control, 8 Alt (Meta), 32 Mod3 (Hyper),
64 Super; for example `4:c120` is `C-x` and `64:sLeft` is `s-<left>`.

| Command | Meaning |
|---|---|
| `hello PID` | Emacs runs as PID; its toplevels are Emacs frames.  The compositor answers with everything it knows |
| `layout F A X Y W H ...` | show application A in frame F at X,Y (frame relative), size W×H; applications left out are hidden (unless floating).  The first visible entry of an application wins |
| `focus ID` | give the keyboard to application or frame ID |
| `global-keys KEY...` | keys always sent to Emacs |
| `prefix-keys KEY...` | keys sent to Emacs from applications in line mode |
| `simulation-keys FROM=TO[,TO...] ...` | key translations for line mode |
| `mode ID line\|char` | input mode of application ID |
| `fullscreen ID 0\|1`, `float ID 0\|1` | window state |
| `assign ID FRAME` | put floating window ID on the workspace (frame) FRAME; 0 hides it |
| `workspaces FRAME NUMBER ...` | the workspace number of each frame, for status bars; frame 0 is a workspace not shown yet |
| `minibuffer FRAME Y` | the minibuffer of frame FRAME starts at Y (frame relative); fullscreen windows leave it uncovered while Emacs prompts |
| `frame-output FRAME OUTPUT` | move a workspace to the monitor named OUTPUT and show it |
| `output-enable OUTPUT 0\|1` | turn a monitor off (its workspaces move to another) or on; the last one stays on |
| `frame-place FRAME OUTPUT` | move a workspace to the monitor named OUTPUT without focusing it; it is shown there if that monitor shows nothing else, or if it has the focus |
| `close ID`, `kill ID` | ask the application to close / disconnect it |
| `key ID KEY...` | type keys in application ID |
| `xkb LAYOUT VARIANT OPTIONS` | keyboard layout (`-` for empty) |
| `repeat RATE DELAY` | key repeat |
| `quit` | end the session |

The compositor sends one S-expression per line, `(EVENT :key value ...)`:

| Event | Meaning |
|---|---|
| `(new :id :app :title :pid :floating)` | an application window appeared |
| `(update :id :app :title)` | its app id or title changed |
| `(close :id)` | it went away |
| `(activate :id)` | the user clicked it (or a floating window appeared) |
| `(fullscreen :id :state)` | fullscreen state changed |
| `(request-activate :id)` | it asks for attention (xdg-activation) |
| `(frame :id :tag)` | the Emacs frame titled TAG has id ID |
| `(need-frame :output)` | a monitor has no Emacs frame (workspace) |
| `(workspace-switch :number)`, `(workspace-move :number)` | a status bar asks to show workspace NUMBER, or to move the focused window there |
| `(bar :workspaces)` | whether a status bar now shows the workspaces |
| `(output :name :x :y :width :height :scale)`, `(output-removed :name)` | monitors |
| `(output-off :name)` | a monitor was turned off (it is also removed) |
| `(lid :closed)` | the laptop lid was closed (t) or opened (nil) |

Emacs frames are matched with compositor ids by giving each new frame the
title `waymacs-frame-N` until the `frame` event arrives.  A frame whose
workspace belongs on a monitor (`waymacs-workspace-outputs`) is titled
`waymacs-frame-N@OUTPUT`, and the compositor puts it on that monitor.

## Tests

`make test` runs `test/smoke.sh`: waymacs and `emacs-pgtk -Q` without a
display (wlroots headless backend, software rendering), with `foot` as the
application.  It types with `wtype`, clicks with `wlrctl`, and checks the
results through `emacsclient` and through files written by the shell in
`foot`.  With `xterm` (and `zenity`) installed it also runs X11
applications.  A second session with two headless monitors checks
workspaces assigned to monitors, and closing the lid (simulated from
Lisp), by the colours on screen.  `KEEP=1 make
test` keeps the log and screenshots.

GTK menus ignore keys typed with `wtype`: wtype uploads a new improvised
keymap each time, and GTK matches menu keys by keycode.  The menu test
therefore types through the compositor (`waymacs-input-send-key`), as a
physical keyboard does not have this problem.

## Known limitations

- wlroots 0.18 and 0.19 only; 0.20 changed the API again.
- X11 applications: no scaling (they are blurry on a scaled monitor),
  and windows that are not dialogs tile like Wayland ones.
- Emacs child frames (posframe, corfu popups...) are part of the Emacs
  frame, so application windows cover them where they overlap.
- Keys typed in the few milliseconds while the keyboard moves between
  Emacs and an application can reach the wrong one; printable keys and a
  few others that reach Emacs in an application buffer are passed on.
- A `read-key` prompt from a timer does not take the keyboard from an
  application (minibuffer prompts do).

## Source layout

| Path | Contents |
|---|---|
| `src/waymacs.h` | shared declarations |
| `src/main.cpp` | setup, protocol globals, starting Emacs |
| `src/ipc.cpp` | the Emacs connection |
| `src/view.cpp` | Emacs frames, application windows, popups, focus |
| `src/input.cpp` | keyboards, key interception and simulation, pointer |
| `src/output.cpp` | monitors and layer-shell surfaces |
| `src/lock.cpp` | screen lockers |
| `src/bar.cpp` | status bars and taskbars (River status protocols, foreign toplevels) |
| `src/xwayland.cpp` | X11 applications (Xwayland) |
| `lisp/waymacs.el` | the Emacs side |
| `protocols/` | protocol XML not shipped by wayland-protocols |
| `COPYING` | the GNU General Public License, version 3 |
| `contrib/waybar/` | waybar configuration and style for waymacs |
| `contrib/waymacs.desktop.in` | session file for display managers (`make install` fills in the paths) |
| `contrib/swaylock/` | swaylock configuration: a black lock screen |
| `contrib/waymacs-session.in` | starts waymacs through the login shell, for the session file |
| `test/` | test scripts and the minimal `init.el` they use |

## Third-party material

- `protocols/wlr-layer-shell-unstable-v1.xml` is taken unmodified from
  [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols),
  commit `2b8d43325b7012cc3f9b55c08d26e50e42beac7d` (protocol version 5).
  Copyright © 2017 Drew DeVault.  It is distributed under the permissive
  (HPND-style) license whose full text is kept at the top of the file.
- `protocols/river-status-unstable-v1.xml` and
  `protocols/river-control-unstable-v1.xml` are taken unmodified from the
  [River](https://codeberg.org/river/river) compositor, release v0.3.11
  (fetched from its GitHub mirror, `riverwm/river`).  Copyright 2020 The
  River Developers.  They are distributed under the ISC-style permission
  notice kept at the top of each file.  waymacs implements these
  protocols so that waybar's River modules work with it.
- At build time `wayland-scanner` generates C headers in `build/` from the
  wayland-protocols XML files `xdg-shell.xml`, `cursor-shape-v1.xml` and
  `tablet-unstable-v2.xml` (MIT license, copyright their authors; the
  generated headers carry their notices), and C headers and code from the
  River XML files above.  They are not part of the repository.
- No code is copied from other projects.  The design follows
  [EXWM](https://github.com/emacs-exwm/exwm) (line and char mode, prefix
  and simulation keys, one buffer per window) and was informed by
  [ewm](https://codeberg.org/ezemtsov/ewm), the compositor skeleton
  follows wlroots' `tinywl` example, and the layer-shell arrangement
  follows the approach of [dwl](https://codeberg.org/dwl/dwl).

## License

Copyright (C) 2026 Adam Quandour.

waymacs is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version (GPL-3.0-or-later).  It is distributed in the hope that
it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License in `COPYING` for more details.

Each source file says so with an `SPDX-License-Identifier` line.  The
protocol files in `protocols/` keep their own licenses (see Third-party
material).
