#!/usr/bin/env bash
set -euo pipefail

config_files=("${XDG_CONFIG_HOME:-$HOME/.config}/ghostty/config" "${XDG_CONFIG_HOME:-$HOME/.config}/ghostty/config.ghostty")
found=false

write_if_changed() {
    local target="$1" tmp="$2"
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

for config_file in "${config_files[@]}"; do
    [ -f "$config_file" ] || continue
    found=true

    if grep -qE '^theme\s*=\s*noctalia$' "$config_file"; then
        :
    elif grep -qE '^theme\s*=' "$config_file"; then
        tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
        sed -E 's/^theme\s*=.*/theme = noctalia/' "$config_file" >"$tmp_file"
        write_if_changed "$config_file" "$tmp_file"
    else
        [ -s "$config_file" ] && [ -n "$(tail -c1 "$config_file")" ] && echo >>"$config_file"
        echo "theme = noctalia" >>"$config_file"
    fi
done

if [ "$found" != true ]; then
    echo "Error: no ghostty config file found" >&2
    exit 1
fi

pgrep -f ghostty >/dev/null && pkill -SIGUSR2 ghostty || true
