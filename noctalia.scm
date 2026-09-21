;;; Ping @rakino in case of dependency issue.
;;;
;;; This Git repository is available as a Guix channel
;;; https://guix.gnu.org/manual/devel/en/html_node/Channels.html
;;;
;;; --8<---------------cut here---------------start------------->8---
;;; (channel
;;;   (name 'noctalia)
;;;   (url "https://github.com/noctalia-dev/noctalia")
;;;   (branch "main"))
;;; --8<---------------cut here---------------end--------------->8---
;;;
;;; It provides this (noctalia) module with the noctalia-git package.

(define-module (noctalia)
  ;; Utilities
  #:use-module (guix channels)
  #:use-module (guix describe)
  #:use-module (guix gexp)
  #:use-module ((guix licenses) #:prefix license:)
  #:use-module (guix packages)
  #:use-module (guix utils)
  #:use-module (ice-9 popen)
  #:use-module (ice-9 rdelim)
  #:use-module (ice-9 regex)
  #:use-module (srfi srfi-1)
  ;; Guix origin methods
  #:use-module (guix git-download)
  ;; Guix build systems
  #:use-module (guix build-system meson)
  ;; Guix packages
  #:use-module (gnu packages calendar)
  #:use-module (gnu packages cpp)
  #:use-module (gnu packages crypto)
  #:use-module (gnu packages curl)
  #:use-module (gnu packages fontutils)
  #:use-module (gnu packages freedesktop)
  #:use-module (gnu packages gl)
  #:use-module (gnu packages glib)
  #:use-module (gnu packages gnome)
  #:use-module (gnu packages gtk)
  #:use-module (gnu packages image)
  #:use-module (gnu packages jemalloc)
  #:use-module (gnu packages linux)
  #:use-module (gnu packages markup)
  #:use-module (gnu packages maths)
  #:use-module (gnu packages multiprecision)
  #:use-module (gnu packages pkg-config)
  #:use-module (gnu packages polkit)
  #:use-module (gnu packages pulseaudio)
  #:use-module (gnu packages stb)
  #:use-module (gnu packages xdisorg)
  #:use-module (gnu packages xml))

(define wayland-protocols-1.48
  (package
    (inherit wayland-protocols)
    (name "wayland-protocols")
    (version "1.48")
    (source (origin
              (method git-fetch)
              (uri (git-reference
                     (url "https://gitlab.freedesktop.org/wayland/wayland-protocols")
                     (commit version)))
              (file-name (git-file-name name version))
              (sha256
               (base32
                "0zqnn7bwqzifchjhclrrcqnp39cpd3nnf6nbd9bav2hwhcx92mwy"))))))

(define source-checkout
  (local-file "." "noctalia-checkout"
              #:recursive? #t
              #:select?
              (or (git-predicate (current-source-directory))
                  (const #t))))

;; Version derived from the checkout
;; git describe --tags --always --dirty=-dirty --abbrev=12
(define (git-output . args)
  (let ((dir (current-source-directory)))
    (if dir
        (catch #t
          (lambda ()
            (let* ((port (apply open-pipe* OPEN_READ "git" "-C" dir args))
                   (out (read-line port)))
              (close-pipe port)
              (if (eof-object? out) "unknown" out)))
          (lambda _ "unknown"))
        "unknown")))

(define %meson-version
  (let ((dir (current-source-directory)))
    (or (and dir
             (catch #t
               (lambda ()
                 (call-with-input-file (string-append dir "/meson.build")
                   (lambda (port)
                     (let loop ((line (read-line port)))
                       (cond
                        ((eof-object? line) #f)
                        ((string-match "^  version: '([^']*)'," line)
                         => (lambda (m) (match:substring m 1)))
                        (else (loop (read-line port))))))))
               (lambda _ #f)))
        "5.0.0")))

;; Full commit: the channel commit when used through `guix pull', otherwise
;; the HEAD of the local checkout.
(define %commit
  (or (let ((c (find (lambda (c) (eq? (channel-name c) 'noctalia))
                     (current-channels))))
        (and c (channel-commit c)))
      (let ((h (git-output "rev-parse" "HEAD")))
        (and (not (string=? h "unknown")) h))
      "unknown"))

(define (short-commit n)
  (if (>= (string-length %commit) n)
      (string-take %commit n)
      %commit))

;; Same value the upstream `vcs_tag' computes.  Without a .git directory
;; (channel use) there are no tags to describe, so fall back to
;; v<version>-g<hash>.
(define %git-describe
  (let ((d (git-output "describe" "--tags" "--always" "--dirty=-dirty"
                       "--abbrev=12")))
    (if (string=? d "unknown")
        (string-append "v" %meson-version "-g" (short-commit 12))
        d)))

(define %pkgver
  (string-append %meson-version
                 "-r" (let ((n (git-output "rev-list" "--count" "HEAD")))
                        (if (string=? n "unknown") "0" n))
                 ".g" (short-commit 9)))

(define-public noctalia-git
  (package
    (name "noctalia-git")
    (version %pkgver)
    (source source-checkout)
    (build-system meson-build-system)
    (arguments
     (list #:build-type "release"
           #:phases
           #~(modify-phases %standard-phases
               (add-after 'unpack 'prepare-for-build
                 (lambda _
                   ;; /bin/sh doesn't exist in the build environment.
                   (substitute* "tests/process_test.cpp"
                     (("/bin/(sh)" _ cmd)
                      (which cmd)))
                   ;; Adjust import paths for STB headers packaged in Guix.
                   (substitute* (find-files "." "\\.cpp$|^meson\\.build$")
                     (("\\bstb/stb_") "stb_"))))
               (add-after 'prepare-for-build 'inject-git-revision
                 (lambda _
                   (use-modules (ice-9 textual-ports))
                   (let ((before (call-with-input-file "meson.build" get-string-all)))
                     (substitute* "meson.build"
                       (("fallback: 'unknown'")
                        (string-append "fallback: '" #$%git-describe "'"))
                       (("set\\('VCS_TAG', 'unknown'\\)")
                        (string-append "set('VCS_TAG', '" #$%git-describe "')")))
                     (when (string=? before
                                     (call-with-input-file "meson.build" get-string-all))
                       (error "VCS_TAG fallback not found in meson.build"))))))))
    (native-inputs
     (list pkg-config))
    (inputs
     (list cairo
           curl
           fontconfig
           freetype
           glib
           gmp
           harfbuzz
           jemalloc
           mpfr
           (librsvg-for-system)
           libjxl
           libical
           libqalculate
           libsecret
           libsndfile
           libsodium
           libwebp
           libxkbcommon
           libxml2
           linux-pam
           md4c
           mesa
           nlohmann-json
           pango
           pipewire
           polkit
           sdbus-c++
           stb-image-resize2
           stb-image-write
           tomlplusplus
           wayland
           wayland-protocols-1.48
           wireplumber))
    (home-page "https://noctalia.dev/")
    (synopsis "Wayland shell and bar")
    (description
     "Noctalia is a lightweight Wayland shell and bar built directly on
Wayland and OpenGL ES, with no Qt or GTK dependency.")
    (license license:expat)))

;; Also return the package at the end, so that this file can be used by
;; commands that evaluate it.  For example:
;;
;; guix build --file=noctalia.scm
;; guix shell --file=noctalia.scm
;; guix package --install-from-file=noctalia.scm
noctalia-git
