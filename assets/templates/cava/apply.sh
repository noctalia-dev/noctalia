#!/usr/bin/env bash
set -euo pipefail

config_file="${XDG_CONFIG_HOME:-$HOME/.config}/cava/config"

if [ ! -f "$config_file" ]; then
    echo "Error: cava config file not found at $config_file" >&2
    exit 1
fi

write_if_changed() {
    local target="$1" tmp="$2"
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

if grep -q '^\[color\]' "$config_file"; then
    if sed -n '/^\[color\]/,/^\[/p' "$config_file" | grep -qE '^theme\s*=\s*"noctalia"'; then
        :
    elif sed -n '/^\[color\]/,/^\[/p' "$config_file" | grep -qE '^theme\s*='; then
        tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
        sed -E '/^\[color\]/,/^\[/{s/^theme\s*=.*/theme = "noctalia"/}' "$config_file" >"$tmp_file"
        write_if_changed "$config_file" "$tmp_file"
    else
        tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
        sed '/^\[color\]/a theme = "noctalia"' "$config_file" >"$tmp_file"
        write_if_changed "$config_file" "$tmp_file"
    fi
else
    printf '\n[color]\ntheme = "noctalia"\n' >>"$config_file"
fi

if pgrep -x cava >/dev/null; then
    if ! pgrep -ax cava | grep -q -- '-p.*stdin'; then
        pkill -USR1 -x cava || true
    fi
fi
