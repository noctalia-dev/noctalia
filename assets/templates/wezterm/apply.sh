#!/usr/bin/env bash
set -euo pipefail

config_file="${XDG_CONFIG_HOME:-$HOME/.config}/wezterm/wezterm.lua"
scheme_line='config.color_scheme = "Noctalia"'

if [ ! -f "$config_file" ]; then
    echo "Error: wezterm.lua not found at $config_file" >&2
    exit 1
fi

write_if_changed() {
    local target="$1" tmp="$2"
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

if ! grep -q "^\s*config\.color_scheme\s*=\s*['\"]Noctalia['\"]\s*" "$config_file"; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT

    if grep -q '^\s*config\.color_scheme\s*=' "$config_file"; then
        sed "s|^\(\s*config\.color_scheme\s*=\s*\).*$|\1\"Noctalia\"|" "$config_file" >"$tmp_file"
        trap - EXIT
        write_if_changed "$config_file" "$tmp_file"
    elif grep -q '^\s*return\s*config' "$config_file"; then
        sed '/^\s*return\s*config/i\'"$scheme_line" "$config_file" >"$tmp_file"
        trap - EXIT
        write_if_changed "$config_file" "$tmp_file"
    else
        rm -f "$tmp_file"
        trap - EXIT
        echo "Warning: config.color_scheme not set and return config not found in $config_file" >&2
    fi
fi

touch "$config_file"
