#!/bin/sh
# Copyright (C) 2026 Adam Quandour
# SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
# End-to-end smoke test.  Runs waymacs headless with foot as the
# application, drives it with wtype (keyboard) and wlrctl (pointer), and
# checks the result through emacsclient and through files written by the
# shell running in foot.
#
# Needs: emacs-pgtk, foot, wtype, wlrctl, grim (for the screenshots left
# in the work directory).
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
export WAYMACS_TEST_SERVER="waymacs-smoke-$$"
failures=0

elisp() { emacsclient -s "$WAYMACS_TEST_SERVER" -e "$1" 2>/dev/null; }

# wait_for EXPRESSION: wait up to 5 s for the elisp EXPRESSION to be t
wait_for() {
	for _ in $(seq 1 50); do
		[ "$(elisp "$1")" = t ] && return 0
		sleep 0.1
	done
	return 1
}

wait_for_file() {
	for _ in $(seq 1 50); do
		[ -s "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; failures=$((failures + 1)); }
check() { if "$@"; then pass "$name"; else fail "$name"; fi; }

# type in the focused window; pause so Emacs can hand the keyboard back
type_keys() { wtype "$@"; sleep 0.4; }

# is the middle of the screen dark (foot) rather than light (Emacs)?
center_is_dark() {
	sleep 0.5	# let the clients draw
	brightness=$(grim -t ppm -g '640,360 1x1' - | tail -c 3 | od -An -tu1 |
		awk '{ print $1 + $2 + $3 }')
	[ "${brightness:-765}" -lt 300 ]
}
center_is_light() { ! center_is_dark; }

foot_buffer='(cl-find "foot" (buffer-list) :key (lambda (buffer) (buffer-local-value (quote waymacs-app-id) buffer)) :test (function equal))'

"$top/test/run-headless.sh" > "$work/waymacs.log" 2>&1 &
compositor=$!
for _ in $(seq 1 100); do
	grep -q 'Emacs connected' "$work/waymacs.log" 2>/dev/null && break
	sleep 0.1
done
WAYLAND_DISPLAY=$(sed -n 's/.*running on WAYLAND_DISPLAY=//p' "$work/waymacs.log")
export WAYLAND_DISPLAY
echo "work directory: $work (WAYLAND_DISPLAY=$WAYLAND_DISPLAY)"

name="Emacs connects and its frame is paired"
check wait_for '(and (waymacs--connected-p) (frame-parameter nil (quote waymacs-id)) t)'

name="an application gets a buffer shown in the selected window"
elisp '(waymacs-launch "foot")' > /dev/null
check wait_for "(and (eq (window-buffer) $foot_buffer) t)"

name="the application gets the keyboard"
check wait_for '(equal waymacs--last-focus (list (waymacs--window-id)))'
sleep 0.5	# let the shell start

name="typing goes to the application"
type_keys "echo typed > $work/typed" -k Return
check wait_for_file "$work/typed"

name="prefix keys (C-x 3) go to Emacs"
type_keys -M ctrl x -m ctrl 3
check wait_for '(= (length (window-list)) 2)'

name="after a split the application stays in one window"
check wait_for "(and (eq (window-buffer) $foot_buffer) (= 1 (length (get-buffer-window-list $foot_buffer nil t))))"

name="the layout shows the application in the selected window"
check wait_for '(equal (cadr waymacs--last-layout) (cons (frame-parameter nil (quote waymacs-id)) (cons (waymacs--window-id) (waymacs--window-rect (selected-window) (waymacs--frame-offset (selected-frame))))))'

name="clicking the other window selects it"
wlrctl pointer move -3000 -3000
wlrctl pointer move 900 300
wlrctl pointer click left
check wait_for '(> (car (window-pixel-edges)) 0)'

name="clicking the application focuses it"
wlrctl pointer move -600 0
wlrctl pointer click left
check wait_for "(and (eq (window-buffer) $foot_buffer) (equal waymacs--last-focus (list (waymacs--window-id))))"

name="C-c C-k puts the application in char mode"
type_keys -M ctrl ck -m ctrl
check wait_for "(and (buffer-local-value (quote waymacs--char-mode) $foot_buffer) t)"

name="in char mode C-c goes to the application"
type_keys "cat > $work/cat" -k Return
type_keys -M ctrl c -m ctrl
type_keys "echo interrupted > $work/interrupted" -k Return
check wait_for_file "$work/interrupted"

name="the global key s-i goes back to line mode"
type_keys -M logo i -m logo
check wait_for "(not (buffer-local-value (quote waymacs--char-mode) $foot_buffer))"

name="simulation keys (C-t -> Z) are translated"
elisp '(customize-set-variable (quote waymacs-input-simulation-keys) (quote (([?\C-t] . [?Z]))))' > /dev/null
type_keys "echo sim"
type_keys -M ctrl t -m ctrl
type_keys " > $work/simulated" -k Return
wait_for_file "$work/simulated"
check grep -qx simZ "$work/simulated"
grim "$work/tiled.png" 2> /dev/null

# screen locking (ext-session-lock-v1)
center_color() { grim -t ppm -g '640,360 1x1' - | tail -c 3 | od -An -tu1; }
if command -v swaylock > /dev/null; then
	swaylock -c 336699 > "$work/swaylock.log" 2>&1 &
	locker=$!
	sleep 1.5
	name="swaylock locks the screen"
	check sh -c '[ "$(echo $0)" = "51 102 153" ]' "$(center_color)"
	name="while locked keys reach neither the application nor Emacs"
	type_keys "echo leaked > $work/leaked" -k Return
	type_keys -M logo d -m logo
	elisp '(waymacs--sync t)' > /dev/null	# Emacs asking for the keyboard
	type_keys "echo leaked > $work/leaked2" -k Return
	nothing_leaked() { [ ! -e "$work/leaked" ] && [ ! -e "$work/leaked2" ] &&
		[ "$(elisp '(and (active-minibuffer-window) t)')" = nil ]; }
	check nothing_leaked
	name="unlocking gives the keyboard back"
	kill -USR1 "$locker"
	sleep 1
	type_keys "echo unlocked > $work/unlocked" -k Return
	check wait_for_file "$work/unlocked"
	name="a crashed locker leaves the screen locked"
	swaylock -c 336699 > /dev/null 2>&1 &
	locker=$!
	sleep 1.5
	kill -9 "$locker"
	sleep 1
	type_keys "echo leaked > $work/leaked3" -k Return
	still_locked() { [ "$(echo $(center_color))" = "153 0 0" ] && [ ! -e "$work/leaked3" ]; }
	check still_locked
	name="a new locker can unlock it"
	swaylock -c 336699 > /dev/null 2>&1 &
	locker=$!
	sleep 1.5
	kill -USR1 "$locker"
	sleep 1
	type_keys "echo unlocked > $work/unlocked2" -k Return
	check wait_for_file "$work/unlocked2"
else
	echo "SKIP screen locking (swaylock not installed)"
fi

name="fullscreen"
elisp '(waymacs-toggle-fullscreen)' > /dev/null
check wait_for "(and (buffer-local-value (quote waymacs--fullscreen) $foot_buffer) t)"
grim "$work/fullscreen.png" 2> /dev/null
elisp '(waymacs-toggle-fullscreen)' > /dev/null
wait_for "(not (buffer-local-value (quote waymacs--fullscreen) $foot_buffer))"

name="floating"
elisp '(waymacs-floating-toggle)' > /dev/null
check wait_for "(and waymacs--floating-focus (not (get-buffer-window $foot_buffer)) t)"
grim "$work/floating.png" 2> /dev/null
# its buffer is in no window, yet its keys (C-c C-t C-f) work
name="C-c C-t C-f typed in the floating window tiles it"
type_keys -M ctrl ctf -m ctrl
check wait_for "(and (get-buffer-window $foot_buffer) (not (buffer-local-value (quote waymacs--floating) $foot_buffer)) t)"

name="M-x over a floating window acts on it"
elisp '(waymacs-floating-toggle)' > /dev/null
wait_for "(and waymacs--floating-focus t)"
type_keys -M alt x -m alt
type_keys "waymacs-floating-toggle" -k Return
check wait_for "(and (get-buffer-window $foot_buffer) t)"

name="menus of applications get the keyboard (GTK3)"
if python3 -c 'import gi; gi.require_version("Gtk", "3.0")' 2> /dev/null; then
	GDK_BACKEND=wayland python3 "$top/test/gtk-menu.py" > "$work/menu.txt" 2>&1 &
	wait_for '(equal (buffer-local-value (quote waymacs-title) (window-buffer)) "gtk-menu-test")'
	# typed by the compositor itself: wtype's improvised keymaps upset
	# GTK's keycode-based menu bindings
	for key in m '<down>' RET; do
		elisp "(waymacs-input-send-key \"$key\" (waymacs--window-id))" > /dev/null
		sleep 0.5
	done
	check grep -q 'activated First item' "$work/menu.txt"
	elisp '(kill-buffer (window-buffer))' > /dev/null
else
	echo "SKIP $name (python3-gi not installed)"
fi

name="the first frame is workspace 1"
check wait_for '(eql (waymacs-workspace-current) 1)'

name="s-@ moves the application to workspace 2 without showing it"
elisp "(progn (delete-other-windows) (switch-to-buffer $foot_buffer))" > /dev/null
sleep 0.5
type_keys -M logo @ -m logo
check wait_for "(let ((frame (waymacs-workspace--frame 2))) (and frame (not (frame-visible-p frame)) (eq (window-buffer (frame-selected-window frame)) $foot_buffer) (not (get-buffer-window $foot_buffer)) (eql (waymacs-workspace-current) 1)))"

name="s-2 shows workspace 2 with the application focused"
type_keys -M logo 2 -m logo
check wait_for "(and (eql (waymacs-workspace-current) 2) (frame-parameter nil (quote waymacs-id)) (equal waymacs--last-focus (list (buffer-local-value (quote waymacs--id) $foot_buffer))))"

name="workspace 2 is on screen"
check center_is_dark
grim "$work/workspace2.png" 2> /dev/null

name="the mode line lists the workspaces"
check wait_for '(equal (waymacs-workspace-mode-line) "1 [2]")'

name="s-1 goes back to workspace 1"
type_keys -M logo 1 -m logo
check wait_for '(eql (waymacs-workspace-current) 1)'

name="workspace 1 is on screen again"
check center_is_light
grim "$work/workspace1.png" 2> /dev/null

# a status bar (waybar with contrib/waybar's configuration)
if command -v waybar > /dev/null; then
	waybar -c "$top/contrib/waybar/config.jsonc" -s "$top/contrib/waybar/style.css" \
		> "$work/waybar.log" 2>&1 &
	bar=$!
	name="waybar reserves its space"
	check wait_for '(< (frame-pixel-height) 720)'
	name="Emacs hides its workspace list while the bar shows it"
	check wait_for '(null (waymacs--mode-line-workspaces))'
	name="clicking tag 2 in waybar shows workspace 2"
	wlrctl pointer move -3000 -3000
	wlrctl pointer move 41 15
	wlrctl pointer click left
	check wait_for '(eql (waymacs-workspace-current) 2)'
	grim "$work/waybar.png" 2> /dev/null
	type_keys -M logo 1 -m logo
	kill "$bar"
	name="without the bar the frame and the workspace list come back"
	check wait_for '(and (= (frame-pixel-height) 720) (waymacs--mode-line-workspaces) t)'
else
	echo "SKIP status bar (waybar not installed)"
fi

name="another Emacs started in the session is an application"
emacs_buffer='(cl-find-if (lambda (buffer) (string-match-p "emacs" (or (buffer-local-value (quote waymacs-app-id) buffer) ""))) (buffer-list))'
# the worst case: it loads waymacs and calls waymacs-enable too
elisp "(waymacs-launch \"WAYMACS_TEST_SERVER=waymacs-smoke-child-$$ emacs-pgtk -Q -l $top/test/init.el -l $top/lisp/waymacs.el -f waymacs-enable\")" > /dev/null
check wait_for "(and $emacs_buffer (waymacs--connected-p) (frame-parameter nil (quote waymacs-id)) t)"

name="another Emacs starts in char mode"
check wait_for "(and (eq (window-buffer) $emacs_buffer) (buffer-local-value (quote waymacs--char-mode) $emacs_buffer))"
type_keys -M alt x -m alt
name="M-x typed in another Emacs goes to it"
check_m_x() { [ "$(elisp '(and (active-minibuffer-window) t)')" = nil ]; }
check check_m_x
type_keys -M ctrl g -m ctrl

name="another Emacs's new frame goes in another window, leaving it in view"
child_elisp() { emacsclient -s "waymacs-smoke-child-$$" -e "$1" 2>/dev/null; }
emacs_windows='(cl-count-if (lambda (window) (string-match-p "emacs" (or (buffer-local-value (quote waymacs-app-id) (window-buffer window)) ""))) (window-list))'
child_elisp '(progn (make-frame (quote ((name . "second")))) t)' > /dev/null
check wait_for "(= $emacs_windows 2)"
child_elisp '(delete-frame (cl-find "second" (frame-list) :key (lambda (frame) (frame-parameter frame (quote name))) :test (function equal)))' > /dev/null
wait_for "(= $emacs_windows 1)"
elisp "(progn (select-window (get-buffer-window $emacs_buffer)) (delete-other-windows))" > /dev/null
elisp "(kill-buffer $emacs_buffer)" > /dev/null
wait_for "(null $emacs_buffer)"

name="a second connection is refused"
second_connection_refused() {
	python3 -c '
import socket, sys
connection = socket.socket(socket.AF_UNIX)
connection.connect(sys.argv[1])
connection.sendall(b"hello 1\n")
connection.settimeout(3)
try:
    reply = connection.recv(100)
except ConnectionResetError:
    reply = b""
sys.exit(0 if reply == b"" else 1)
' "$XDG_RUNTIME_DIR/waymacs-$WAYLAND_DISPLAY.sock"
}
check second_connection_refused

name="the session Emacs stays connected"
check wait_for '(and (waymacs--connected-p) (frame-parameter nil (quote waymacs-id)) t)'

if command -v Xwayland > /dev/null && command -v xterm > /dev/null; then
	xterm_buffer='(cl-find "XTerm" (buffer-list) :key (lambda (buffer) (buffer-local-value (quote waymacs-app-id) buffer)) :test (function equal))'
	name="an X11 application (Xwayland) gets a buffer shown in the selected window"
	elisp '(progn (delete-other-windows) (waymacs-launch "xterm"))' > /dev/null
	check wait_for "(and (eq (window-buffer) $xterm_buffer) (equal waymacs--last-focus (list (waymacs--window-id))) t)"
	sleep 1	# let the shell start
	name="typing goes to the X11 application"
	# through the compositor's keyboard: some Xwayland builds cannot
	# compile the improvised keymaps wtype uploads
	elisp "(dolist (key (append (string-to-list \"echo x11 > $work/x11\") (list (quote return)))) (waymacs-input-send-key key))" > /dev/null
	check wait_for_file "$work/x11"
	if command -v zenity > /dev/null; then
		name="an X11 dialog floats"
		elisp '(waymacs-launch "GDK_BACKEND=x11 zenity --info --text=hello")' > /dev/null
		check wait_for '(and (cl-find-if (lambda (buffer) (and (equal (buffer-local-value (quote waymacs-app-id) buffer) "zenity") (buffer-local-value (quote waymacs--floating) buffer))) (buffer-list)) t)'
		pkill -x zenity
	fi
	name="killing the buffer closes the X11 application"
	elisp "(kill-buffer $xterm_buffer)" > /dev/null
	check wait_for "(null $xterm_buffer)"
else
	echo "SKIP X11 applications (Xwayland or xterm not installed)"
fi

name="killing the buffer closes the application"
elisp "(kill-buffer $foot_buffer)" > /dev/null
check wait_for "(null $foot_buffer)"

name="the session ends cleanly with Emacs"
elisp '(kill-emacs)' > /dev/null
wait "$compositor"
check [ $? -eq 0 ]

# ---- two monitors, workspaces assigned to them ----
# The headless outputs are HEADLESS-2 at 0,0 and HEADLESS-1 at 1280,0.
export WAYMACS_TEST_SERVER="waymacs-smoke-outputs-$$"
WLR_HEADLESS_OUTPUTS=2 \
WAYMACS_TEST_SETUP="(setq waymacs-workspace-outputs '((\"HEADLESS-1\" 1 2 3) (\"HEADLESS-2\" 6 7 8)))" \
	"$top/test/run-headless.sh" > "$work/outputs.log" 2>&1 &
compositor=$!
for _ in $(seq 1 100); do
	grep -q 'Emacs connected' "$work/outputs.log" 2>/dev/null && break
	sleep 0.1
done
WAYLAND_DISPLAY=$(sed -n 's/.*running on WAYLAND_DISPLAY=//p' "$work/outputs.log")
color_frames='(dolist (frame (frame-list)) (set-frame-parameter frame (quote background-color) (pcase (frame-parameter frame (quote waymacs-workspace)) (1 "red") (2 "green") (6 "blue") (7 "yellow") (_ "white"))))'
# the colour at the middle of each output, e.g. "blue red"
output_colors() {
	elisp "$color_frames" > /dev/null
	sleep 0.5
	for position in 640,360 1920,360; do
		grim -t ppm -g "$position 1x1" - | tail -c 3 | od -An -tu1 |
			awk '{ print (($1 > 200 && $2 > 200 && $3 > 200) ? "white" : ($1 > 200 && $2 > 200) ? "yellow" : $1 > 200 ? "red" : $2 > 100 ? "green" : $3 > 200 ? "blue" : "other") }'
	done | tr '\n' ' ' | sed 's/ $//'
}
name="the other monitor gets the first workspace assigned to it"
check wait_for '(and (waymacs-workspace--frame 6) (frame-parameter (waymacs-workspace--frame 6) (quote waymacs-id)) t)'
name="each monitor shows its own workspace"
check [ "$(output_colors)" = "blue red" ]
name="a workspace is made on the monitor it is assigned to"
elisp '(waymacs-workspace-switch 7)' > /dev/null
sleep 1
check [ "$(output_colors)" = "yellow red" ]
elisp '(waymacs-workspace-switch 2)' > /dev/null
sleep 1
check [ "$(output_colors)" = "yellow green" ]
place_workspaces() {
	elisp "(progn (setq waymacs-workspace-outputs '($1)) (mapc (function waymacs-workspace--place) (waymacs-workspace-frames)))" > /dev/null
	sleep 1
}
name="the focused workspace moves to its new monitor and stays shown"
place_workspaces '("HEADLESS-1" 1 3) ("HEADLESS-2" 2 6 7 8)'
check [ "$(output_colors)" = "green red" ]
name="another workspace moves without being shown; the monitor it left gets one"
place_workspaces '("HEADLESS-1" 3) ("HEADLESS-2" 1 2 6 7 8)'
check [ "$(output_colors)" = "green white" ]
# HEADLESS-1 plays the laptop screen; the lid is closed from Lisp, as
# the compositor does it on the lid switch
elisp '(setq waymacs-internal-output-regexp "\\`HEADLESS-1")' > /dev/null
name="closing the lid turns the laptop screen off"
elisp "(waymacs--dispatch '(lid :closed t))" > /dev/null
check wait_for '(equal (mapcar (function car) waymacs--outputs) (list "HEADLESS-2"))'
name="its workspace is shown on the other monitor"
elisp '(waymacs-workspace-switch 3)' > /dev/null
sleep 1
check [ "$(output_colors | cut -d' ' -f1)" = white ]
name="opening the lid turns it on with its workspace"
elisp '(waymacs-workspace-switch 2)' > /dev/null
elisp "(waymacs--dispatch '(lid :closed nil))" > /dev/null
check wait_for '(= (length waymacs--outputs) 2)'
sleep 1
check [ "$(output_colors)" = "green white" ]
elisp '(kill-emacs)' > /dev/null
wait "$compositor"

if [ "$failures" -eq 0 ]; then
	echo "all tests passed"
	# KEEP=1 keeps the log and screenshots
	[ -n "${KEEP:-}" ] || rm -rf "$work"
else
	echo "$failures test(s) failed; log and screenshots in $work"
	exit 1
fi
