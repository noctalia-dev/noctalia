#!/usr/bin/env python3
"""
Static i18n checks for noctalia.

  1) Missing keys (always on): every i18n::tr / i18n::trp string literal key must exist
     in assets/translations/en.json. Exit 1 when any are missing.

  2) --unused: catalog entries not referenced anywhere in source. Includes indirect
     usages (.labelKey = "...", titleKey = "..."): any string literal that exactly
     matches a catalog key counts as a reference. Keys reachable only through dynamic
     string construction (e.g. prefix + variable) are reported separately as "possibly
     dynamic". Use --fail-on-unused to exit 1.

  3) --raw: flag likely user-visible string literals in src/ui/ and src/shell/ that are
     not wrapped in i18n::tr. Two tiers:
       (a) setter calls: setText, setPlaceholder, setTitle, … (always included)
       (b) any multi-word "…" literal in real code (add --setters-only to skip this tier)
     Suppress a line with: // check-i18n:ignore-line
     Use --fail-on-raw to exit 1.

  4) --all: enable --unused and --raw together.

Usage:
  python3 tools/i18n-check.py                              # missing keys only (fast)
  python3 tools/i18n-check.py --unused                     # + unused catalog keys
  python3 tools/i18n-check.py --raw                        # + untranslated UI strings
  python3 tools/i18n-check.py --raw --setters-only         # raw, setter calls only
  python3 tools/i18n-check.py --all                        # everything
  python3 tools/i18n-check.py --all --fail-on-raw --fail-on-unused  # strict CI
  python3 tools/i18n-check.py --exclude src/shell/test/test_panel.cpp
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRANSLATIONS = ROOT / "assets" / "translations" / "en.json"
DEFAULT_SRC = ROOT / "src"
DEFAULT_RAW_ROOTS = ("src/ui", "src/shell")

# i18n::tr("a.b.c", ...) or i18n::tr("a.b.c")
TR_RE = re.compile(r'i18n::tr\s*\(\s*"([^"]+)"')
# i18n::trp("a.b.c", count, ...) → also requires "a.b.c-plural" when plural branch exists
TRP_RE = re.compile(r'i18n::trp\s*\(\s*"([^"]+)"')

# Any string literal that looks like a dotted i18n key (for indirect-usage scan)
KEY_LITERAL_RE = re.compile(r'"([a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+(?:-plural)?)"')

# UI setters: first argument is a normal "..." string literal (same-line only).
_UI_METHODS = (
    "setText",
    "setPlaceholder",
    "setTitle",
    "setSubtitle",
    "setButtonText",
    "setEmptyText",
    "setDescription",
    "setHelp",
    "setMessage",
    "setTooltip",
    "setHint",
    "setLabel",
    "setHeader",
    "setFooter",
    "setCaption",
    "setSummary",
    "setDetail",
)
RAW_UI_LITERAL_RE = re.compile(
    rf"(?:->|\.)\s*(?P<meth>{'|'.join(_UI_METHODS)})\s*\(\s*\"(?P<lit>[^\"\\]*)\"",
)

HAS_I18N = re.compile(r"i18n::tr(?:p)?\s*\(")
IGNORE_LINE_MARK = re.compile(r"//\s*check-i18n:ignore-line\b|//\s*i18n:ignore-line\b")
SKIP_LINE_HINT = re.compile(
    r"(assert\s*\(|static_assert\s*\(|\bkLog\w*\.|\buiAssert|\bLogger\b|\bstd::cerr\b|\bspdlog::|\bNOCTALIA_DEBUG\b|\bthrow\b)",
)

# Any "..." literal on a code line (after stripping // comments); used for wide scan
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Token literals that look like words but are data / protocol, not copy
_IGNORE_LITERAL_LOWER = frozenset(
    {
        "true",
        "false",
        "null",
        "none",
        "auto",
        "default",
        "inherit",
        "transparent",
        "check",  # glyph name
        "menu",
        "close",
    }
)

DEFAULT_EXCLUDE_GLOBS = (
    "src/shell/test/test_panel.cpp",  # internal UI lab; lots of intentional English
)


def flatten_json_strings(node: object, prefix: str, out: dict[str, str]) -> None:
    if isinstance(node, dict):
        for k, v in node.items():
            p = f"{prefix}.{k}" if prefix else str(k)
            flatten_json_strings(v, p, out)
    elif isinstance(node, str):
        out[prefix] = node
    elif isinstance(node, (int, float, bool)):
        return
    elif isinstance(node, list):
        for i, item in enumerate(node):
            flatten_json_strings(item, f"{prefix}[{i}]", out)


def iter_source_files(src: Path, extensions: tuple[str, ...], excludes: set[Path]) -> Iterable[Path]:
    for ext in extensions:
        for path in src.rglob(f"*{ext}"):
            try:
                rp = path.resolve()
            except OSError:
                rp = path
            if rp in excludes:
                continue
            yield path


def collect_tr_keys(text: str) -> tuple[set[str], set[str]]:
    """Returns (tr_keys, trp_base_keys)."""
    tr_keys = set(TR_RE.findall(text))
    trp_bases = set(TRP_RE.findall(text))
    return tr_keys, trp_bases


def check_catalog(keys: Iterable[str], catalog: dict[str, str]) -> list[str]:
    missing: list[str] = []
    for k in sorted(keys):
        if k.endswith("."):
            continue
        if k not in catalog:
            missing.append(k)
    return missing


def _ascii_letter_count(s: str) -> int:
    return sum(1 for c in s if ("A" <= c <= "Z" or "a" <= c <= "z"))


def is_placeholder_or_internal_literal(s: str) -> bool:
    """Non–user-copy literals (empty, gauges, paths, noctalia: commands, etc.)."""
    t = s.strip()
    if not t:
        return True
    low = t.lower()
    if low in _IGNORE_LITERAL_LOWER:
        return True
    if low.startswith("noctalia:"):
        return True
    # digits / percent / separators only (e.g. "660", "0%", "100%")
    if re.fullmatch(r"[\d.\s%+-]+", t):
        return True
    # no English letters at all → punctuation, emoji fragments, "-- / --", "…"
    if _ascii_letter_count(t) == 0:
        return True
    # very short alpha codes
    if re.fullmatch(r"[A-Za-z]{1,3}", t):
        return True
    return False


def looks_like_user_visible_copy(s: str) -> bool:
    if is_placeholder_or_internal_literal(s):
        return False
    letters = _ascii_letter_count(s)
    if letters < 4:
        return False
    if " " in s:
        return True
    # single token: long enough to be a word (Apply, System, Cancelled, …)
    if letters >= 7:
        return True
    if letters >= 5 and len(s.strip()) >= 5:
        return True
    return False


def cpp_code_before_line_comment(line: str) -> str:
    """Drop // line comments; only outside double-quoted strings."""
    out: list[str] = []
    i = 0
    n = len(line)
    in_str = False
    esc = False
    while i < n:
        c = line[i]
        if not in_str:
            if c == '"':
                in_str = True
                out.append(c)
            elif c == "/" and i + 1 < n and line[i + 1] == "/":
                break
            else:
                out.append(c)
        else:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        i += 1
    return "".join(out)


