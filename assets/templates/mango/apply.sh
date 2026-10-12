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

# mango falls back to /etc/mango/config.conf only while the user config is missing.
system_config=/etc/mango/config.conf
if [ ! -f "$config_file" ]; then
    if [ -f "$system_config" ]; then
        echo "Warning: not creating $config_file because it would hide $system_config; add '$include_line' to your mango config to apply the Noctalia theme" >&2
        exit 0
    fi
    printf '%s\n' "$include_line" >"$config_file"
    exit 0
fi

if ! grep -Eq '^[[:space:]]*source(-optional)?[[:space:]]*=[[:space:]]*.*noctalia\.conf' "$config_file"; then
    printf '\n%s\n' "$include_line" >>"$config_file"
fi

mmsg dispatch reload_config 2>/dev/null || true
