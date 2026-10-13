;;; waymacs.el --- Emacs as a Wayland window manager  -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Adam Quandour

;; Author: Adam Quandour <adam.quandour@gmail.com>
;; Version: 0.1
;; Package-Requires: ((emacs "29.1"))
;; Keywords: unix, frames

;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; waymacs makes Emacs the window manager of a Wayland session, in the
;; spirit of EXWM.  The `waymacs' program is a small wlroots compositor
;; that starts Emacs (a pgtk build) and lets this package decide
;; everything else:
;;
;; - every application window gets a buffer in `waymacs-mode';
;; - displaying that buffer in an Emacs window places the application
;;   there, so `split-window', `display-buffer-alist', tab-bar
;;   workspaces etc. are the window manager;
;; - the application gets the keyboard while its window is selected,
;;   except for prefix keys (C-x, M-x, ...) and global keys, which go
;;   to Emacs ("line mode").  In "char mode" only global keys do.
;;
;; Put this in your init file and start the `waymacs' program:
;;
;;   (require 'waymacs)
;;   (waymacs-enable)

;;; Code:

(require 'cl-lib)
(require 'subr-x)

(defgroup waymacs nil
  "Emacs as a Wayland window manager."
  :group 'environment
  :prefix "waymacs-")


(defun waymacs--set-and-update-keys (symbol value)
  "Set SYMBOL to VALUE and tell the compositor about the new keys."
  (set-default-toplevel-value symbol value)
  (when (fboundp 'waymacs-input--update-keys)
    (waymacs-input--update-keys)))

(defcustom waymacs-input-prefix-keys
  '(?\C-x ?\C-u ?\C-h ?\M-x ?\M-` ?\M-& ?\M-:)
  "Keys that go to Emacs while an application window in line mode has focus.