def is_wide_noise_literal(s: str) -> bool:
    """Filter paths, URLs, dotted config keys, fmt templates, debug tags, etc. (wide scan only)."""
    t = s.strip()
    low = t.lower()
    if low.startswith("http://") or low.startswith("https://"):
        return True
    if re.fullmatch(r"#[0-9a-fA-F]{3,8}", t):
        return True
    # include / import paths (no spaces)
    if "/" in t and " " not in t and re.fullmatch(r"[\w./+\-]+", t):
        return True
    # dotted i18n-ish keys without spaces
    if t.count(".") >= 2 and " " not in t and re.fullmatch(r"[a-zA-Z0-9_.-]+", t):
        return True
    # fmt / spdlog style templates
    if "{" in t and "}" in t:
        return True
    # C++ / debug context tags (Class::method)
    if "::" in t:
        return True
    # IPC / CLI style machine replies (not translatable UI copy in the same sense)
    if low.startswith("error:") or low.startswith("warning:") or re.fullmatch("ok\n?", low):
        return True
    # hex escapes in format fragments
    if r"\x" in t or r"\u" in t:
        return True
    return False


def looks_like_wide_prose_string(s: str) -> bool:
    """Multi-word (or long) prose in arbitrary string literals; stricter than setter copy."""
    if is_placeholder_or_internal_literal(s) or is_wide_noise_literal(s):
        return False
    letters = _ascii_letter_count(s)
    if letters < 5:
        return False
    t = s.strip()
    if " " in t and len(t) >= 10:
        return True
    # Long single chunk: e.g. technical sentence without many spaces (rare)
    if len(t) >= 28 and letters >= 15:
        return True
    return False


