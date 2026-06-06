#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
config_file="$config_dir/mango/config.conf"

# mango expands ~ in source paths; keep it tilde'd so the path stays portable.
if [[ "$config_dir" == "$HOME"/* ]]; then
    include_dir="~/${config_dir#"$HOME"/}"
else
    include_dir="$config_dir"
fi
include_line="source=$include_dir/mango/noctalia.conf"

mkdir -p "$(dirname "$config_file")"

if [ ! -f "$config_file" ]; then
    printf '%s\n' "$include_line" >"$config_file"
    exit 0
fi

if ! grep -q 'source=.*noctalia\.conf' "$config_file"; then
    printf '\n%s\n' "$include_line" >>"$config_file"
fi

mmsg dispatch reload_config 2>/dev/null || true
