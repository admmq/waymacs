;;; init.el --- minimal configuration for trying waymacs  -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Adam Quandour
;; SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)

;; Used by test/run-headless.sh and test/run-nested.sh, which load
;; waymacs on the command line as a session would:
;;   emacs -Q -l test/init.el -l lisp/waymacs.el -f waymacs-enable
;; It starts a server named $WAYMACS_TEST_SERVER (default "waymacs-test")
;; so the session can be inspected with
;;   emacsclient -s waymacs-test -e '(...)'

(setq inhibit-startup-screen t)
(menu-bar-mode -1)
(tool-bar-mode -1)
(scroll-bar-mode -1)
(require 'server)
(setq server-name (or (getenv "WAYMACS_TEST_SERVER") "waymacs-test"))
(server-start)
;; settings for a test, made before waymacs is loaded
(when-let* ((setup (getenv "WAYMACS_TEST_SETUP")))
  (eval (car (read-from-string setup)) t))
