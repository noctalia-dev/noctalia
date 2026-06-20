#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/velo"
config_file="$config_dir/config"
palette_name="current-noctalia-override"

darkmode="true"
[ "${1:-}" = "light" ] && darkmode="false"

mkdir -p "$config_dir"

if [ ! -f "$config_file" ]; then
    printf 'palette = "%s"\ndarkmode = %s\n' "$palette_name" "$darkmode" > "$config_file"
    exit 0
fi

if grep -q '^palette[[:space:]]*=' "$config_file"; then
    sed -i 's|^palette[[:space:]]*=.*|palette = "'"$palette_name"'"|' "$config_file"
else
    printf 'palette = "%s"\n' "$palette_name" >> "$config_file"
fi

if grep -q '^darkmode[[:space:]]*=' "$config_file"; then
    sed -i 's|^darkmode[[:space:]]*=.*|darkmode = '"$darkmode"'|' "$config_file"
else
    printf 'darkmode = %s\n' "$darkmode" >> "$config_file"
fi
