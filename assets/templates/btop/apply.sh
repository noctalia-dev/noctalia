#!/usr/bin/env bash
set -euo pipefail

config_file="${XDG_CONFIG_HOME:-$HOME/.config}/btop/btop.conf"

if [ ! -f "$config_file" ]; then
    echo "Warning: btop config file not found at $config_file" >&2
    exit 0
fi

if grep -qE '^color_theme\s*=\s*"noctalia"' "$config_file"; then
    :
elif grep -qE '^color_theme\s*=' "$config_file"; then
    sed -i -E 's/^color_theme\s*=.*/color_theme = "noctalia"/' "$config_file"
else
    [ -s "$config_file" ] && [ -n "$(tail -c1 "$config_file")" ] && echo >>"$config_file"
    echo 'color_theme = "noctalia"' >>"$config_file"
fi

if pgrep -x btop >/dev/null; then
    pkill -SIGUSR2 -x btop
fi
