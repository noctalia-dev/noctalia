#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
config_file="$config_dir/sway/config"

# sway expands ~ in include paths; keep it tilde'd so the path stays portable.
if [[ "$config_dir" == "$HOME"/* ]]; then
    include_dir="~/${config_dir#"$HOME"/}"
else
    include_dir="$config_dir"
fi
include_line="include $include_dir/sway/noctalia"

mkdir -p "$(dirname "$config_file")"

# sway loads only the first config it finds, so a new user file would hide ~/.i3/config or a
# system config.
find_other_config() {
    local candidate
    for candidate in "$HOME/.i3/config" "$config_dir/i3/config" /etc/sway/config /etc/i3/config; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [ ! -f "$config_file" ]; then
    if other_config="$(find_other_config)"; then
        echo "Warning: not creating $config_file because it would hide $other_config; add '$include_line' to your sway config to apply the Noctalia theme" >&2
        exit 0
    fi
    printf '%s\n' "$include_line" >"$config_file"
    exit 0
fi

if ! grep -q '^include .*noctalia' "$config_file"; then
    printf '\n%s\n' "$include_line" >>"$config_file"
fi
