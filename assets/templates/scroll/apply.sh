#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
config_file="$config_dir/scroll/config"

# scroll expands ~ in include paths; keep it tilde'd so the path stays portable.
if [[ "$config_dir" == "$HOME"/* ]]; then
    include_dir="~/${config_dir#"$HOME"/}"
else
    include_dir="$config_dir"
fi
include_line="include $include_dir/scroll/noctalia"

mkdir -p "$(dirname "$config_file")"

if [ ! -f "$config_file" ]; then
    printf 'Warning: %s is missing; add the Noctalia theme include to your active Scroll configuration manually:\ninclude "%s/scroll/noctalia"\n' "$config_file" "$config_dir" >&2
    exit 1
fi

if ! grep -q '^include .*noctalia' "$config_file"; then
    printf '\n%s\n' "$include_line" >>"$config_file"
fi
