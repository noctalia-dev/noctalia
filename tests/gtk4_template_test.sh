#!/usr/bin/env bash
set -euo pipefail

templates_dir=${1:?usage: gtk4_template_test.sh <templates-dir>}
file="$templates_dir/gtk/gtk4.css"

fail() {
    printf 'gtk4_template_test: FAIL: %s\n' "$*" >&2
    exit 1
}

[ -f "$file" ] || fail "missing $file"

grep -q '@media (prefers-color-scheme: light)' "$file" || fail "missing light media query"
grep -q '@media (prefers-color-scheme: dark)' "$file" || fail "missing dark media query"
grep -q 'colors.surface.light.hex' "$file" || fail "missing light color tokens"
grep -q 'colors.surface.dark.hex' "$file" || fail "missing dark color tokens"
grep -q 'colors.surface.default.hex' "$file" || fail "missing GTK < 4.20 fallback tokens"

light_line=$(grep -n '@media (prefers-color-scheme: light)' "$file" | cut -d: -f1)
dark_line=$(grep -n '@media (prefers-color-scheme: dark)' "$file" | cut -d: -f1)
fallback_line=$(grep -n 'colors.surface.default.hex' "$file" | head -1 | cut -d: -f1)

[ "$fallback_line" -lt "$light_line" ] || fail "fallback colors must come before the media queries"
[ "$light_line" -lt "$dark_line" ] || fail "light media query must come before the dark one"

printf 'gtk4_template_test: OK\n'
