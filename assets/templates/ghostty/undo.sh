#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/ghostty"
theme_file="$config_dir/themes/noctalia"

rm -f -- "$theme_file"

for config_file in "$config_dir/config" "$config_dir/config.ghostty"; do
    [ -f "$config_file" ] || continue
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '!/^[[:space:]]*theme[[:space:]]*=[[:space:]]*noctalia[[:space:]]*$/' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
    fi
    rm -f "$tmp_file"
    trap - EXIT
done
pgrep -f ghostty >/dev/null && pkill -SIGUSR2 ghostty || true