Each element is an event (a character or a symbol such as `f1').
Keys bound in `waymacs-mode-map' are added automatically."
  :type '(repeat sexp)
  :initialize #'custom-initialize-default
  :set #'waymacs--set-and-update-keys)

(defcustom waymacs-input-global-keys
  `((,(kbd "s-r") . waymacs-reset)
    (,(kbd "s-i") . waymacs-input-toggle-keyboard)
    (,(kbd "s-d") . waymacs-launch)
    (,(kbd "s-f") . waymacs-toggle-fullscreen)
    (,(kbd "s-<left>") . windmove-left)
    (,(kbd "s-<right>") . windmove-right)
    (,(kbd "s-<up>") . windmove-up)
    (,(kbd "s-<down>") . windmove-down)
    ;; s-1 ... s-0: show workspace 1 ... 10
    ,@(mapcar (lambda (number)
                (cons (kbd (format "s-%d" (% number 10)))
                      (intern (format "waymacs-workspace-switch-%d" number))))
              (number-sequence 1 10))
    ;; Super+Shift+1 ... 0 on a US layout: move there
    ,@(cl-mapcar (lambda (number key)
                   (cons (kbd (concat "s-" key))
                         (intern (format "waymacs-workspace-move-%d" number))))
                 (number-sequence 1 10)
                 '("!" "@" "#" "$" "%" "^" "&" "*" "(" ")")))
  "Global key bindings that work everywhere, even in char mode.
An alist of (KEY . COMMAND).  The bindings are made in `global-map' by
`waymacs-enable'; change them later with `waymacs-input-set-key'.

The keys moving windows to workspaces are what Super+Shift+digit types
on a US keyboard layout (s-! for 1, s-@ for 2...); with another layout,
bind `waymacs-workspace-move-1' ... `waymacs-workspace-move-10' to the
characters that layout types."
  :type '(alist :key-type key-sequence :value-type function)
  :initialize #'custom-initialize-default
  :set (lambda (symbol value)
         (waymacs--set-and-update-keys symbol value)
         (when (bound-and-true-p waymacs--process)
           (dolist (binding value)
             (global-set-key (car binding) (cdr binding))))))

(defcustom waymacs-input-simulation-keys nil
  "Keys translated into other keys for applications in line mode.
An alist of (KEY . TARGET), both key sequences; only the first event of
KEY matters.  The compositor does the translation, so key repeat works.
Example, for Emacs-like movement in browsers:
  \\=`(([?\\C-b] . [left]) ([?\\C-f] . [right]) ([?\\C-p] . [up])
    ([?\\C-n] . [down]) ([?\\C-a] . [home]) ([?\\C-e] . [end])
    ([?\\M-v] . [prior]) ([?\\C-v] . [next]) ([?\\C-d] . [delete])
    ([?\\C-k] . [S-end delete]) ([?\\M-w] . [?\\C-c]) ([?\\C-y] . [?\\C-v]))"
  :type '(alist :key-type key-sequence :value-type key-sequence)
  :initialize #'custom-initialize-default
  :set #'waymacs--set-and-update-keys)

(defcustom waymacs-input-char-mode-apps '("\\`[Ee]macs\\(-pgtk\\)?\\'")
  "Regexps matching app ids of applications that start in char mode.
The default matches Emacs: in line mode the prefix keys (`M-x', `C-x'...)
typed in another Emacs's window would go to the session's Emacs instead.
Terminals are good candidates too (app id \"foot\", \"kitty\"...)."
  :type '(repeat regexp))

(defcustom waymacs-xkb-layout nil
  "XKB layout(s) for keyboards, such as \"us,ru\".  nil keeps the default."
  :type '(choice (const nil) string))

(defcustom waymacs-xkb-variant nil
  "XKB variant(s) for keyboards, such as \"dvorak\".  nil keeps the default."
  :type '(choice (const nil) string))

(defcustom waymacs-xkb-options nil
  "XKB options, such as \"ctrl:nocaps,grp:alt_shift_toggle\"."
  :type '(choice (const nil) string))

(defcustom waymacs-keyboard-repeat nil
  "Key repeat as (RATE . DELAY): keys per second and milliseconds.
nil keeps the compositor default (25 . 600)."
  :type '(choice (const nil) (cons integer integer)))

(defcustom waymacs-buffer-name-function #'waymacs-buffer-name-default
  "Function returning the name of an application buffer.
It is called in the buffer, where `waymacs-title' and `waymacs-app-id'
are set.  nil means never rename the buffer."
  :type '(choice (const nil) function))

(defcustom waymacs-activate-function #'waymacs-activate-default
  "Function called with a buffer whose application asks to be activated.
Applications do that through xdg-activation, e.g. a browser asked to
open a link by another program."
  :type 'function)

(defcustom waymacs-manage-finish-hook nil
  "Hook run in the buffer of a new application window.
The window is not yet displayed when this runs."
  :type 'hook)

(defcustom waymacs-update-title-hook nil
  "Hook run in an application buffer after its title changed."
  :type 'hook)

(defcustom waymacs-update-app-id-hook nil
  "Hook run in an application buffer after its app id changed."
  :type 'hook)

(defcustom waymacs-init-hook nil
  "Hook run once waymacs is connected to the compositor."
  :type 'hook)

(defcustom waymacs-workspace-initial-buffer #'get-scratch-buffer-create
  "Function returning the buffer a new workspace shows."
  :type 'function)

(defcustom waymacs-workspace-back-and-forth nil
  "Non-nil means switching to the shown workspace goes to the previous one."
  :type 'boolean)

(defcustom waymacs-workspace-move-deletes-window t
  "Non-nil means moving a buffer to another workspace deletes its window.
The other windows then take its place, as in tiling window managers.
The window stays when it is the only one of its frame."
  :type 'boolean)

(defcustom waymacs-workspace-outputs
  '((internal 1 2 3 4 5) (external 6 7 8 9 10))
  "The monitors workspaces belong on, as an alist (OUTPUT . NUMBERS).
OUTPUT is a monitor name such as \"eDP-1\" or \"HDMI-A-1\" (see
`waymacs-workspace-move-to-output'), `internal' for the built-in screen
of a laptop (see `waymacs-internal-output-regexp') or `external' for the
other monitors.  A workspace is made on its monitor, and moves there
when that monitor is connected; while it is not, the workspace lives on
another one.  Workspace 10 is the one on s-0.  Workspaces not listed
are made on the focused monitor; nil assigns none.

The default puts 1 to 5 on a laptop's own screen and 6 to 0 on a
monitor plugged into it; with a single monitor, all are on it."
  :type '(alist :key-type (choice (const :tag "Built-in screen" internal)
                                  (const :tag "Other monitors" external)
                                  (string :tag "Monitor name"))
                :value-type (repeat (integer :tag "Workspace"))))

(defcustom waymacs-internal-output-regexp "\\`\\(eDP\\|LVDS\\|DSI\\)"
  "Regexp matching the names of built-in screens, for `waymacs-workspace-outputs'."
  :type 'regexp)

(defcustom waymacs-lid-turns-off-screen t
  "Non-nil means closing a laptop's lid turns its screen off.
Only while another monitor is connected: the workspaces of the laptop
screen move to the other monitor.  Opening the lid turns the screen on
again, and the workspaces that belong there (`waymacs-workspace-outputs')
go back.  The screen is the one `waymacs-internal-output-regexp' matches."
  :type 'boolean)

(defcustom waymacs-workspace-show-in-mode-line 'auto
  "Whether the mode line lists the workspaces, the shown one in brackets.
`auto' shows them unless a status bar (waybar) does."
  :type '(choice (const :tag "Unless a status bar shows them" auto)
                 (const :tag "Always" t)
                 (const :tag "Never" nil)))

;;; State

(defvar waymacs--process nil
  "The connection to the compositor.")

(defvar waymacs--partial ""
  "Input from the compositor not yet terminated by a newline.")

(defvar waymacs--buffers (make-hash-table)
  "Application window id -> buffer.")

(defvar waymacs--outputs nil
  "Alist of (NAME . PLIST) describing the outputs.")

(defvar waymacs--outputs-off nil
  "Names of the monitors turned off, with the lid closed.")

(defvar waymacs--lid-closed nil
  "Non-nil while the laptop lid is closed.")

(defvar waymacs--frame-counter 0
  "Counter used to make frame tags.")

(defvar waymacs--last-layout nil
  "The last layout command sent.")

(defvar waymacs--last-focus nil
  "The last focus request sent, as (ID . FRAME-P).")

(defvar waymacs--last-window nil
  "The selected window when the focus was last computed.")

(defvar waymacs--floating-focus nil
  "Id of the floating window that has the focus, if any.")

(defvar waymacs--pending-display nil
  "New application buffers waiting for the minibuffer to exit.")

(defvar waymacs--making-background-frame nil
  "Non-nil while making a frame that should not be selected.")

(defvar waymacs--last-minibuffer nil
  "The last minibuffer position sent, as (FRAME-ID . TOP).")

(defvar waymacs--bar-shows-workspaces nil
  "Non-nil while a status bar shows the workspaces.")

(defvar waymacs--last-workspaces nil
  "The last workspace numbers sent, as a list of FRAME-ID NUMBER ...")

(defvar waymacs--shown-windows nil
  "Windows that showed an application at the last sync.")

(defvar waymacs--sync-timer nil
  "Timer for a focus update postponed by pending input.")

(defvar-local waymacs--id nil
  "Id of the application window of this buffer.")

(defvar-local waymacs-app-id nil
  "The application id (Wayland app_id) of this buffer's window.")

(defvar-local waymacs-title nil
  "The title of this buffer's application window.")

(defvar-local waymacs-pid nil
  "The process id of this buffer's application.")

(defvar-local waymacs--floating nil
  "Non-nil if this buffer's window floats.")

(defvar-local waymacs--fullscreen nil
  "Non-nil if this buffer's window is fullscreen.")

(defvar-local waymacs--char-mode nil
  "Non-nil if this buffer's window is in char mode.")

(defvar-local waymacs--close-requested nil
  "Non-nil once the application was asked to close.")

;;; Connection

(defun waymacs--connected-p ()
  "Return non-nil if connected to the compositor."
  (process-live-p waymacs--process))

(defun waymacs--send (&rest args)
  "Send a command made of ARGS (atoms or lists of atoms) to the compositor."
  (when (waymacs--connected-p)
    (process-send-string
     waymacs--process
     (concat (mapconcat (lambda (arg) (format "%s" arg)) (flatten-tree args) " ")
             "\n"))))

(defun waymacs--filter (_process string)
  "Handle STRING, output of the compositor connection."
  (setq waymacs--partial (concat waymacs--partial string))
  (let (newline)
    (while (setq newline (string-search "\n" waymacs--partial))
      (let ((line (substring waymacs--partial 0 newline)))
        (setq waymacs--partial (substring waymacs--partial (1+ newline)))
        (condition-case-unless-debug err
            (waymacs--dispatch (car (read-from-string line)))
          (error (message "waymacs: %S while handling %s" err line))))))
  (waymacs--sync))

(defun waymacs--sentinel (_process event)
  "Report EVENT, a change in the compositor connection."
  (message "waymacs: connection %s" (string-trim event)))

(defvar waymacs--socket nil
  "The compositor's socket, taken from $WAYMACS_SOCKET at start.")

(defun waymacs--take-socket ()
  "Take the compositor socket out of the environment.
Programs started from Emacs must not see it: another Emacs started
there would otherwise connect and take over the window management."
  (let ((socket (getenv "WAYMACS_SOCKET")))
    (when socket
      (setq waymacs--socket socket)
      (setenv "WAYMACS_SOCKET" nil))
    waymacs--socket))

(defun waymacs--connect ()
  "Connect to the compositor at `waymacs--socket'."
  (when (waymacs--connected-p)
    (delete-process waymacs--process))
  (setq waymacs--partial ""
        waymacs--outputs nil
        waymacs--outputs-off nil
        waymacs--lid-closed nil
        waymacs--last-minibuffer nil
        waymacs--last-workspaces nil
        waymacs--last-layout nil
        waymacs--last-focus nil)
  (setq waymacs--process
        (make-network-process :name "waymacs" :family 'local
                              :service waymacs--socket
                              :coding 'utf-8-unix :noquery t
                              :filter #'waymacs--filter
                              :sentinel #'waymacs--sentinel))
  (waymacs--send "hello" (emacs-pid)))

;;; Application buffers

(defvar-keymap waymacs-mode-map
  :doc "Keymap of `waymacs-mode'.
In line mode the first key of every binding here goes to Emacs,
except keys bound to `waymacs-input-forward-key'."
  "C-c C-k" #'waymacs-input-char-mode
  "C-c C-q" #'waymacs-input-send-next-key
  "C-c C-f" #'waymacs-toggle-fullscreen
  "C-c C-t C-f" #'waymacs-floating-toggle
  ;; Keys that reach Emacs by accident (typed while focus was moving)
  ;; are passed on to the application.
  "<remap> <self-insert-command>" #'waymacs-input-forward-key
  "RET" #'waymacs-input-forward-key "<return>" #'waymacs-input-forward-key
  "TAB" #'waymacs-input-forward-key "<tab>" #'waymacs-input-forward-key
  "DEL" #'waymacs-input-forward-key "<backspace>" #'waymacs-input-forward-key
  "<delete>" #'waymacs-input-forward-key "<escape>" #'waymacs-input-forward-key
  "<left>" #'waymacs-input-forward-key "<right>" #'waymacs-input-forward-key
  "<up>" #'waymacs-input-forward-key "<down>" #'waymacs-input-forward-key
  "<home>" #'waymacs-input-forward-key "<end>" #'waymacs-input-forward-key
  "<prior>" #'waymacs-input-forward-key "<next>" #'waymacs-input-forward-key)

(define-derived-mode waymacs-mode nil "Waymacs"
  "Major mode of buffers standing for Wayland application windows.
Displaying the buffer in a window shows the application there.

\\{waymacs-mode-map}"
  (buffer-disable-undo)
  (setq-local buffer-read-only t
              cursor-type nil
              left-fringe-width 0
              right-fringe-width 0
              left-margin-width 0
              right-margin-width 0
              show-trailing-whitespace nil
              mode-line-process '(:eval (waymacs--mode-line-process)))
  (add-hook 'kill-buffer-query-functions #'waymacs--kill-buffer-query nil t))

(defun waymacs--mode-line-process ()
  "Mode line indicator for an application buffer."
  (concat (if waymacs--char-mode " [char]" " [line]")
          (and waymacs--floating " [float]")
          (and waymacs--fullscreen " [full]")))

(defun waymacs--buffer (id)
  "Return the live buffer of application window ID, or nil."
  (let ((buffer (and id (gethash id waymacs--buffers))))
    (and (buffer-live-p buffer) buffer)))

(defun waymacs--window-id (&optional window)
  "Id of the application shown in WINDOW, or nil."
  (buffer-local-value 'waymacs--id (window-buffer window)))

(defun waymacs-buffer-name-default ()
  "Name an application buffer after its title, or else its app id."
  (cond ((and waymacs-title (not (string-empty-p waymacs-title))) waymacs-title)
        ((and waymacs-app-id (not (string-empty-p waymacs-app-id))) waymacs-app-id)
        (t "*waymacs*")))

(defun waymacs--rename-buffer ()
  "Rename the current application buffer with `waymacs-buffer-name-function'."
  (when waymacs-buffer-name-function
    (rename-buffer (funcall waymacs-buffer-name-function) t)))

(defun waymacs--kill-buffer-query ()
  "Close the application instead of killing its buffer right away.
The buffer goes away once the window does."
  (cond ((not (and waymacs--id (waymacs--connected-p))) t)
        ((and waymacs--close-requested
              (yes-or-no-p "The application did not close; kill it? "))
         (waymacs--send "kill" waymacs--id)
         nil)
        (t (setq waymacs--close-requested t)
           (waymacs--send "close" waymacs--id)
           nil)))

(defun waymacs-activate-default (buffer)
  "Show and select BUFFER, or focus it if it floats.
A buffer shown on another workspace is selected there, which shows that
workspace, as a taskbar click does in other window managers."
  (cond ((buffer-local-value 'waymacs--floating buffer)
         (setq waymacs--floating-focus (buffer-local-value 'waymacs--id buffer)
               waymacs--last-window (selected-window)))
        ((when-let* ((window (get-buffer-window buffer t)))
           (make-frame-visible (window-frame window))
           (select-frame-set-input-focus (window-frame window))
           (select-window window)))
        (t (pop-to-buffer buffer))))

;;; Events from the compositor

(defun waymacs--dispatch (event)
  "Handle EVENT, a list (NAME . PLIST) sent by the compositor."
  (let ((properties (cdr event)))
    (pcase (car event)
      ('new (waymacs--on-new properties))
      ('update (waymacs--on-update properties))
      ('close (waymacs--on-close properties))
      ('activate (waymacs--on-activate properties))
      ('fullscreen (waymacs--on-fullscreen properties))
      ('request-activate
       (when-let* ((buffer (waymacs--buffer (plist-get properties :id))))
         (funcall waymacs-activate-function buffer)))
      ('frame (waymacs--on-frame properties))
      ('need-frame (waymacs--make-output-frame (plist-get properties :output)))
      ;; clicks in a status bar
      ('workspace-switch (waymacs-workspace-switch (plist-get properties :number)))
      ('workspace-move (waymacs-workspace-move (plist-get properties :number)))
      ('bar (setq waymacs--bar-shows-workspaces (plist-get properties :workspaces))
            (force-mode-line-update t))
      ('output (waymacs--on-output properties)
               (waymacs--update-lid))
      ('output-removed
       (setq waymacs--outputs (assoc-delete-all (plist-get properties :name) waymacs--outputs))
       (waymacs--update-lid))
      ('output-off (cl-pushnew (plist-get properties :name) waymacs--outputs-off
                               :test #'equal))
      ('lid (setq waymacs--lid-closed (plist-get properties :closed))
            (waymacs--update-lid)))))

(defun waymacs--internal-output-p (name)
  "Non-nil if monitor NAME is a laptop's own screen."
  (string-match-p waymacs-internal-output-regexp name))

(defun waymacs--update-lid ()
  "Turn the laptop screen off or on as `waymacs-lid-turns-off-screen' says.
Off while the lid is closed and another monitor is on; on otherwise."
  (let* ((names (mapcar #'car waymacs--outputs))
         (external (cl-remove-if #'waymacs--internal-output-p names)))
    (if (and waymacs-lid-turns-off-screen waymacs--lid-closed external)
        (dolist (name (cl-remove-if-not #'waymacs--internal-output-p names))
          (waymacs--send "output-enable" name 0))
      (dolist (name waymacs--outputs-off)
        (waymacs--send "output-enable" name 1))
      (setq waymacs--outputs-off nil))))

(defun waymacs--on-output (event)
  "Remember the monitor described in EVENT.
A monitor just connected gets the workspaces that belong there."
  (let* ((name (plist-get event :name))
         (new (not (assoc name waymacs--outputs))))
    (setf (alist-get name waymacs--outputs nil nil #'equal) event)
    (when (and new waymacs-workspace-outputs)
      (mapc #'waymacs-workspace--place (waymacs-workspace-frames)))))

(defun waymacs--on-new (event)
  "Make a buffer for the application window described by EVENT, a plist."
  (let* ((id (plist-get event :id))
         (old (waymacs--buffer id))
         (buffer (or old (generate-new-buffer "*waymacs*"))))
    (puthash id buffer waymacs--buffers)
    (with-current-buffer buffer
      (unless old
        (waymacs-mode))
      (setq waymacs--id id
            waymacs-app-id (plist-get event :app)
            waymacs-title (plist-get event :title)
            waymacs-pid (plist-get event :pid)
            waymacs--floating (plist-get event :floating))
      (unless old
        (waymacs--rename-buffer)
        (run-hooks 'waymacs-update-app-id-hook 'waymacs-update-title-hook)
        (when (cl-some (lambda (regexp) (string-match-p regexp (or waymacs-app-id "")))
                       waymacs-input-char-mode-apps)
          (waymacs-input--set-char-mode t))
        (run-hooks 'waymacs-manage-finish-hook)))
    (unless (or old (buffer-local-value 'waymacs--floating buffer))
      (if (active-minibuffer-window)
          (push buffer waymacs--pending-display)
        (pop-to-buffer-same-window buffer)))))

(defun waymacs--on-update (event)
  "Record the new app id and title in EVENT."
  (when-let* ((buffer (waymacs--buffer (plist-get event :id))))
    (with-current-buffer buffer
      (let ((app (plist-get event :app))
            (title (plist-get event :title)))
        (unless (and (equal app waymacs-app-id) (equal title waymacs-title))
          (let ((app-changed (not (equal app waymacs-app-id)))
                (title-changed (not (equal title waymacs-title))))
            (setq waymacs-app-id app
                  waymacs-title title)
            (waymacs--rename-buffer)
            (when app-changed (run-hooks 'waymacs-update-app-id-hook))
            (when title-changed (run-hooks 'waymacs-update-title-hook))))))))

(defun waymacs--on-close (event)
  "Kill the buffer of the window that EVENT says is gone."
  (let* ((id (plist-get event :id))
         (buffer (waymacs--buffer id)))
    (remhash id waymacs--buffers)
    (when (eql waymacs--floating-focus id)
      (setq waymacs--floating-focus nil))
    (when buffer
      (with-current-buffer buffer
        (setq waymacs--id nil))
      (kill-buffer buffer))))

(defun waymacs--on-activate (event)
  "Follow the compositor focusing the window in EVENT (e.g. after a click)."
  (let* ((id (plist-get event :id))
         (buffer (waymacs--buffer id)))
    (setq waymacs--floating-focus nil)
    (cond ((null buffer))
          ((buffer-local-value 'waymacs--floating buffer)
           (setq waymacs--floating-focus id
                 waymacs--last-window (selected-window)))
          ((when-let* ((window (get-buffer-window buffer 'visible)))
             (select-frame (window-frame window))
             (select-window window))))))

(defun waymacs--on-fullscreen (event)
  "Record the fullscreen state in EVENT."
  (when-let* ((buffer (waymacs--buffer (plist-get event :id))))
    (with-current-buffer buffer
      (setq waymacs--fullscreen (plist-get event :state)))
    (force-mode-line-update t)))

;;; Frames

(defun waymacs--frame-p (frame)
  "Non-nil if FRAME is a top-level frame on the Wayland display."
  (and (eq (framep frame) 'pgtk)
       (not (frame-parameter frame 'parent-frame))))

(defun waymacs--tag-frame (frame)
  "Title FRAME with a unique tag so the compositor can name it."
  (when (and (waymacs--frame-p frame)
             (not (frame-parameter frame 'waymacs-tag)))
    (set-frame-parameter frame 'waymacs-tag
                         (format "waymacs-frame-%d" (cl-incf waymacs--frame-counter)))
    (set-frame-parameter frame 'undecorated t)
    ;; Every frame is a workspace.
    (unless (frame-parameter frame 'waymacs-workspace)
      (set-frame-parameter frame 'waymacs-workspace (waymacs-workspace--free-number)))
    (waymacs--title-frame frame)
    ;; Frames made interactively are where the user wants to be, as
    ;; under any window manager.
    (unless waymacs--making-background-frame
      (select-frame frame))))

(defun waymacs--title-frame (frame)
  "Title FRAME, not yet known to the compositor, with its tag.
The title is \"TAG@OUTPUT\" when its workspace belongs on monitor OUTPUT:
the compositor then shows it there from the start."
  (let ((output (waymacs-workspace--output (frame-parameter frame 'waymacs-workspace))))
    (set-frame-parameter frame 'title (concat (frame-parameter frame 'waymacs-tag)
                                              (and output (concat "@" output))))))

(defun waymacs--on-frame (event)
  "Remember the compositor id of the frame tagged as said in EVENT."
  (dolist (frame (frame-list))
    (when (equal (frame-parameter frame 'waymacs-tag) (plist-get event :tag))
      (let ((id (plist-get event :id)))
        (set-frame-parameter frame 'waymacs-id id)
        (set-frame-parameter frame 'title nil)
        (waymacs-workspace--place frame)
        ;; floating windows moved here before the frame was ever shown
        (dolist (app-id (frame-parameter frame 'waymacs-pending-floating))
          (waymacs--send "assign" app-id id))
        (set-frame-parameter frame 'waymacs-pending-floating nil)))))

(defun waymacs--make-frame (parameters &optional background)
  "Make a frame with PARAMETERS showing `waymacs-workspace-initial-buffer'.
With BACKGROUND, do not select it."
  (let ((waymacs--making-background-frame background)
        (graphical-frame (if (waymacs--frame-p (selected-frame))
                             (selected-frame)
                           (cl-find-if #'waymacs--frame-p (frame-list)))))
    (if graphical-frame
        (with-selected-frame graphical-frame
          ;; A new frame shows the current buffer, which must not be an
          ;; application: the application would move to the new frame.
          ;; (Selecting a frame changes the current buffer, so this
          ;; comes after it.)
          (with-current-buffer (funcall waymacs-workspace-initial-buffer)
            (make-frame parameters)))
      (with-current-buffer (funcall waymacs-workspace-initial-buffer)
        (make-frame-on-display (getenv "WAYLAND_DISPLAY") parameters)))))

(defun waymacs--make-output-frame (output)
  "Give OUTPUT, a monitor showing no workspace, a workspace.
The workspaces that belong there (`waymacs-workspace-outputs') move
there, or the first of them is made; else a new workspace is made with
a number that belongs on no monitor."
  (let* ((numbers (waymacs-workspace--numbers-on output))
         (frames (delq nil (mapcar #'waymacs-workspace--frame numbers))))
    (cond (frames (mapc #'waymacs-workspace--place frames))
          (numbers (waymacs--make-frame `((waymacs-workspace . ,(car numbers))) t))
          (t (waymacs--make-frame
              `((waymacs-workspace . ,(waymacs-workspace--free-number t)))
              t)))))

(defun waymacs--after-select-frame (frame &rest _)
  "Show FRAME, after `select-frame-set-input-focus' selected it."
  (when-let* ((id (frame-parameter frame 'waymacs-id)))
    (setq waymacs--floating-focus nil
          waymacs--last-focus (cons id t))
    (waymacs--send "focus" id)))

;;; Layout and focus

(defun waymacs--frame-offset (frame)
  "Offset (DX . DY) of FRAME's native area inside its window."
  (let ((outer (frame-edges frame 'outer-edges))
        (native (frame-edges frame 'native-edges)))
    (cons (- (nth 0 native) (nth 0 outer))
          (- (nth 1 native) (nth 1 outer)))))

(defun waymacs--window-rect (window offset)
  "List (X Y W H): WINDOW's text area, OFFSET being its frame's offset."
  (pcase-let* ((`(,left ,top ,right ,bottom) (window-inside-pixel-edges window))
               ;; Before Emacs 31 the body edges ignore the tab line.
               (tab-line (if (< emacs-major-version 31)
                             (window-tab-line-height window)
                           0)))
    (list (+ (car offset) left) (+ (cdr offset) top tab-line)
          (- right left) (- bottom top tab-line))))

(defun waymacs--layout ()
  "The layout command describing where applications are shown.
Selected windows come first: when a buffer is shown in several windows,
the compositor uses the first one it can."
  (let ((frames (cons (selected-frame) (delq (selected-frame) (frame-list))))
        entries)
    (dolist (frame frames)
      (when-let* ((frame-id (and (frame-visible-p frame)
                                 (frame-parameter frame 'waymacs-id))))
        (let ((offset nil))
          (dolist (window (window-list frame 'never))
            (when-let* ((id (waymacs--window-id window)))
              ;; A window shown in an Emacs window is tiled, not floating.
              (with-current-buffer (window-buffer window)
                (setq waymacs--floating nil))
              (unless offset
                (setq offset (waymacs--frame-offset frame)))
              (push (cons frame-id (cons id (waymacs--window-rect window offset)))
                    entries))))))
    (cons "layout" (nreverse entries))))

(defun waymacs--emacs-input-p ()
  "Non-nil if Emacs is reading keys for itself."
  (or (minibufferp (window-buffer (selected-window)))
      overriding-terminal-local-map
      overriding-local-map
      prefix-arg))

(defun waymacs--focus-target ()
  "Where the keyboard should be, as (ID . FRAME-P), or nil."
  (let* ((window (selected-window))
         (frame-id (frame-parameter (window-frame window) 'waymacs-id))
         (id (waymacs--window-id window)))
    (unless (eq window waymacs--last-window)
      (setq waymacs--last-window window
            waymacs--floating-focus nil))
    (cond ((waymacs--emacs-input-p) (and frame-id (cons frame-id t)))
          ((waymacs--buffer waymacs--floating-focus) (cons waymacs--floating-focus nil))
          (id (cons id nil))
          (frame-id (cons frame-id t)))))

(defun waymacs--replace-in-window (window buffer)
  "Show something other than the application BUFFER in WINDOW."
  (let ((switch-to-prev-buffer-skip
         ;; not BUFFER, nor an application shown somewhere else
         (lambda (_window candidate _bury-or-kill)
           (or (eq candidate buffer)
               (and (buffer-local-value 'waymacs--id candidate)
                    (get-buffer-window candidate t))))))
    (switch-to-prev-buffer window))
  (when (eq (window-buffer window) buffer)
    (set-window-buffer window (funcall waymacs-workspace-initial-buffer))))

(defun waymacs--show-applications-once ()
  "Show every application in one window only, as EXWM does.
A Wayland window has one size and position, so it can only be drawn in
one Emacs window; other windows showing its buffer (after `C-x 3', for
example) switch to another buffer.  The selected window keeps it, or
else the window that showed it first."
  (maphash
   (lambda (_id buffer)
     (let ((windows (and (buffer-live-p buffer)
                         (get-buffer-window-list buffer 'never t))))
       (when (cdr windows)
         (let ((keep (if (memq (selected-window) windows)
                         (selected-window)
                       (or (cl-find-if (lambda (window)
                                         (and (memq window windows)
                                              (eq (window-buffer window) buffer)))
                                       waymacs--shown-windows)
                           (car windows)))))
           (dolist (window windows)
             (unless (eq window keep)
               (waymacs--replace-in-window window buffer)))))))
   waymacs--buffers)
  (setq waymacs--shown-windows
        (cl-remove-if-not (lambda (window) (waymacs--window-id window))
                          (window-list-1 nil 'never t))))

(defun waymacs--sync (&optional force-focus)
  "Send the layout and the focus to the compositor if they changed.
With FORCE-FOCUS, send the focus even if it did not change."
  (when (waymacs--connected-p)
    (waymacs--show-applications-once)
    (let ((layout (waymacs--layout)))
      (unless (equal layout waymacs--last-layout)
        (setq waymacs--last-layout layout)
        (waymacs--send layout)))
    ;; workspace numbers, for status bars
    ;; (a workspace never shown has no id yet: 0)
    (let ((workspaces (cl-loop for frame in (waymacs-workspace-frames)
                               append (list (or (frame-parameter frame 'waymacs-id) 0)
                                            (frame-parameter frame 'waymacs-workspace)))))
      (unless (equal workspaces waymacs--last-workspaces)
        (setq waymacs--last-workspaces workspaces)
        (waymacs--send "workspaces" workspaces)))
    ;; where the minibuffer starts: a fullscreen window leaves it visible
    ;; while Emacs prompts
    (when-let* ((frame (selected-frame))
                (frame-id (frame-parameter frame 'waymacs-id))
                (minibuffer (minibuffer-window frame))
                ((eq (window-frame minibuffer) frame))
                (position (cons frame-id
                                (+ (cdr (waymacs--frame-offset frame))
                                   (nth 1 (window-pixel-edges minibuffer))))))
      (unless (equal position waymacs--last-minibuffer)
        (setq waymacs--last-minibuffer position)
        (waymacs--send "minibuffer" (car position) (cdr position))))
    (let ((target (waymacs--focus-target)))
      (when (and target (or force-focus (not (equal target waymacs--last-focus))))
        (setq waymacs--last-focus target)
        (waymacs--send "focus" (car target))))))

(defun waymacs--post-command ()
  "Hand the keyboard back to an application once a command is done."
  (when (and waymacs--pending-display (not (active-minibuffer-window)))
    (dolist (buffer (nreverse (prog1 waymacs--pending-display
                                (setq waymacs--pending-display nil))))
      (when (and (buffer-live-p buffer) (not (get-buffer-window buffer 'visible)))
        (pop-to-buffer-same-window buffer))))
  (if (input-pending-p)
      ;; More keys are on their way to Emacs; keep the keyboard until
      ;; they are handled, then send the focus.
      (unless waymacs--sync-timer
        (setq waymacs--sync-timer
              (run-with-idle-timer 0 nil (lambda ()
                                           (setq waymacs--sync-timer nil)
                                           (waymacs--sync t)))))
    (waymacs--sync t)))

;;; Keys

(defun waymacs--key-vector (key)
  "KEY (a vector, a `kbd' string, a string of events or an event) as a vector."
  (cond ((vectorp key) key)
        ((stringp key) (if (key-valid-p key) (key-parse key) (vconcat key)))
        (t (vector key))))

(defun waymacs--keysym-name (symbol)
  "The XKB keysym name of the Emacs function key SYMBOL."
  (let ((name (symbol-name symbol)))
    (or (cdr (assq symbol '((deletechar . "Delete") (iso-lefttab . "ISO_Left_Tab")
                            (backtab . "ISO_Left_Tab"))))
        (if (string-prefix-p "kp-" name) (concat "KP_" (substring name 3)) name))))

(defun waymacs--key-spec (event)
  "The compositor's spelling of EVENT: MODS:cCODEPOINT or MODS:sKEYSYM."
  (let ((modifier-bits 0) base)
    (if (integerp event)
        (let ((char (logand event #x3fffff)))
          (dolist (modifier-bit '((25 . 1) (26 . 4) (27 . 8) (24 . 32) (23 . 64)))
            (unless (zerop (logand event (ash 1 (car modifier-bit))))
              (setq modifier-bits (logior modifier-bits (cdr modifier-bit)))))
          (setq base (pcase char
                       (9 "sTab") (13 "sReturn") (27 "sEscape") (127 "sBackSpace")
                       ((pred (> 32))
                        (setq modifier-bits (logior modifier-bits 4))
                        (format "c%d" (+ char 96)))
                       (_ (format "c%d" char)))))
      (let ((modifiers (event-modifiers event))
            (basic (event-basic-type event)))
        (dolist (modifier '((shift . 1) (control . 4) (meta . 8) (hyper . 32) (super . 64)))
          (when (memq (car modifier) modifiers)
            (setq modifier-bits (logior modifier-bits (cdr modifier)))))
        (setq base (cond ((integerp basic) (format "c%d" basic))
                         ((symbolp basic) (concat "s" (waymacs--keysym-name basic)))))))
    (and base (format "%d:%s" modifier-bits base))))

(defun waymacs--first-event (key)
  "The first event of KEY."
  (aref (waymacs--key-vector key) 0))

(defun waymacs--mode-map-events ()
  "First events of the bindings in `waymacs-mode-map' that go to Emacs."
  (let (events)
    (map-keymap
     (lambda (event binding)
       (cond ((memq binding '(waymacs-input-forward-key undefined ignore nil)))
             ((eq event 'remap))
             ;; M-x lives in the keymap as ESC x
             ((and (eq event meta-prefix-char) (keymapp binding))
              (map-keymap (lambda (key _binding) (push (event-apply-modifier key 'meta 27 "M-") events))
                          binding))
             (t (push event events))))
     waymacs-mode-map)
    events))

(defun waymacs-input--update-keys ()
  "Tell the compositor which keys to intercept and translate."
  (when (waymacs--connected-p)
    (waymacs--send "global-keys"
                   (delq nil (mapcar (lambda (binding) (waymacs--key-spec (waymacs--first-event (car binding))))
                                     waymacs-input-global-keys)))
    (waymacs--send "prefix-keys"
                   (delq nil (mapcar (lambda (key) (waymacs--key-spec (waymacs--first-event key)))
                                     (append waymacs-input-prefix-keys (waymacs--mode-map-events)))))
    (waymacs--send "simulation-keys"
                   (delq nil (mapcar (lambda (simulation)
                                       (let ((from (waymacs--key-spec (waymacs--first-event (car simulation))))
                                             (to (delq nil (mapcar #'waymacs--key-spec
                                                                   (waymacs--key-vector (cdr simulation))))))
                                         (and from to (concat from "=" (string-join to ",")))))
                                     waymacs-input-simulation-keys)))))

(defun waymacs-input-set-key (key command)
  "Bind KEY to COMMAND globally, including while applications have focus."
  (interactive "KSet key globally: \nCSet key %s to command: ")
  (setf (alist-get key waymacs-input-global-keys nil nil #'equal) command)
  (global-set-key key command)
  (waymacs-input--update-keys))

(defun waymacs--current-id ()
  "Id of the application in the selected window, or signal an error."
  (or (waymacs--window-id) (user-error "Not an application window")))

(defun waymacs-input--set-char-mode (on)
  "Put the current buffer's application in char mode if ON, else line mode."
  (setq waymacs--char-mode on)
  (waymacs--send "mode" waymacs--id (if on "char" "line"))
  (force-mode-line-update))

(defun waymacs-input-char-mode ()
  "Send all keys except global ones to the selected application."
  (interactive)
  (with-current-buffer (window-buffer)
    (waymacs--current-id)
    (waymacs-input--set-char-mode t)))

(defun waymacs-input-line-mode ()
  "Let prefix keys of the selected application go to Emacs."
  (interactive)
  (with-current-buffer (window-buffer)
    (waymacs--current-id)
    (waymacs-input--set-char-mode nil)))

(defun waymacs-input-toggle-keyboard ()
  "Toggle the selected application between line and char mode."
  (interactive)
  (with-current-buffer (window-buffer)
    (waymacs--current-id)
    (waymacs-input--set-char-mode (not waymacs--char-mode))
    (message "waymacs: %s mode" (if waymacs--char-mode "char" "line"))))

(defun waymacs-input-send-key (key &optional id)
  "Type KEY (a key sequence) in application window ID, by default the selected one."
  (let ((id (or id (waymacs--current-id)))
        (specs (delq nil (mapcar #'waymacs--key-spec (waymacs--key-vector key)))))
    (while specs
      (waymacs--send "key" id (seq-take specs 8))
      (setq specs (nthcdr 8 specs)))))

(defun waymacs-input-send-next-key (times)
  "Send the next key (or the next TIMES keys) to the selected application."
  (interactive "p")
  (let ((id (waymacs--current-id)))
    (dotimes (i times)
      (waymacs-input-send-key
       (vector (read-event (format "Send key to application (%d/%d): " (1+ i) times)))
       id))))

(defun waymacs-input-forward-key ()
  "Type the last key in this buffer's application."
  (interactive)
  (when waymacs--id
    (waymacs-input-send-key (vector last-input-event) waymacs--id)))

;;; Commands

(defun waymacs-launch (command)
  "Run the shell COMMAND, typically a graphical application."
  (interactive (list (read-shell-command "$ ")))
  (start-process-shell-command command nil command))

(defun waymacs-toggle-fullscreen ()
  "Toggle fullscreen for the application in the selected window."
  (interactive)
  (let ((buffer (or (and (waymacs--window-id) (window-buffer))
                    (waymacs--buffer waymacs--floating-focus)
                    (user-error "Not an application window"))))
    (with-current-buffer buffer
      (waymacs--send "fullscreen" waymacs--id (if waymacs--fullscreen 0 1)))))

(defun waymacs-floating-toggle ()
  "Float the application in the selected window, or tile the focused floating one."
  (interactive)
  (let ((floating (waymacs--buffer waymacs--floating-focus))
        (id (waymacs--window-id)))
    (cond (floating
           (setq waymacs--floating-focus nil)
           (with-current-buffer floating (setq waymacs--floating nil))
           (waymacs--send "float" (buffer-local-value 'waymacs--id floating) 0)
           (switch-to-buffer floating))
          (id
           (let ((buffer (window-buffer)))
             (with-current-buffer buffer (setq waymacs--floating t))
             (waymacs--send "float" id 1)
             (replace-buffer-in-windows buffer)
             (setq waymacs--floating-focus id
                   waymacs--last-window (selected-window))))
          (t (user-error "Not an application window")))))

(defun waymacs-reset ()
  "Reset the selected application: line mode, not fullscreen; resync everything."
  (interactive)
  (when-let* ((id (waymacs--window-id)))
    (with-current-buffer (window-buffer)
      (waymacs-input--set-char-mode nil))
    (waymacs--send "fullscreen" id 0))
  (setq waymacs--last-layout nil
        waymacs--floating-focus nil)
  (waymacs-input--update-keys)
  (waymacs--sync t))

(defun waymacs-exit ()
  "End the session: quit the compositor (and with it Emacs)."
  (interactive)
  (when (yes-or-no-p "Quit the waymacs session? ")
    (save-some-buffers)
    (waymacs--send "quit")))

;;; Workspaces
;;
;; Every Emacs frame is a workspace with a number (the frame parameter
;; `waymacs-workspace').  A monitor shows one workspace at a time; the
;; compositor shows a frame when it gets the focus.  Workspaces made in
;; the background (to move a window there) are invisible frames until
;; they are first shown.

(defvar waymacs-workspace--previous nil
  "Number of the workspace shown before the current one.")

(defun waymacs-workspace-current ()
  "Number of the selected frame's workspace."
  (frame-parameter (selected-frame) 'waymacs-workspace))

(defun waymacs-workspace--frame (number)
  "The frame of workspace NUMBER, or nil."
  (cl-find number (frame-list)
           :key (lambda (frame) (frame-parameter frame 'waymacs-workspace))))

(defun waymacs-workspace-frames ()
  "Frames that are workspaces, by number."
  (sort (cl-remove-if-not (lambda (frame) (frame-parameter frame 'waymacs-workspace))
                          (frame-list))
        (lambda (first second)
          (< (frame-parameter first 'waymacs-workspace)
             (frame-parameter second 'waymacs-workspace)))))

(defun waymacs-workspace--free-number (&optional unassigned)
  "The smallest number no workspace has.
With UNASSIGNED, also one that belongs on no connected monitor."
  (let ((number 1))
    (while (or (waymacs-workspace--frame number)
               (and unassigned (waymacs-workspace--output number)))
      (cl-incf number))
    number))

(defun waymacs--output-matches-p (output name)
  "Non-nil if OUTPUT, a key of `waymacs-workspace-outputs', means monitor NAME."
  (pcase output
    ('internal (string-match-p waymacs-internal-output-regexp name))
    ('external (not (string-match-p waymacs-internal-output-regexp name)))
    (_ (equal output name))))

(defun waymacs-workspace--output (number)
  "The connected monitor workspace NUMBER belongs on, or nil."
  (let ((names (sort (mapcar #'car waymacs--outputs) #'string<)))
    (cl-loop for (output . numbers) in waymacs-workspace-outputs
             when (memq number numbers)
             thereis (cl-find-if (lambda (name) (waymacs--output-matches-p output name))
                                 names))))

(defun waymacs-workspace--numbers-on (name)
  "The workspace numbers that belong on monitor NAME, in order."
  (sort (cl-remove-if-not (lambda (number) (equal (waymacs-workspace--output number) name))
                          (delete-dups (apply #'append
                                              (mapcar #'cdr waymacs-workspace-outputs))))
        #'<))

(defun waymacs-workspace--place (frame)
  "Put FRAME's workspace on the monitor it belongs on, if any.
A frame the compositor does not know yet is placed when it learns about
it (the `frame' event)."
  (when-let* ((id (frame-parameter frame 'waymacs-id))
              (number (frame-parameter frame 'waymacs-workspace))
              (output (waymacs-workspace--output number)))
    (waymacs--send "frame-place" id output)))

(defun waymacs-workspace--make (number &optional background)
  "Make workspace NUMBER; with BACKGROUND, make it without showing it."
  (waymacs--make-frame `((waymacs-workspace . ,number)
                         ,@(and background '((visibility . nil))))
                       background))

(defun waymacs-workspace-switch (number)
  "Show workspace NUMBER, making it on the focused monitor if needed.
A workspace that belongs on a monitor (`waymacs-workspace-outputs') is
made there.
A workspace on another monitor is shown there and gets the focus."
  (interactive "nWorkspace: ")
  (let ((current (waymacs-workspace-current)))
    (when (and (eql number current) waymacs-workspace-back-and-forth
               waymacs-workspace--previous)
      (setq number waymacs-workspace--previous))
    (unless (eql number current)
      (let ((frame (or (waymacs-workspace--frame number)
                       (waymacs-workspace--make number))))
        ;; a workspace made in the background is shown for the first
        ;; time: monitors may have come or gone since it was made
        (unless (frame-parameter frame 'waymacs-id)
          (waymacs--title-frame frame))
        (setq waymacs-workspace--previous current)
        (make-frame-visible frame)
        (select-frame-set-input-focus frame)))))

(defun waymacs-workspace--window-for (frame)
  "The window of FRAME that should receive a moved buffer.
Its selected window, or a new window split from it when that window
shows an application already, so that both stay visible."
  (let ((window (frame-selected-window frame)))
    (if (waymacs--window-id window)
        (or (split-window-sensibly window) (split-window window nil 'right))
      window)))

(defun waymacs--assign-floating (buffer frame)
  "Put the floating window of BUFFER on FRAME's workspace."
  (let ((id (buffer-local-value 'waymacs--id buffer))
        (frame-id (frame-parameter frame 'waymacs-id)))
    (if frame-id
        (waymacs--send "assign" id frame-id)
      ;; the compositor learns about the frame when it is first shown
      (push id (frame-parameter frame 'waymacs-pending-floating))
      (waymacs--send "assign" id 0))))

(defun waymacs-workspace-move (number)
  "Move the selected buffer, or the focused floating window, to workspace NUMBER.
The workspace is made if needed, without showing it.  See also
`waymacs-workspace-move-deletes-window'."
  (interactive "nMove to workspace: ")
  (unless (eql number (waymacs-workspace-current))
    (let ((target (or (waymacs-workspace--frame number)
                      (waymacs-workspace--make number t)))
          (floating (waymacs--buffer waymacs--floating-focus)))
      (if floating
          (progn
            (setq waymacs--floating-focus nil)
            (waymacs--assign-floating floating target))
        (let ((window (selected-window))
              (buffer (window-buffer)))
          (when (minibufferp buffer)
            (user-error "Cannot move the minibuffer"))
          (set-window-buffer (waymacs-workspace--window-for target) buffer)
          (if (and waymacs-workspace-move-deletes-window (not (one-window-p t)))
              (delete-window window)
            (switch-to-prev-buffer window)))))))

(defun waymacs-workspace-move-to-output (output)
  "Move the shown workspace to monitor OUTPUT and show it there."
  (interactive (list (completing-read "Move workspace to monitor: "
                                      (mapcar #'car waymacs--outputs) nil t)))
  (when-let* ((id (frame-parameter (selected-frame) 'waymacs-id)))
    (waymacs--send "frame-output" id output)))

(defun waymacs-workspace-delete ()
  "Delete the shown workspace.  Its applications keep running, hidden."
  (interactive)
  (unless (cdr (waymacs-workspace-frames))
    (user-error "This is the only workspace"))
  (delete-frame))

(dotimes (index 10)
  (let ((number (1+ index)))
    (defalias (intern (format "waymacs-workspace-switch-%d" number))
      (lambda () (interactive) (waymacs-workspace-switch number))
      (format "Show workspace %d." number))
    (defalias (intern (format "waymacs-workspace-move-%d" number))
      (lambda () (interactive) (waymacs-workspace-move number))
      (format "Move the selected buffer or floating window to workspace %d." number))))

(defun waymacs-workspace-mode-line ()
  "The workspace list for the mode line, the frame's own in brackets."
  ;; while a mode line is computed its window is the selected one
  (let ((shown (window-frame (selected-window))))
    (mapconcat (lambda (frame)
                 (format (if (eq frame shown) "[%d]" "%d")
                         (frame-parameter frame 'waymacs-workspace)))
               (waymacs-workspace-frames) " ")))

(defun waymacs--mode-line-workspaces ()
  "The workspace list, if `waymacs-workspace-show-in-mode-line' says so."
  (when (if (eq waymacs-workspace-show-in-mode-line 'auto)
            (not waymacs--bar-shows-workspaces)
          waymacs-workspace-show-in-mode-line)
    (waymacs-workspace-mode-line)))

(defconst waymacs--mode-line-construct '(:eval (waymacs--mode-line-workspaces))
  "What `waymacs-workspace-show-in-mode-line' adds to `global-mode-string'.")

;;; Setup

(defun waymacs--configure-keyboard ()
  "Send the keyboard options to the compositor."
  (when (or waymacs-xkb-layout waymacs-xkb-variant waymacs-xkb-options)
    (waymacs--send "xkb" (or waymacs-xkb-layout "-") (or waymacs-xkb-variant "-")
                   (or waymacs-xkb-options "-")))
  (when waymacs-keyboard-repeat
    (waymacs--send "repeat" (car waymacs-keyboard-repeat) (cdr waymacs-keyboard-repeat))))

(defun waymacs--init ()
  "Connect to the compositor and start managing windows."
  (cond
   ((not (waymacs--take-socket))
    (message "waymacs: not started by the waymacs compositor ($WAYMACS_SOCKET unset)"))
   ((not (featurep 'pgtk))
    (display-warning 'waymacs "waymacs needs an Emacs built with --with-pgtk"))
   (t
    (add-to-list 'default-frame-alist '(undecorated . t))
    (dolist (binding waymacs-input-global-keys)
      (global-set-key (car binding) (cdr binding)))
    (waymacs--connect)
    (waymacs-input--update-keys)
    (waymacs--configure-keyboard)
    (add-hook 'post-command-hook #'waymacs--post-command)
    (add-hook 'window-state-change-hook #'waymacs--sync)
    (add-hook 'minibuffer-setup-hook #'waymacs--sync)
    (add-hook 'after-make-frame-functions #'waymacs--tag-frame)
    (advice-add 'select-frame-set-input-focus :after #'waymacs--after-select-frame)
    (let ((waymacs--making-background-frame t))
      (mapc #'waymacs--tag-frame (frame-list)))
    (add-to-list 'global-mode-string waymacs--mode-line-construct t)
    (waymacs--sync t)
    (run-hooks 'waymacs-init-hook))))

;;;###autoload
(defun waymacs-enable ()
  "Make Emacs the window manager of the waymacs session it runs in.
Call this from your init file; it does nothing outside waymacs."
  (waymacs--take-socket)
  (if after-init-time
      (waymacs--init)
    (add-hook 'emacs-startup-hook #'waymacs--init)))

(defun waymacs-disable ()
  "Stop managing windows and disconnect from the compositor."
  (interactive)
  (remove-hook 'emacs-startup-hook #'waymacs--init)
  (remove-hook 'post-command-hook #'waymacs--post-command)
  (remove-hook 'window-state-change-hook #'waymacs--sync)
  (remove-hook 'minibuffer-setup-hook #'waymacs--sync)
  (remove-hook 'after-make-frame-functions #'waymacs--tag-frame)
  (advice-remove 'select-frame-set-input-focus #'waymacs--after-select-frame)
  (setq global-mode-string (delete waymacs--mode-line-construct global-mode-string))
  (when (waymacs--connected-p)
    (delete-process waymacs--process))
  (setq waymacs--process nil))

(provide 'waymacs)
;;; waymacs.el ends here
