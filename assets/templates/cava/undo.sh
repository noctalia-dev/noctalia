#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/cava"
config_file="$config_dir/config"
theme_file="$config_dir/themes/noctalia"

rm -f -- "$theme_file"

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*theme[[:space:]]*=[[:space:]]*"noctalia"/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
    fi
fi
if pgrep -x cava >/dev/null && ! pgrep -ax cava | grep -q -- '-p.*stdin'; then
    pkill -USR1 -x cava || true
fi
