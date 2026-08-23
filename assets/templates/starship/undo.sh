#!/usr/bin/env bash
set -euo pipefail

palette_file="${XDG_CACHE_HOME:-$HOME/.cache}/noctalia/starship-palette.toml"
marker_begin="# >>> NOCTALIA STARSHIP PALETTE >>>"
marker_end="# <<< NOCTALIA STARSHIP PALETTE <<<"

rm -f -- "$palette_file"

expand_tilde() {
    case "$1" in
        "~") printf '%s' "$HOME" ;;
        "~/"*) printf '%s' "$HOME/${1#~/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

read_env_value() {
    awk -F= -v env_name="$1" '$1 == env_name { sub(/^[^=]*=/, ""); print; exit }'
}

discover_starship_config_from_environ_file() {
    local environ_file="$1" value
    [ -r "$environ_file" ] || return 1
    value=$(tr '\0' '\n' <"$environ_file" | read_env_value STARSHIP_CONFIG || true)
    if [ -n "$value" ]; then
        expand_tilde "$value"
        return 0
    fi
    return 1
}

discover_starship_config() {
    if [ -n "${STARSHIP_CONFIG:-}" ]; then
        expand_tilde "$STARSHIP_CONFIG"
        return 0
    fi

    if command -v systemctl >/dev/null 2>&1; then
        local from_systemd
        from_systemd=$(systemctl --user show-environment 2>/dev/null | read_env_value STARSHIP_CONFIG || true)
        if [ -n "$from_systemd" ]; then
            expand_tilde "$from_systemd"
            return 0
        fi
    fi

    local proc pid owner discovered
    shopt -s nullglob
    for proc in /proc/[0-9]*/environ; do
        pid=${proc#/proc/}
        pid=${pid%/environ}
        owner=$(stat -c '%u' "/proc/$pid" 2>/dev/null || true)
        [ "$owner" = "$(id -u)" ] || continue
        if discovered=$(discover_starship_config_from_environ_file "$proc"); then
            printf '%s' "$discovered"
            return 0
        fi
    done

    printf '%s' "${XDG_CONFIG_HOME:-$HOME/.config}/starship.toml"
}

config_file=$(discover_starship_config)
if [ -f "$config_file" ]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk -v begin="$marker_begin" -v end="$marker_end" '
        $0 == begin { in_block = 1; next }
        in_block && $0 == end { in_block = 0; next }
        in_block { next }
        /^[[:space:]]*palette[[:space:]]*=[[:space:]]*"noctalia"/ { next }
        { print }
    ' "$config_file" >"$tmp_file"
    if ! cmp -s "$config_file" "$tmp_file"; then
        cat "$tmp_file" >"$config_file"
    fi
fi
