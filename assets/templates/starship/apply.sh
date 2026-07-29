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

tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
cleanup() {
    rm -f "$tmp_file"
}
trap cleanup EXIT

# Build the desired starship.toml into a temp file (never sed -i the live path).
# ! -f covers a missing path and a dangling symlink (write-through creates the target).
if [ ! -f "$config_file" ]; then
    {
        echo 'palette = "noctalia"'
        echo ""
        echo "$marker_begin"
        cat "$palette_file"
        echo "$marker_end"
    } >"$tmp_file"
else
    # Strip the previous noctalia palette block and any palette= line, keep the rest.
    body_file="$(mktemp "${config_file}.body.XXXXXX")"
    cleanup_body() {
        rm -f "$tmp_file" "$body_file"
    }
    trap cleanup_body EXIT

    awk -v begin="$marker_begin" -v end="$marker_end" '
        $0 == begin { in_block = 1; next }
        in_block {
            if ($0 == end) {
                in_block = 0
            }
            next
        }
        /^palette[[:space:]]*=/ { next }
        { print }
    ' "$config_file" >"$body_file"

    # Drop trailing blank lines so the leading newline below does not accumulate.
    sed -i -e :a -e '/^\n*$/{$d;N;ba}' "$body_file"

    {
        if grep -qE '^"\$schema"' "$body_file"; then
            awk '
                /^"\$schema"/ {
                    print
                    if (!inserted) {
                        print "palette = \"noctalia\""
                        inserted = 1
                    }
                    next
                }
                { print }
            ' "$body_file"
        else
            echo 'palette = "noctalia"'
            cat "$body_file"
        fi
        echo ""
        echo "$marker_begin"
        cat "$palette_file"
        echo "$marker_end"
    } >"$tmp_file"

    rm -f "$body_file"
    trap cleanup EXIT
fi

# Only touch the live path when content actually changes. Write through a symlink
# instead of replacing it with a regular file via mv/sed -i.
if [ ! -e "$config_file" ] && [ ! -L "$config_file" ]; then
    mv "$tmp_file" "$config_file"
    trap - EXIT
elif ! cmp -s "$config_file" "$tmp_file"; then
    cat "$tmp_file" >"$config_file"
fi
