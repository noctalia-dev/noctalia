#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/labwc"
config_file="$config_dir/rc.xml"
theme_file="$config_dir/noctalia.conf"
openbox_theme="${XDG_DATA_HOME:-$HOME/.local/share}/themes/noctalia/openbox-3/themerc"

rm -f -- "$theme_file" "$openbox_theme"

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/<name>[[:space:]]*noctalia[[:space:]]*<\/name>/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
    fi
fi
