#!/usr/bin/env bash
set -euo pipefail

palette_file="${XDG_CACHE_HOME:-$HOME/.cache}/noctalia/starship-palette.toml"
marker_begin="# >>> NOCTALIA STARSHIP PALETTE >>>"
marker_end="# <<< NOCTALIA STARSHIP PALETTE <<<"

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
    local environ_file="$1"
    local value
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

    # Noctalia applies templates from its daemon, which does not inherit shell-only
    # exports from .bashrc/.zshrc. Recover STARSHIP_CONFIG from the user session.
    if command -v systemctl >/dev/null 2>&1; then
        local from_systemd
        from_systemd=$(
            systemctl --user show-environment 2>/dev/null | read_env_value STARSHIP_CONFIG || true
        )
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

if [ ! -f "$palette_file" ]; then
    echo "Error: Starship palette file not found at $palette_file" >&2
    exit 1
fi

mkdir -p "$(dirname "$config_file")"

if [ ! -f "$config_file" ]; then
    echo 'palette = "noctalia"' >"$config_file"
else
    if grep -qE '^palette\s*=' "$config_file"; then
        sed -i -E 's/^palette\s*=.*/palette = "noctalia"/' "$config_file"
    elif grep -qE '^"\$schema"' "$config_file"; then
        sed -i '/^"\$schema"/a palette = "noctalia"' "$config_file"
    else
        sed -i '1i palette = "noctalia"' "$config_file"
    fi

    if grep -qF "$marker_begin" "$config_file"; then
        begin_regex=$(printf '%s' "$marker_begin" | sed 's/[[\.*^$()+?{|]/\\&/g')
        end_regex=$(printf '%s' "$marker_end" | sed 's/[[\.*^$()+?{|]/\\&/g')
        sed -i "/$begin_regex/,/$end_regex/d" "$config_file"
    fi

    # Drop trailing blank lines so the leading newline below does not accumulate
    sed -i -e :a -e '/^\n*$/{$d;N;ba}' "$config_file"
fi

{
    echo ""
    echo "$marker_begin"
    cat "$palette_file"
    echo "$marker_end"
} >>"$config_file"
