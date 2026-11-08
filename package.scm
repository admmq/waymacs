(use-modules ((guix licenses) #:prefix license:)
             (gnu packages)
             (gnu packages emacs)
             (gnu packages freedesktop)
             (gnu packages pkg-config)
             (gnu packages window-management)
             (gnu packages xorg)
             (guix packages)
             (guix gexp)
             (guix utils)
             (guix git-download)
             (guix build-system gnu))

(let ((commit "d5b30e19e9bd50e31b431d82a2fc9157e439a262")
      (revision "0"))
  (package
    (name "waymacs")
    (version (git-version "0.1" revision commit))
    (source (origin
              (method git-fetch)
              (uri (git-reference
                    (url "https://github.com/admmq/waymacs")
                    (commit commit)))
              (file-name (git-file-name name version))
              (sha256
               (base32
                "0i5gx8jqdh701dxj69g8myldrw050h6kg6qzvpfdng19b7gbdz47"))))
    (build-system gnu-build-system)
    (arguments
     (list
      #:modules '((guix build emacs-utils)
                  (guix build gnu-build-system)
                  (guix build utils))
      #:imported-modules `(,@%default-gnu-imported-modules
                           (guix build emacs-utils))
      ;; `make check' byte-compiles waymacs.el with warnings as errors;
      ;; `make test' needs foot, wtype, wlrctl and grim, so it is not run.
      #:make-flags
      #~(list (string-append "CC=" #$(cc-for-target))
              (string-append "CXX=" #$(cxx-for-target))
              (string-append "PREFIX=" #$output)
              (string-append "EMACS=" #$(this-package-input "emacs-pgtk")
                             "/bin/emacs")
              ;; where the session file tells Emacs to load waymacs.el from
              (string-append "LISPDIR=" #$output
                             "/share/emacs/site-lisp/waymacs"))
      #:phases
      #~(modify-phases %standard-phases
          (delete 'configure)
          ;; `make install' leaves waymacs.el in the checkout
          (add-after 'install 'install-elisp
            (lambda _
              (let ((lisp-dir (string-append #$output
                                             "/share/emacs/site-lisp/waymacs")))
                (install-file "lisp/waymacs.el" lisp-dir)
                (emacs-generate-autoloads #$name lisp-dir)
                (emacs-byte-compile-directory lisp-dir)))))))
    (native-inputs
     (list pkg-config wayland))
    (inputs
     (list emacs-pgtk libxcb wlroots-0.19))
    (home-page "https://github.com/admmq/waymacs")
    (synopsis "Emacs as a Wayland window manager")
    (description
     "waymacs makes Emacs the window manager of a Wayland session, in the
spirit of EXWM.  Every application window gets an Emacs buffer, and showing
that buffer in an Emacs window shows the application there, so Emacs window
commands, @code{display-buffer-alist} and tab-bar workspaces manage the
applications.  It consists of @command{waymacs}, a small wlroots compositor,
and @file{waymacs.el}, which makes all window management decisions.  The
package also installs a @file{waymacs.desktop} session file for display
managers.")
    (license license:gpl3+)))
