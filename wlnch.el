;;; wlnch.el --- Major mode for editing wlnch config files -*- lexical-binding: t; -*-

;; Author: wlnch contributors
;; Version: 0.1.0
;; Keywords: languages, files, wayland
;; Package-Requires: ((emacs "26.1"))

;; This file is not part of GNU Emacs.

;;; Commentary:

;; A major mode for `wlnch' (a tiny Wayland command launcher) config
;; files.  The file format is one entry per line:
;;
;;     KEY[&|#RRGGBB]:NAME:COMMAND
;;
;; with `#'-prefixed comment lines, an optional `#!' shebang on the
;; first line, and `---' lines acting as visual separators.
;;
;; Highlighting:
;;
;;   - `#'-prefixed lines render as comments.
;;   - The first line, if it starts with `#!', renders as a shebang.
;;   - Inline `#RRGGBB' modifiers render with the hex value as their
;;     own background colour, the same way `css-mode' previews colours.
;;   - The COMMAND portion of each entry is fontified as Bash by
;;     piggy-backing on `sh-mode'.
;;
;; Installation:
;;
;;   (require 'wlnch)
;;
;; Files named `wlnchrc', `.wlnchrc' or `wlnchrc.<anything>' are
;; auto-detected, as are files whose shebang interpreter is `wlnch'.

;;; Code:

(require 'sh-script)

(defgroup wlnch nil
  "Major mode for editing wlnch config files."
  :group 'languages
  :prefix "wlnch-")

;;;; Faces ----------------------------------------------------------------

(defface wlnch-shebang-face
  '((t :inherit font-lock-preprocessor-face))
  "Face for the `#!' shebang line."
  :group 'wlnch)

(defface wlnch-key-face
  '((t :inherit font-lock-keyword-face :weight bold))
  "Face for the KEY character at the start of an entry."
  :group 'wlnch)

(defface wlnch-sticky-face
  '((t :inherit font-lock-warning-face :weight bold))
  "Face for the sticky modifier `&' on an entry."
  :group 'wlnch)

(defface wlnch-name-face
  '((t :inherit font-lock-variable-name-face))
  "Face for the NAME field of an entry."
  :group 'wlnch)

(defface wlnch-separator-face
  '((t :inherit font-lock-comment-face :weight bold))
  "Face for the `---' visual separator."
  :group 'wlnch)

(defface wlnch-field-separator-face
  '((t :inherit shadow))
  "Face for the `:' field separators inside an entry."
  :group 'wlnch)

;;;; Regexps --------------------------------------------------------------

(defconst wlnch--entry-regexp
  (concat
   "^\\([^#:\n \t]\\)"                          ; 1: KEY
   "\\(&\\|#[0-9a-fA-F]\\{6\\}\\)?"             ; 2: optional MOD (& or #RRGGBB)
   "\\(:\\)"                                    ; 3: first ':'
   "\\([^:\n]*\\)"                              ; 4: NAME
   "\\(:\\)"                                    ; 5: second ':'
   "\\(.*\\)$")                                 ; 6: COMMAND
  "Regexp matching one wlnch entry line.
Subgroups: 1 KEY, 2 MOD (optional), 3 `:', 4 NAME, 5 `:', 6 COMMAND.")

(defconst wlnch--separator-regexp
  "^[ \t]*\\(---\\)[ \t]*$"
  "Regexp matching a `---' visual-separator line.")

;;;; #RRGGBB inline preview ----------------------------------------------

(defun wlnch--readable-fg (hex)
  "Return \"black\" or \"white\", whichever contrasts better with HEX.
HEX must be a string of the form \"#RRGGBB\".  Uses the Rec.601
luma coefficients."
  (let* ((r (/ (string-to-number (substring hex 1 3) 16) 255.0))
         (g (/ (string-to-number (substring hex 3 5) 16) 255.0))
         (b (/ (string-to-number (substring hex 5 7) 16) 255.0))
         (luma (+ (* 0.299 r) (* 0.587 g) (* 0.114 b))))
    (if (> luma 0.5) "black" "white")))

(defun wlnch--hex-color-face (hex)
  "Return a face spec that previews HEX inline (background + readable fg)."
  (list :background hex
        :foreground (wlnch--readable-fg hex)
        :weight 'bold))

(defun wlnch--mod-face ()
  "Return the face spec for the modifier captured by `wlnch--entry-regexp'.
Returns nil if there is no modifier, `wlnch-sticky-face' for `&',
or a hex-preview face for `#RRGGBB'."
  (let ((m (match-string-no-properties 2)))
    (cond
     ((null m) nil)
     ((string= m "&") 'wlnch-sticky-face)
     (t (wlnch--hex-color-face m)))))

;;;; Embedded sh-mode fontification --------------------------------------
;;
;; Standard "fontify some text as another language" trick: keep a
;; hidden buffer in `sh-mode' (with bash explicitly selected), copy
;; the text in, run `font-lock-ensure', and copy the resulting `face'
;; properties back into our buffer at the right offsets.

(defvar wlnch--sh-fontify-buffer nil
  "Hidden buffer used to fontify embedded shell commands.")

(defun wlnch--sh-fontify-buffer ()
  "Return the live hidden sh-mode buffer used for command fontification."
  (if (buffer-live-p wlnch--sh-fontify-buffer)
      wlnch--sh-fontify-buffer
    (setq wlnch--sh-fontify-buffer
          (with-current-buffer (get-buffer-create " *wlnch-sh-fontify*")
            (delay-mode-hooks (sh-mode))
            (sh-set-shell "bash" nil nil)
            (current-buffer)))))

(defun wlnch--fontify-as-sh (beg end)
  "Apply sh-mode `face' properties to the region [BEG, END) in current buffer."
  (when (and (integerp beg) (integerp end) (< beg end))
    (let ((text   (buffer-substring-no-properties beg end))
          (sh-buf (wlnch--sh-fontify-buffer)))
      (with-current-buffer sh-buf
        (let ((inhibit-read-only t))
          (erase-buffer)
          (insert text)
          (font-lock-ensure)))
      (with-silent-modifications
        (let ((sh-pos 1)
              (sh-end (1+ (- end beg))))
          (while (< sh-pos sh-end)
            (let* ((face    (with-current-buffer sh-buf
                              (get-text-property sh-pos 'face)))
                   (sh-next (with-current-buffer sh-buf
                              (next-single-property-change
                               sh-pos 'face nil sh-end))))
              (when face
                (put-text-property
                 (+ beg (1- sh-pos))
                 (+ beg (1- sh-next))
                 'face face))
              (setq sh-pos sh-next))))))))

(defun wlnch--fontify-command-matcher (limit)
  "Font-lock matcher: locate an entry's COMMAND and fontify it as Bash.
The matcher returns non-nil so font-lock keeps walking the region
until LIMIT, but installs no highlighters of its own — the face
properties are copied in by `wlnch--fontify-as-sh' as a side effect."
  (when (re-search-forward wlnch--entry-regexp limit t)
    (wlnch--fontify-as-sh (match-beginning 6) (match-end 6))
    t))

;;;; Font-lock keywords --------------------------------------------------

(defvar wlnch-font-lock-keywords
  `(;; Shebang: only at the very start of the buffer.  `\\`' anchors
    ;; the match to point-min, so this only ever fires on line 1.
    ("\\`#!.*$" . 'wlnch-shebang-face)

    ;; Comments: any line starting with `#' that isn't already the
    ;; shebang.  OVERRIDE=nil means the shebang face above wins on
    ;; line 1, while every other `#'-line is rendered as a comment.
    ("^#.*$" . 'font-lock-comment-face)

    ;; The `---' visual separator.
    (,wlnch--separator-regexp 1 'wlnch-separator-face)

    ;; Entry line: KEY, optional modifier, the two `:' separators
    ;; and NAME.  COMMAND is handled by the matcher below.
    (,wlnch--entry-regexp
     (1 'wlnch-key-face)
     (2 (wlnch--mod-face) nil t)
     (3 'wlnch-field-separator-face)
     (4 'wlnch-name-face)
     (5 'wlnch-field-separator-face))

    ;; COMMAND fontified as Bash via the embedded sh-mode buffer.
    ;; This is a side-effect-only matcher — no highlighters.
    (wlnch--fontify-command-matcher))
  "Font-lock keywords for `wlnch-mode'.")

;;;; Syntax table --------------------------------------------------------
;;
;; We deliberately do *not* mark `#' as a comment-start character:
;; that would also turn `#RRGGBB' inside an entry into a comment.
;; Instead, comments are recognised by the `^#.*$' font-lock keyword,
;; and `M-;' (`comment-dwim') still works because `comment-start' is
;; set in the mode body.

(defvar wlnch-mode-syntax-table
  (let ((st (make-syntax-table)))
    (modify-syntax-entry ?_ "w" st)
    (modify-syntax-entry ?- "w" st)
    st)
  "Syntax table for `wlnch-mode'.")

;;;; Mode definition -----------------------------------------------------

;;;###autoload
(define-derived-mode wlnch-mode prog-mode "wlnch"
  "Major mode for editing wlnch config files.

\\{wlnch-mode-map}"
  :syntax-table wlnch-mode-syntax-table
  (setq-local comment-start "# ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "#+[ \t]*")
  (setq-local font-lock-defaults
              '(wlnch-font-lock-keywords
                t   ; KEYWORDS-ONLY: skip syntactic fontification entirely
                nil ; CASE-FOLD
                nil ; SYNTAX-ALIST
                nil ; SYNTAX-BEGIN
                ))
  (setq-local font-lock-multiline nil))

;;;###autoload
(add-to-list 'auto-mode-alist
             '("/\\.?wlnchrc\\(?:\\.[^/]*\\)?\\'" . wlnch-mode))

;;;###autoload
(add-to-list 'interpreter-mode-alist '("wlnch" . wlnch-mode))

(provide 'wlnch)

;;; wlnch.el ends here