def scan_raw_ui_file(path: Path, text: str, *, wide_strings: bool) -> list[tuple[int, str, str]]:
    """
    Return (line_no, where, literal).
    `where` is a setter name, e.g. setText, or the word literal for wide-scan hits.
    """
    hits: list[tuple[int, str, str]] = []
    seen: set[tuple[int, str]] = set()
    lines = text.splitlines()
    for i, line in enumerate(lines, start=1):
        if IGNORE_LINE_MARK.search(line):
            continue
        if HAS_I18N.search(line):
            continue
        if SKIP_LINE_HINT.search(line):
            continue
        if re.match(r"^\s*#", line):
            continue

        code = cpp_code_before_line_comment(line)
        setter_literals: set[str] = set()

        for m in RAW_UI_LITERAL_RE.finditer(code):
            lit = m.group("lit")
            meth = m.group("meth")
            if not looks_like_user_visible_copy(lit):
                continue
            key = (i, lit)
            if key in seen:
                continue
            seen.add(key)
            setter_literals.add(lit)
            hits.append((i, meth, lit))

        if wide_strings:
            for m in STRING_LITERAL_RE.finditer(code):
                lit = m.group(1)
                if lit in setter_literals:
                    continue
                if not looks_like_wide_prose_string(lit):
                    continue
                key = (i, lit)
                if key in seen:
                    continue
                seen.add(key)
                hits.append((i, "literal", lit))

    return hits


def collect_key_literals(text: str) -> tuple[set[str], set[str]]:
    """
    Returns (exact_keys, key_prefixes).

    exact_keys: string literals that look like complete dotted i18n keys.
    key_prefixes: string literals that look like key prefixes ending with '.'
                  (signals possible dynamic construction, e.g. "foo.bar." + variable).
    """
    exact: set[str] = set()
    prefixes: set[str] = set()
    for m in KEY_LITERAL_RE.finditer(text):
        exact.add(m.group(1))
    # Also capture prefix-style literals like "foo.bar." (ends with dot)
    for m in re.finditer(r'"([a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+\.)"', text):
        prefixes.add(m.group(1))
    return exact, prefixes


def find_unused_keys(
    catalog: dict[str, str],
    directly_used: set[str],
    source_literals: set[str],
    source_prefixes: set[str],
) -> tuple[list[str], list[str]]:
    """
    Returns (unused, possibly_dynamic).

    unused: catalog keys with no evidence of use anywhere in source.
    possibly_dynamic: catalog keys not seen as exact literals but whose key prefix
                      appears as a literal string (dynamic construction likely).
    """
    unused: list[str] = []
    possibly_dynamic: list[str] = []
    for key in sorted(catalog):
        if key in directly_used or key in source_literals:
            continue
        # Check if any known prefix covers this key
        if any(key.startswith(p) for p in source_prefixes):
            possibly_dynamic.append(key)
        else:
            unused.append(key)
    return unused, possibly_dynamic


