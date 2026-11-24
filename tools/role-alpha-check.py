#!/usr/bin/env python3
"""Ratchet on palette roles combined with a fractional alpha literal.

Use palette roles for colors and named opacity values for transparency. Legacy
composites that cannot use an existing role without a visual change require a
reason on the affected argument. The baseline counts unreviewed literals.

Usage: tools/role-alpha-check.py [--library-source PATH] [--update]
"""

import argparse
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "role-alpha-baseline.txt"
# Mask comments and strings without shifting offsets. A numbered line in a
# diagnostic then still points to the actual C++ source.
NON_CODE = re.compile(
    r'R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|"(?:\\[\s\S]|[^"\\])*"|(?<![\w\x27])\x27(?:\\[\s\S]|[^\x27\\\n])*\x27'
    r'|//[^\n]*|/\*[\s\S]*?\*/'
)
CALL = re.compile(r"\b(?:colorSpecFromRole|colorForRole)\s*\(")
TOKEN = re.compile(r"::|[()\[\]{},?:]")
NUMBER = re.compile(
    r"[+-]?\s*(?:(?:\d[\d']*\.?[\d']*|\.[\d']+)(?:[eE][+-]?[\d']+)?"
    r"|0[xX](?:[\da-fA-F']+\.?[\da-fA-F']*|\.[\da-fA-F']+)[pP][+-]?[\d']+)[fFlL]?"
)
EXCEPTION = re.compile(r"/\*\s*palette-alpha-exception:\s+([\s\S]*?)\s*\*/")
OPEN = {"(": ")", "[": "]", "{": "}"}


def top_level_tokens(code: str, start: int = 0, *, group_bounds: bool = True):
    stack = []
    for token in TOKEN.finditer(code, start):
        value = token[0]
        if value in OPEN:
            if not stack and group_bounds:
                yield token
            stack.append(OPEN[value])
        elif stack:
            if value == stack[-1]:
                stack.pop()
                if not stack and group_bounds:
                    yield token
        else:
            yield token


def alpha_argument(code: str, start: int) -> tuple[int, int] | None:
    comma = None
    for token in top_level_tokens(code, start, group_bounds=False):
        if token[0] == ",":
            if comma is not None:
                return None
            comma = token.end()
        elif token[0] == ")":
            return (comma, token.start()) if comma is not None else None
    return None


def has_literal_alpha(expr: str) -> bool:
    expr = expr.strip()
    if NUMBER.fullmatch(expr):
        cleaned = re.sub(r"\s+", "", expr.replace("'", "")).rstrip("fFlL")
        try:
            value = float.fromhex(cleaned) if "0x" in cleaned.lower() else float(cleaned)
        except ValueError:
            return True
        # Transparent and unmodified are explicit states, not palette blends.
        return value not in (0.0, 1.0)
    tokens = list(top_level_tokens(expr))
    if len(tokens) == 2 and tokens[0].start() == 0 and tokens[-1].end() == len(expr):
        if (tokens[0][0], tokens[-1][0]) in {("(", ")"), ("{", "}")}:
            return has_literal_alpha(expr[1:-1])
    question = None
    depth = 0
    for token in tokens:
        if token[0] == "?":
            if question is None:
                question = token.end()
            depth += 1
        elif token[0] == ":" and depth:
            depth -= 1
            if depth == 0:
                return has_literal_alpha(expr[question:token.start()]) or has_literal_alpha(expr[token.end():])
    return False


def scan(content: str) -> tuple[list[int], list[tuple[int, str]], list[int]]:
    comments = [token for token in NON_CODE.finditer(content) if token[0].startswith(("//", "/*"))]
    exceptions = {
        token.start(): (token.end(), " ".join(reason[1].split()))
        for token in comments
        if (reason := EXCEPTION.fullmatch(token[0])) and reason[1].strip()
    }
    markers = {token.start() for token in comments if "palette-alpha-exception:" in token[0]}
    code = NON_CODE.sub(lambda token: re.sub(r"[^\n]", " ", token[0]), content)
    findings = []
    accepted = []
    for call in CALL.finditer(code):
        bounds = alpha_argument(code, call.end())
        if bounds is None or not has_literal_alpha(code[slice(*bounds)]):
            continue
        start, end = bounds
        line = content.count("\n", 0, call.start()) + 1
        # An exception must be a trailing block comment on this alpha argument.
        tags = [pos for pos in exceptions if start <= pos < end]
        if len(tags) == 1 and not code[exceptions[tags[0]][0]:end].strip():
            pos = tags[0]
            accepted.append((line, exceptions[pos][1]))
            markers.remove(pos)
        else:
            findings.append(line)
    return findings, accepted, [content.count("\n", 0, pos) + 1 for pos in sorted(markers)]

def cpp_sources(root: pathlib.Path):
    return sorted(
        path for path in (root / "src").rglob("*")
        if path.suffix in {".cpp", ".h", ".cc", ".hpp", ".cxx", ".hxx", ".ipp", ".inl"}
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--library-source",
        type=pathlib.Path,
        default=os.environ.get("NYTHERA_LIB_SOURCE", ROOT.parent / "nythera-lib"),
        help="NytheraLib checkout (defaults to NYTHERA_LIB_SOURCE or ../nythera-lib)",
    )
    parser.add_argument("--update", action="store_true", help="Lower the baseline to the current total")
    parser.add_argument("--list-exceptions", action="store_true", help="Show reviewed exceptions and reasons")
    args = parser.parse_args()
    if not (args.library_source / "src" / "ui" / "palette.h").is_file():
        parser.error(f"NytheraLib sources not found at {args.library_source}; set --library-source")

    findings = []
    accepted = []
    bad_markers = []
    for root, name in ((ROOT, "Nythera"), (args.library_source, "NytheraLib")):
        for path in cpp_sources(root):
            location = f"{name}/{path.relative_to(root)}"
            lines, exceptions, invalid = scan(path.read_text(encoding="utf-8"))
            findings.extend(f"{location}:{line}" for line in lines)
            accepted.extend((f"{location}:{line}", reason) for line, reason in exceptions)
            bad_markers.extend(f"{location}:{line}" for line in invalid)
    total = len(findings)
    baseline = int(BASELINE.read_text(encoding="utf-8").strip())

    if bad_markers:
        for location in bad_markers:
            print(f"error: {location}: invalid or unused palette alpha exception")
        return 1

    if args.list_exceptions:
        for location, reason in accepted:
            print(f"{location}: {reason}")

    if args.update and total <= baseline:
        BASELINE.write_text(f"{total}\n", encoding="utf-8")
        print(f"role-alpha baseline set to {total}")
        return 0

    if total > baseline:
        print(f"error: {total} palette roles with a literal alpha, baseline is {baseline}.")
        print("Use a palette role (e.g. surface_container_high, primary_container) instead of blending one,")
        print("or a named Style constant for real transparency. See CONTRIBUTING.md, Palette colors.")
        for location in findings:
            print(f"  {location}")
        return 1
    if total < baseline:
        print(f"error: count dropped to {total} (baseline {baseline}). Run `just role-alpha-update` to lock it in.")
        return 1
    print(f"role-alpha: {total} unreviewed (baseline {baseline}), {len(accepted)} documented exceptions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
