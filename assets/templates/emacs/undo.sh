#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
theme_file="$(bash "$script_dir/output-path.sh")"

[ -z "$theme_file" ] || rm -f -- "$theme_file"
emacsclient -e "(when (custom-theme-enabled-p 'noctalia) (disable-theme 'noctalia))" >/dev/null 2>&1 || true
