;;; noctalia-theme.el --- Theme using Template SCSS variables -*- lexical-binding: t -*-

;; Copyright (C) 2025

;; Author: Generated (Improved)
;; Version: 1.2
;; Package-Requires: ((emacs "24.1"))
;; Keywords: faces

;;; Commentary:

;; A theme using Template SCSS variables with quality of life improvements:
;; - Better source block distinction
;; - Improved text visibility when selected
;; - Refined org-mode styling with hidden asterisks
;; - Enhanced contrast and readability
;; - Seamless integration of source blocks with consistent styling

;;; Code:

(deftheme noctalia "Theme using Template variables with quality of life improvements.")

;; Define all the color variables (replaced by template processor)
(let* ((bg "{{colors.background.default.hex}}")
      (err "{{colors.error.default.hex}}")
      (err-container "{{colors.error_container.default.hex}}")
      (hover "{{colors.hover.default.hex}}")
      (on-background "{{colors.on_background.default.hex}}")
      (on-err "{{colors.on_error.default.hex}}")
      (on-err-container "{{colors.on_error_container.default.hex}}")
      (on-hover "{{colors.on_hover.default.hex}}")
      (on-primary "{{colors.on_primary.default.hex}}")
      (on-primary-container "{{colors.on_primary_container.default.hex}}")
      (on-secondary "{{colors.on_secondary.default.hex}}")
      (on-secondary-container "{{colors.on_secondary_container.default.hex}}")
      (on-surface "{{colors.on_surface.default.hex}}")
      (on-surface-variant "{{colors.on_surface_variant.default.hex}}")
      (on-tertiary "{{colors.on_tertiary.default.hex}}")
      (on-tertiary-container "{{colors.on_tertiary_container.default.hex}}")
      (outline-color "{{colors.outline.default.hex}}")
      (outline-variant "{{colors.outline_variant.default.hex}}")
      (primary "{{colors.primary.default.hex}}")
      (primary-container "{{colors.primary_container.default.hex}}")
      (secondary "{{colors.secondary.default.hex}}")
      (secondary-container "{{colors.secondary_container.default.hex}}")
      (shadow "{{colors.shadow.default.hex}}")
      (surface "{{colors.surface.default.hex}}")
      (surface-container "{{colors.surface_container.default.hex}}")
      (surface-container-high "{{colors.surface_container_high.default.hex}}")
      (surface-container-highest "{{colors.surface_container_highest.default.hex}}")
      (surface-container-low "{{colors.surface_container_low.default.hex}}")
      (surface-container-lowest "{{colors.surface_container_lowest.default.hex}}")
      (surface-variant "{{colors.surface_variant.default.hex}}")
      (tertiary "{{colors.tertiary.default.hex}}")
      (tertiary-container "{{colors.tertiary_container.default.hex}}")
      ;; Map success colors to tertiary (as used in other templates)
      (success "{{colors.tertiary.default.hex}}")
      (on-success "{{colors.on_tertiary.default.hex}}")
      (success-container "{{colors.tertiary_container.default.hex}}")
      (on-success-container "{{colors.on_tertiary_container.default.hex}}")
      ;; Map fixed colors to regular colors
      (primary-fixed "{{colors.primary.default.hex}}")
      (primary-fixed-dim "{{colors.primary_container.default.hex}}")
      (secondary-fixed "{{colors.secondary.default.hex}}")
      (secondary-fixed-dim "{{colors.secondary_container.default.hex}}")
      (tertiary-fixed "{{colors.tertiary.default.hex}}")
      (tertiary-fixed-dim "{{colors.tertiary_container.default.hex}}")
      (on-primary-fixed "{{colors.on_primary.default.hex}}")
      (on-primary-fixed-variant "{{colors.on_primary_container.default.hex}}")
      (on-secondary-fixed "{{colors.on_secondary.default.hex}}")
      (on-secondary-fixed-variant "{{colors.on_secondary_container.default.hex}}")
      (on-tertiary-fixed "{{colors.on_tertiary.default.hex}}")
      (on-tertiary-fixed-variant "{{colors.on_tertiary_container.default.hex}}")
      ;; Map inverse colors to surface variants
      (inverse-on-surface "{{colors.on_surface.default.hex}}")
      (inverse-primary "{{colors.primary.default.hex}}")
      (inverse-surface "{{colors.surface.default.hex}}")
      ;; terminal colors
      (terminal-foreground "{{colors.terminal_foreground.default.hex}}")
      (terminal-background "{{colors.terminal_background.default.hex}}")
      (terminal-cursor "{{colors.terminal_cursor.default.hex}}")
      (terminal-cursor-text "{{colors.terminal_cursor_text.default.hex}}")
      ;; ANSI terminal colors
      (term0 "{{colors.terminal_normal_black.default.hex}}")
      (term1 "{{colors.terminal_normal_red.default.hex}}")
      (term2 "{{colors.terminal_normal_green.default.hex}}")
      (term3 "{{colors.terminal_normal_yellow.default.hex}}")
      (term4 "{{colors.terminal_normal_blue.default.hex}}")
      (term5 "{{colors.terminal_normal_magenta.default.hex}}")
      (term6 "{{colors.terminal_normal_cyan.default.hex}}")
      (term7 "{{colors.terminal_normal_white.default.hex}}")
      (term8 "{{colors.terminal_bright_black.default.hex}}")
      (term9 "{{colors.terminal_bright_red.default.hex}}")
      (term10 "{{colors.terminal_bright_green.default.hex}}")
      (term11 "{{colors.terminal_bright_yellow.default.hex}}")
      (term12 "{{colors.terminal_bright_blue.default.hex}}")
      (term13 "{{colors.terminal_bright_magenta.default.hex}}")
      (term14 "{{colors.terminal_bright_cyan.default.hex}}")
      (term15 "{{colors.terminal_bright_white.default.hex}}"))

  (custom-theme-set-faces
   'noctalia
   ;; Basic faces
   `(default ((t (:background ,bg :foreground ,on-background))))
   `(cursor ((t (:background ,primary))))
   `(highlight ((t (:background ,surface-container-high))))
   `(region ((t (:background ,primary-container :foreground ,on-primary-container :extend t))))
   `(secondary-selection ((t (:background ,secondary-container :foreground ,on-secondary-container :extend t))))
   `(isearch ((t (:background ,tertiary-container :foreground ,on-tertiary-container :weight bold))))
   `(lazy-highlight ((t (:background ,secondary-container :foreground ,on-secondary-container))))
   `(vertical-border ((t (:foreground ,surface-variant))))
   `(border ((t (:background ,surface-variant :foreground ,surface-variant))))
   `(fringe ((t (:background ,surface :foreground ,outline-variant))))
   `(shadow ((t (:foreground ,outline-variant))))
   `(link ((t (:foreground ,primary :underline t))))
   `(link-visited ((t (:foreground ,tertiary :underline t))))
   `(success ((t (:foreground ,success))))
   `(warning ((t (:foreground ,secondary))))
   `(error ((t (:foreground ,err))))
   `(match ((t (:background ,secondary-container :foreground ,on-secondary-container))))

   ;; Font-lock
   `(font-lock-builtin-face ((t (:foreground ,primary))))
   `(font-lock-comment-face ((t (:foreground ,outline-color :slant italic))))
   `(font-lock-comment-delimiter-face ((t (:foreground ,outline-variant))))
   `(font-lock-constant-face ((t (:foreground ,tertiary :weight bold))))
   `(font-lock-doc-face ((t (:foreground ,on-surface-variant :slant italic))))
   `(font-lock-function-name-face ((t (:foreground ,primary :weight bold))))
   `(font-lock-keyword-face ((t (:foreground ,secondary :weight bold))))
   `(font-lock-string-face ((t (:foreground ,tertiary))))
   `(font-lock-type-face ((t (:foreground ,primary-fixed))))
   `(font-lock-variable-name-face ((t (:foreground ,on-surface))))
   `(font-lock-warning-face ((t (:foreground ,err :weight bold))))
   `(font-lock-preprocessor-face ((t (:foreground ,secondary-fixed-dim))))
   `(font-lock-negation-char-face ((t (:foreground ,tertiary-fixed))))

   ;; Show paren
   `(show-paren-match ((t (:background ,primary-container :foreground ,on-primary-container :weight bold))))
   `(show-paren-mismatch ((t (:background ,err-container :foreground ,on-err-container :weight bold))))

   ;; Mode line - improved status bar styling
   `(mode-line ((t (:background ,surface-container-high :foreground ,on-surface :box nil))))
   `(mode-line-inactive ((t (:background ,surface :foreground ,on-surface-variant :box nil))))
   `(mode-line-buffer-id ((t (:foreground ,primary :weight bold))))
   `(mode-line-emphasis ((t (:foreground ,primary :weight bold))))
   `(mode-line-highlight ((t (:foreground ,primary :box nil))))

   ;; Improved Source blocks - make them integrated with the theme
   `(org-block ((t (:background ,surface-container-low :extend t :inherit fixed-pitch))))
   `(org-block-begin-line ((t (:background ,surface-container-low :foreground ,primary-fixed-dim :extend t :slant italic :inherit fixed-pitch))))
   `(org-block-end-line ((t (:background ,surface-container-low :foreground ,primary-fixed-dim :extend t :slant italic :inherit fixed-pitch))))
   `(org-code ((t (:background ,surface-container-low :foreground ,tertiary-fixed :inherit fixed-pitch))))
   `(org-verbatim ((t (:background ,surface-container-low :foreground ,primary-fixed :inherit fixed-pitch))))
   `(org-meta-line ((t (:foreground ,outline-color :slant italic))))

   ;; Org mode with hidden asterisks
   `(org-level-1 ((t (:foreground ,primary :weight bold :height 1.2))))
   `(org-level-2 ((t (:foreground ,secondary :weight bold :height 1.1))))
   `(org-level-3 ((t (:foreground ,tertiary :weight bold))))
   `(org-level-4 ((t (:foreground ,primary :weight bold))))
   `(org-level-5 ((t (:foreground ,secondary :weight bold))))
   `(org-level-6 ((t (:foreground ,tertiary :weight bold))))
   `(org-level-7 ((t (:foreground ,primary :weight bold))))
   `(org-level-8 ((t (:foreground ,secondary :weight bold))))
   `(org-document-title ((t (:foreground ,primary :weight bold :height 1.3))))
   `(org-document-info ((t (:foreground ,primary-container))))
   `(org-todo ((t (:foreground ,err :weight bold))))
   `(org-done ((t (:foreground ,success :weight bold))))
   `(org-headline-done ((t (:foreground ,on-surface-variant))))
   `(org-hide ((t (:foreground ,bg)))) ;; Hide leading asterisks
   `(org-ellipsis ((t (:foreground ,tertiary :underline nil)))) ;; Style for folded content indicator
   `(org-table ((t (:foreground ,secondary-fixed :inherit fixed-pitch))))
   `(org-formula ((t (:foreground ,tertiary :inherit fixed-pitch))))
   `(org-checkbox ((t (:foreground ,primary :weight bold :inherit fixed-pitch))))
   `(org-date ((t (:foreground ,secondary-fixed :underline t))))
   `(org-special-keyword ((t (:foreground ,on-surface-variant :slant italic))))
   `(org-tag ((t (:foreground ,outline-color :weight normal))))

   ;; Magit
   `(magit-section-highlight ((t (:background ,surface-container-low))))
   `(magit-diff-hunk-heading ((t (:background ,surface-container :foreground ,on-surface-variant))))
   `(magit-diff-hunk-heading-highlight ((t (:background ,surface-container-high :foreground ,on-surface))))
   `(magit-diff-context ((t (:foreground ,on-surface-variant))))
   `(magit-diff-context-highlight ((t (:background ,surface-container-low :foreground ,on-surface))))
   `(magit-diff-added ((t (:background ,success-container :foreground ,on-success-container))))
   `(magit-diff-added-highlight ((t (:background ,success-container :foreground ,on-success-container :weight bold))))
   `(magit-diff-removed ((t (:background ,err-container :foreground ,on-err-container))))
   `(magit-diff-removed-highlight ((t (:background ,err-container :foreground ,on-err-container :weight bold))))
   `(magit-hash ((t (:foreground ,outline-color))))
   `(magit-branch-local ((t (:foreground ,tertiary :weight bold))))
   `(magit-branch-remote ((t (:foreground ,primary :weight bold))))

   ;; Company
   `(company-tooltip ((t (:background ,surface-container :foreground ,on-surface))))
   `(company-tooltip-selection ((t (:background ,primary-container :foreground ,on-primary-container))))
   `(company-tooltip-common ((t (:foreground ,primary))))
   `(company-tooltip-common-selection ((t (:foreground ,on-primary-container :weight bold))))
   `(company-tooltip-annotation ((t (:foreground ,tertiary))))
   `(company-scrollbar-fg ((t (:background ,primary))))
   `(company-scrollbar-bg ((t (:background ,surface-variant))))
   `(company-preview ((t (:foreground ,on-surface-variant :slant italic))))
   `(company-preview-common ((t (:foreground ,primary :slant italic))))

   ;; Ido
   `(ido-first-match ((t (:foreground ,primary :weight bold))))
   `(ido-only-match ((t (:foreground ,tertiary :weight bold))))
   `(ido-subdir ((t (:foreground ,secondary))))
   `(ido-indicator ((t (:foreground ,err))))
   `(ido-virtual ((t (:foreground ,outline-color))))

   ;; Helm
   `(helm-selection ((t (:background ,primary-container :foreground ,on-primary-container))))
   `(helm-match ((t (:foreground ,primary :weight bold))))
   `(helm-source-header ((t (:background ,surface-container-high :foreground ,primary :weight bold :height 1.1))))
   `(helm-candidate-number ((t (:foreground ,tertiary :weight bold))))
   `(helm-ff-directory ((t (:foreground ,primary :weight bold))))
   `(helm-ff-file ((t (:foreground ,on-surface))))
   `(helm-ff-executable ((t (:foreground ,tertiary))))

   ;; corfu
   `(corfu-default ((t (:background ,surface-container :foreground ,on-surface))))
   `(corfu-current ((t (:background ,primary-container :foreground ,on-primary-container))))

   ;; Which-key
   `(which-key-key-face ((t (:foreground ,primary :weight bold))))
   `(which-key-separator-face ((t (:foreground ,outline-variant))))
   `(which-key-command-description-face ((t (:foreground ,on-surface))))
   `(which-key-group-description-face ((t (:foreground ,secondary))))
   `(which-key-special-key-face ((t (:foreground ,tertiary :weight bold))))

   ;; Line numbers
   `(line-number ((t (:foreground ,outline-variant :inherit fixed-pitch))))
   `(line-number-current-line ((t (:foreground ,primary :weight bold :inherit fixed-pitch))))

   ;; Parenthesis matching
   `(sp-show-pair-match-face ((t (:background ,primary-container :foreground ,on-primary-container))))
   `(sp-show-pair-mismatch-face ((t (:background ,err-container :foreground ,on-err-container))))

   ;; Rainbow delimiters
   `(rainbow-delimiters-depth-1-face ((t (:foreground ,primary))))
   `(rainbow-delimiters-depth-2-face ((t (:foreground ,secondary))))
   `(rainbow-delimiters-depth-3-face ((t (:foreground ,tertiary))))
   `(rainbow-delimiters-depth-4-face ((t (:foreground ,primary-fixed))))
   `(rainbow-delimiters-depth-5-face ((t (:foreground ,secondary-fixed))))
   `(rainbow-delimiters-depth-6-face ((t (:foreground ,tertiary-fixed))))
   `(rainbow-delimiters-depth-7-face ((t (:foreground ,primary-fixed-dim))))
   `(rainbow-delimiters-depth-8-face ((t (:foreground ,secondary-fixed-dim))))
   `(rainbow-delimiters-depth-9-face ((t (:foreground ,tertiary-fixed-dim))))
   `(rainbow-delimiters-mismatched-face ((t (:foreground ,err :weight bold))))
   `(rainbow-delimiters-unmatched-face ((t (:foreground ,err :weight bold))))

   ;; Dired
   `(dired-directory ((t (:foreground ,primary :weight bold))))
   `(dired-ignored ((t (:foreground ,outline-variant))))
   `(dired-flagged ((t (:foreground ,err))))
   `(dired-marked ((t (:foreground ,tertiary :weight bold))))
   `(dired-symlink ((t (:foreground ,secondary :slant italic))))
   `(dired-header ((t (:foreground ,primary :weight bold :height 1.1))))

   ;; Terminal colors
   `(term-color-black ((t (:foreground ,term0 :background ,term0))))
   `(term-color-red ((t (:foreground ,term1 :background ,term1))))
   `(term-color-green ((t (:foreground ,term2 :background ,term2))))
   `(term-color-yellow ((t (:foreground ,term3 :background ,term3))))
   `(term-color-blue ((t (:foreground ,term4 :background ,term4))))
   `(term-color-magenta ((t (:foreground ,term5 :background ,term5))))
   `(term-color-cyan ((t (:foreground ,term6 :background ,term6))))
   `(term-color-white ((t (:foreground ,term7 :background ,term7))))

   ;; ANSI terminal colors
   `(ansi-color-black ((t (:foreground ,term0 :background ,term0))))
   `(ansi-color-red ((t (:foreground ,term1 :background ,term1))))
   `(ansi-color-green ((t (:foreground ,term2 :background ,term2))))
   `(ansi-color-yellow ((t (:foreground ,term3 :background ,term3))))
   `(ansi-color-blue ((t (:foreground ,term4 :background ,term4))))
   `(ansi-color-magenta ((t (:foreground ,term5 :background ,term5))))
   `(ansi-color-cyan ((t (:foreground ,term6 :background ,term6))))
   `(ansi-color-white ((t (:foreground ,term7 :background ,term7))))
   `(ansi-color-bright-black ((t (:foreground ,term8 :background ,term8))))
   `(ansi-color-bright-red ((t (:foreground ,term9 :background ,term9))))
   `(ansi-color-bright-green ((t (:foreground ,term10 :background ,term10))))
   `(ansi-color-bright-yellow ((t (:foreground ,term11 :background ,term11))))
   `(ansi-color-bright-blue ((t (:foreground ,term12 :background ,term12))))
   `(ansi-color-bright-magenta ((t (:foreground ,term13 :background ,term13))))
   `(ansi-color-bright-cyan ((t (:foreground ,term14 :background ,term14))))
   `(ansi-color-bright-white ((t (:foreground ,term15 :background ,term15))))

   ;; EShell
   `(eshell-prompt ((t (:foreground ,primary :weight bold))))
   `(eshell-ls-directory ((t (:foreground ,primary :weight bold))))
   `(eshell-ls-symlink ((t (:foreground ,secondary :slant italic))))
   `(eshell-ls-executable ((t (:foreground ,tertiary))))
   `(eshell-ls-archive ((t (:foreground ,on-tertiary-container))))
   `(eshell-ls-backup ((t (:foreground ,outline-variant))))
   `(eshell-ls-clutter ((t (:foreground ,err))))
   `(eshell-ls-missing ((t (:foreground ,err))))
   `(eshell-ls-product ((t (:foreground ,on-surface-variant))))
   `(eshell-ls-readonly ((t (:foreground ,on-surface-variant))))
   `(eshell-ls-special ((t (:foreground ,secondary-fixed))))
   `(eshell-ls-unreadable ((t (:foreground ,outline-variant))))

   ;; Improved markdown mode
   `(markdown-header-face ((t (:foreground ,primary :weight bold))))
   `(markdown-header-face-1 ((t (:foreground ,primary :weight bold :height 1.2))))
   `(markdown-header-face-2 ((t (:foreground ,primary-container :weight bold :height 1.1))))
   `(markdown-header-face-3 ((t (:foreground ,secondary :weight bold))))
   `(markdown-header-face-4 ((t (:foreground ,secondary-container :weight bold))))
   `(markdown-inline-code-face ((t (:foreground ,tertiary-fixed :background ,surface-container-low :inherit fixed-pitch))))
   `(markdown-code-face ((t (:background ,surface-container-low :extend t :inherit fixed-pitch))))
   `(markdown-pre-face ((t (:background ,surface-container-low :inherit fixed-pitch))))
   `(markdown-table-face ((t (:foreground ,secondary-fixed :inherit fixed-pitch))))

   ;; Web mode
   `(web-mode-html-tag-face ((t (:foreground ,primary))))
   `(web-mode-html-tag-bracket-face ((t (:foreground ,on-surface-variant))))
   `(web-mode-html-attr-name-face ((t (:foreground ,secondary))))
   `(web-mode-html-attr-value-face ((t (:foreground ,tertiary))))
   `(web-mode-css-selector-face ((t (:foreground ,primary))))
   `(web-mode-css-property-name-face ((t (:foreground ,secondary))))
   `(web-mode-css-string-face ((t (:foreground ,tertiary))))

   ;; Flycheck
   `(flycheck-error ((t (:underline (:style wave :color ,err)))))
   `(flycheck-warning ((t (:underline (:style wave :color ,secondary)))))
   `(flycheck-info ((t (:underline (:style wave :color ,tertiary)))))
   `(flycheck-fringe-error ((t (:foreground ,err))))
   `(flycheck-fringe-warning ((t (:foreground ,secondary))))
   `(flycheck-fringe-info ((t (:foreground ,tertiary))))

   ;; Mini-buffer customization
   `(minibuffer-prompt ((t (:foreground ,primary :weight bold))))

   ;; Improved search highlighting
   `(lsp-face-highlight-textual ((t (:background ,primary-container :foreground ,on-primary-container :weight bold))))
   `(lsp-face-highlight-read ((t (:background ,secondary-container :foreground ,on-secondary-container :weight bold))))
   `(lsp-face-highlight-write ((t (:background ,tertiary-container :foreground ,on-tertiary-container :weight bold))))

   ;; Info and help modes
   `(info-title-1 ((t (:foreground ,primary :weight bold :height 1.3))))
   `(info-title-2 ((t (:foreground ,primary-container :weight bold :height 1.2))))
   `(info-title-3 ((t (:foreground ,secondary :weight bold :height 1.1))))
   `(info-title-4 ((t (:foreground ,secondary-container :weight bold))))
   `(Info-quoted ((t (:foreground ,tertiary))))
   `(info-menu-header ((t (:foreground ,primary :weight bold))))
   `(info-menu-star ((t (:foreground ,primary))))
   `(info-node ((t (:foreground ,tertiary :weight bold))))

   ;; Tabs
   `(tab-bar ((t (:background ,surface-container-high :foreground ,on-surface :box nil))))
   `(tab-bar-tab ((t (:background ,surface-container-high :foreground ,on-surface :weight bold :box nil))))
   `(tab-bar-tab-inactive ((t (:background ,surface :foreground ,on-surface-variant :box nil))))

   `(tab-line ((t (:background ,surface-container-high :foreground ,on-surface :box nil))))
   `(tab-line-tab ((t (:background ,surface :foreground ,on-surface-variant :box nil))))
   `(tab-line-tab-current ((t (:background ,surface-container-high :foreground ,on-surface :weight bold :box nil))))
   `(tab-line-tab-inactive ((t (:background ,surface :foreground ,on-surface-variant :box nil))))
   `(tab-line-highlight ((t (:background ,surface-container-highest :foreground ,on-surface))))

   `(centaur-tabs-default ((t (:background ,surface-container-high :foreground ,on-surface))))
   `(centaur-tabs-selected ((t (:background ,surface-container-high :foreground ,on-surface :weight bold))))
   `(centaur-tabs-unselected ((t (:background ,surface :foreground ,on-surface-variant))))
   `(centaur-tabs-selected-modified ((t (:background ,surface-container-high :foreground ,tertiary :weight bold))))
   `(centaur-tabs-unselected-modified ((t (:background ,surface :foreground ,tertiary))))
   `(centaur-tabs-active-bar-face ((t (:background ,primary))))

   ;; message
   `(message-header-to ((t (:foreground ,primary))))
   `(message-header-cc ((t (:foreground ,secondary))))
   `(message-header-subject ((t (:foreground ,tertiary))))
   `(message-header-newsgroups ((t (:foreground ,tertiary))))
   `(message-header-other ((t (:foreground ,terminal-foreground))))
   `(message-header-name ((t (:foreground ,primary))))
   `(message-header-xheader ((t (:foreground ,tertiary))))
   `(message-separator ((t (:foreground ,on-surface-variant))))
   `(message-cited-text-1 ((t (:foreground ,primary))))
   `(message-cited-text-2 ((t (:foreground ,secondary))))
   `(message-cited-text-3 ((t (:foreground ,tertiary))))
   `(message-cited-text-4 ((t (:foreground ,err))))
   `(message-mml ((t (:foreground ,secondary))))
   `(message-signature-separator ((t (:foreground ,on-surface-variant))))

   ;; Fixed-pitch faces
   `(fixed-pitch ((t (:family "monospace"))))
   `(fixed-pitch-serif ((t (:family "monospace serif"))))

   ;; Variable-pitch face
   `(variable-pitch ((t (:family "sans serif"))))
   ))

;; Add org-mode hooks for hiding leading stars
(with-eval-after-load 'org
  (setq org-hide-leading-stars t)
  (setq org-startup-indented t))

;;;###autoload
(when load-file-name
  (add-to-list 'custom-theme-load-path
               (file-name-as-directory (file-name-directory load-file-name))))

(provide-theme 'noctalia)
;;; noctalia-theme.el ends here
