#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/btop"
config_file="$config_dir/btop.conf"
theme_file="$config_dir/themes/noctalia.theme"

rm -f -- "$theme_file"

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*color_theme[[:space:]]*=[[:space:]]*"noctalia"/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
    fi
fi
pgrep -x btop >/dev/null && pkill -SIGUSR2 -x btop || true
