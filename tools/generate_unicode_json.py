#!/usr/bin/env python3

import argparse
import json
import unicodedata
from pathlib import Path


# Inclusive codepoint ranges offered by the launcher's Unicode category.
RANGES = [
    (0x00A1, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x024F),  # Latin Extended-A/B
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x2010, 0x205E),  # General Punctuation
    (0x2070, 0x209F),  # Superscripts and Subscripts
    (0x20A0, 0x20C0),  # Currency Symbols
    (0x2100, 0x218F),  # Letterlike Symbols, Number Forms
    (0x2190, 0x23FF),  # Arrows, Mathematical Operators, Misc Technical
    (0x2460, 0x27FF),  # Enclosed Alphanumerics through Supplemental Arrows-A
    (0x2900, 0x2BFF),  # Supplemental Arrows-B through Misc Symbols and Arrows
]


def parse_args():
    parser = argparse.ArgumentParser(description="Generate Noctalia launcher Unicode character data.")
    parser.add_argument("--emoji", required=True, help="Path to assets/emoji.json (its characters are skipped)")
    parser.add_argument("--output", required=True, help="Path to write unicode.json")
    return parser.parse_args()


def main():
    args = parse_args()
    emoji = json.loads(Path(args.emoji).read_text(encoding="utf-8"))
    emoji_chars = {item["emoji"].replace("\ufe0f", "") for item in emoji}

    entries = []
    for start, end in RANGES:
        for codepoint in range(start, end + 1):
            char = chr(codepoint)
            if unicodedata.category(char)[0] in "CZM":
                continue
            name = unicodedata.name(char, "")
            if not name or char in emoji_chars:
                continue
            entries.append({"char": char, "name": name.lower(), "codepoint": codepoint})

    with Path(args.output).open("w", encoding="utf-8") as out:
        json.dump(entries, out, ensure_ascii=False, indent=2)
        out.write("\n")


if __name__ == "__main__":
    main()
