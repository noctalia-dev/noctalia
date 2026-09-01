#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/mango"
config_file="$config_dir/config.conf"
theme_file="$config_dir/noctalia.conf"
changed=0

if [ -e "$theme_file" ] || [ -L "$theme_file" ]; then
    rm -f -- "$theme_file"
    changed=1
fi

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*source[[:space:]]*=[[:space:]]*.*noctalia\.conf/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
        changed=1
    fi
fi

if [ "$changed" -eq 1 ]; then
    mmsg dispatch reload_config 2>/dev/null || true
fi
