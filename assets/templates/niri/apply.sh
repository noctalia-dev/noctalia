#!/usr/bin/env bash
set -euo pipefail

command="${1:-apply}"

resolve_config_home() {
    if [ -n "${XDG_CONFIG_HOME:-}" ]; then
        printf '%s\n' "$XDG_CONFIG_HOME"
        return
    fi

    if [ -z "${HOME:-}" ]; then
        echo "error: HOME or XDG_CONFIG_HOME must be set" >&2
        exit 1
    fi

    printf '%s/.config\n' "$HOME"
}

config_dir="$(resolve_config_home)/niri"
config_file="$config_dir/config.kdl"
output_file="$config_dir/noctalia.kdl"
include_line='include "noctalia.kdl"'

has_noctalia_include() {
    grep -Eq '^[[:space:]]*include([[:space:]].*)?"([^"]*/)?noctalia\.kdl"([[:space:]]|$)' "$config_file"
}

apply_include() {
    mkdir -p "$config_dir"

    if [ ! -f "$config_file" ]; then
        # niri falls back to /etc/niri/config.kdl only while the user config is missing.
        if [ -f /etc/niri/config.kdl ]; then
            echo "Warning: not creating $config_file because it would hide /etc/niri/config.kdl; add 'include \"$output_file\"' to your niri config to apply the Noctalia theme" >&2
            return
        fi
        printf '%s\n' "$include_line" >"$config_file"
        return
    fi

    if has_noctalia_include; then
        return
    else
        local grep_status=$?
        if [ "$grep_status" -ne 1 ]; then
            echo "error: failed to inspect $config_file" >&2
            exit "$grep_status"
        fi
    fi

    printf '\n%s\n' "$include_line" >>"$config_file"
}

case "$command" in
    output)
        printf '%s\n' "$output_file"
        ;;
    apply)
        apply_include
        ;;
    *)
        echo "Usage: $0 {output|apply}" >&2
        exit 1
        ;;
esac
