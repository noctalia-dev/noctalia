#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/wezterm"
config_file="$config_dir/wezterm.lua"
theme_file="$config_dir/colors/Noctalia.toml"
changed=0

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*config\.color_scheme[[:space:]]*=[[:space:]]*["'"'"']Noctalia["'"'"']/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
        changed=1
    fi
fi

if [ -e "$theme_file" ] || [ -L "$theme_file" ]; then
    rm -f -- "$theme_file"
    changed=1
fi

if [ "$changed" -eq 1 ] && [ -f "$config_file" ]; then
    touch "$config_file"
fi