def iter_raw_scan_files(roots: tuple[str, ...], extensions: tuple[str, ...], excludes: set[Path]) -> Iterable[Path]:
    seen: set[Path] = set()
    for rel in roots:
        base = (ROOT / rel).resolve()
        if not base.is_dir():
            continue
        for path in iter_source_files(base, extensions, excludes):
            rp = path.resolve()
            if rp in seen:
                continue
            seen.add(rp)
            yield path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--translations",
        type=Path,
        default=DEFAULT_TRANSLATIONS,
        help="Path to translations JSON (default: en.json)",
    )
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC, help="Root for i18n key scan (default: src/)")
    ap.add_argument(
        "--raw-roots",
        nargs="*",
        metavar="REL_PATH",
        default=list(DEFAULT_RAW_ROOTS),
        help="Repo-relative dirs for raw UI literal scan (default: src/ui src/shell)",
    )
    ap.add_argument(
        "--exclude",
        action="append",
        default=[],
        metavar="PATH",
        help="File path relative to repo root to skip (repeatable)",
    )
    ap.add_argument("--unused", action="store_true", help="Report catalog keys not referenced in source")
    ap.add_argument("--raw", action="store_true", help="Report untranslated UI string literals")
    ap.add_argument(
        "--setters-only",
        action="store_true",
        help="With --raw: flag only UI setter calls (setText, setTitle, …), not all multi-word literals",
    )
    ap.add_argument("--all", action="store_true", help="Enable --unused and --raw")
    ap.add_argument("--fail-on-unused", action="store_true", help="Exit 1 when unused catalog keys are found")
    ap.add_argument("--fail-on-raw", action="store_true", help="Exit 1 when raw UI literal hits are found")
    args = ap.parse_args()

    if args.all:
        args.unused = True
        args.raw = True
    if args.fail_on_unused:
        args.unused = True
    if args.fail_on_raw:
        args.raw = True

    excludes: set[Path] = set()
    for p in list(DEFAULT_EXCLUDE_GLOBS) + list(args.exclude):
        excludes.add((ROOT / p).resolve())

    if not args.translations.is_file():
        print(f"error: translations file not found: {args.translations}", file=sys.stderr)
        return 2

    catalog: dict[str, str] = {}
    with args.translations.open(encoding="utf-8") as f:
        flatten_json_strings(json.load(f), "", catalog)

    all_tr: set[str] = set()
    all_trp: set[str] = set()
    all_key_literals: set[str] = set()
    all_key_prefixes: set[str] = set()
    raw_hits: list[tuple[Path, int, str, str]] = []

    for path in iter_source_files(args.src, (".cpp", ".h"), excludes):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as e:
            print(f"warn: skip {path}: {e}", file=sys.stderr)
            continue
        tr_keys, trp_bases = collect_tr_keys(text)
        all_tr |= tr_keys
        all_trp |= trp_bases
        if args.unused:
            lits, prefixes = collect_key_literals(text)
            all_key_literals |= lits
            all_key_prefixes |= prefixes

    if args.raw:
        raw_roots = tuple(args.raw_roots) if args.raw_roots else DEFAULT_RAW_ROOTS
        for path in iter_raw_scan_files(raw_roots, (".cpp", ".h"), excludes):
            try:
                text = path.read_text(encoding="utf-8")
            except OSError as e:
                print(f"warn: skip {path}: {e}", file=sys.stderr)
                continue
            for line_no, where, lit in scan_raw_ui_file(path, text, wide_strings=not args.setters_only):
                raw_hits.append((path, line_no, where, lit))
        raw_hits.sort(key=lambda h: (str(h[0]), h[1], h[2], h[3]))

    missing_tr = check_catalog(all_tr, catalog)
    trp_required: set[str] = set()
    for base in all_trp:
        trp_required.add(base)
        trp_required.add(f"{base}-plural")
    missing_trp = check_catalog(trp_required, catalog)
    missing = sorted(set(missing_tr + missing_trp))
    exit_code = 0

    if missing:
        exit_code = 1
        print(f"Missing {len(missing)} translation key(s) in {args.translations}:")
        for k in missing:
            print(f"  - {k}")

    if args.unused:
        directly_used = all_tr | all_trp | {f"{b}-plural" for b in all_trp}
        unused, possibly_dynamic = find_unused_keys(catalog, directly_used, all_key_literals, all_key_prefixes)
        if possibly_dynamic:
            print(
                f"\nPossibly dynamic ({len(possibly_dynamic)} key(s)): prefix literal found in source,"
                " full key not resolvable statically:"
            )
            for k in possibly_dynamic:
                print(f"  ? {k}")
        if unused:
            if args.fail_on_unused:
                exit_code = 1
            print(f"\nUnused catalog key(s): {len(unused)}, not referenced anywhere in source:")
            for k in unused:
                print(f"  - {k}")
        else:
            print(f"\nUnused keys: none (catalog={len(catalog)}, possibly-dynamic={len(possibly_dynamic)}).")

    if args.raw and raw_hits:
        print(f"\nRaw UI string literals ({len(raw_hits)} hit(s)), prefer i18n::tr / i18n::trp:")
        for path, line_no, where, lit in raw_hits[:400]:
            rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            shown = lit if len(lit) <= 72 else lit[:69] + "..."
            if where == "literal":
                print(f"  {rel}:{line_no}: string literal {shown!r}")
            else:
                print(f"  {rel}:{line_no}: {where}(\"{shown}\")")
        if len(raw_hits) > 400:
            print(f"  ... and {len(raw_hits) - 400} more")
        if args.fail_on_raw:
            exit_code = 1
    elif args.raw:
        print("\nRaw UI scan: clean.")

    if not missing:
        suffix = ""
        if args.raw and raw_hits:
            suffix = f"; raw UI literals: {len(raw_hits)} hit(s) (see above)"
        print(
            f"OK: {len(all_tr)} tr keys, {len(all_trp)} trp base keys"
            f", all present in catalog ({len(catalog)} entries){suffix}."
        )

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
