#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/cava"
config_file="$config_dir/config"
theme_file="$config_dir/themes/noctalia"
changed=0

if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*theme[[:space:]]*=[[:space:]]*"noctalia"/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
        changed=1
    fi
fi

if [ -e "$theme_file" ] || [ -L "$theme_file" ]; then
    rm -f -- "$theme_file"
    changed=1
fi

# A reload signal delivered to a starting process kills it, so signal only when this
# run actually removed the theme. Raw-stdin cava instances do not reload.
if [ "$changed" -eq 1 ] && pgrep -x cava >/dev/null && ! pgrep -ax cava | grep -q -- '-p.*stdin'; then
    pkill -USR1 -x cava || true
fi
