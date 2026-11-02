#!/usr/bin/env python3
"""Ratchet on palette roles combined with a literal alpha.

`colorSpecFromRole(ColorRole::X, 0.4F)` and `colorForRole(ColorRole::X, 0.4F)` blend a role
into a new color instead of using a palette role. The count may only go down: the check fails
when it rises above the baseline, and asks for the baseline to be lowered when it drops.

Usage: tools/role-alpha-check.py [--update]
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "role-alpha-baseline.txt"
PATTERN = re.compile(
    r"\b(?:colorSpecFromRole|colorForRole)\(\s*ColorRole::\w+\s*,\s*"
    r"(?:\w+\s*\?\s*)?[0-9]*\.?[0-9]+F?(?:\s*:\s*[0-9]*\.?[0-9]+F?)?\s*\)"
)


def count_uses() -> dict[str, int]:
    counts: dict[str, int] = {}
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        found = len(PATTERN.findall(path.read_text(encoding="utf-8")))
        if found:
            counts[str(path.relative_to(ROOT))] = found
    return counts


def main() -> int:
    counts = count_uses()
    total = sum(counts.values())

    if "--update" in sys.argv[1:]:
        BASELINE.write_text(f"{total}\n", encoding="utf-8")
        print(f"role-alpha baseline set to {total}")
        return 0

    baseline = int(BASELINE.read_text(encoding="utf-8").strip())
    if total > baseline:
        print(f"error: {total} palette roles with a literal alpha, baseline is {baseline}.")
        print("Use a palette role (e.g. surface_container_high, primary_container) instead of blending one,")
        print("or a named Style constant for real transparency. See CONTRIBUTING.md, Palette colors.")
        for path, found in counts.items():
            print(f"  {found:3d} {path}")
        return 1
    if total < baseline:
        print(f"error: count dropped to {total} (baseline {baseline}). Run `just role-alpha-update` to lock it in.")
        return 1
    print(f"role-alpha: {total} (baseline {baseline})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
