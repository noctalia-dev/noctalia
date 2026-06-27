#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/kitty"
config_file="$config_dir/kitty.conf"

mkdir -p "$config_dir"

if [ ! -f "$config_file" ]; then
    cat >"$config_file" <<'EOF'
include themes/noctalia.conf
EOF
    pkill -USR1 kitty >/dev/null 2>&1 || true
    exit 0
fi

if [ -w "$config_file" ]; then
    kitty +kitten themes --reload-in=all noctalia
else
    kitty +runpy "from kitty.utils import *; reload_conf_in_all_kitties()"
fi

pkill -USR1 kitty >/dev/null 2>&1 || true
