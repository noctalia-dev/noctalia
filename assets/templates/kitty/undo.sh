#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/kitty"
config_file="$config_dir/kitty.conf"
theme_file="$config_dir/themes/noctalia.conf"
changed=0

if [ -e "$theme_file" ] || [ -L "$theme_file" ]; then
    rm -f -- "$theme_file"
    changed=1
fi

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*include[[:space:]]+themes\/noctalia\.conf[[:space:]]*$/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
        changed=1
    fi
fi

# A reload signal delivered to a starting process kills it, so signal only when this
# run actually removed the theme.
if [ "$changed" -eq 1 ]; then
    pkill -USR1 -x kitty >/dev/null 2>&1 || true
fi
