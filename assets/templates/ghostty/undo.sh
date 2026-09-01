#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/ghostty"
theme_file="$config_dir/themes/noctalia"
changed=0

if [ -e "$theme_file" ] || [ -L "$theme_file" ]; then
    rm -f -- "$theme_file"
    changed=1
fi

for config_file in "$config_dir/config" "$config_dir/config.ghostty"; do
    [ -f "$config_file" ] || continue
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*theme[[:space:]]*=[[:space:]]*noctalia[[:space:]]*$/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
        changed=1
    fi
    rm -f "$tmp_file"
    trap - EXIT
done

# Reload only when this run actually removed the theme; an unsolicited reload of an
# unrelated Ghostty startup is what breaks its systemd user service.
if [ "$changed" -eq 1 ]; then
    bash "$script_dir/reload.sh"
fi
