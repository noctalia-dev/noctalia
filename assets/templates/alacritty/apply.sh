#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
config_file="$config_dir/alacritty/alacritty.toml"

# alacritty expands ~ in import paths; keep it tilde'd so the path stays portable.
if [[ "$config_dir" == "$HOME"/* ]]; then
    theme_path="~/${config_dir#"$HOME"/}/alacritty/themes/noctalia.toml"
else
    theme_path="$config_dir/alacritty/themes/noctalia.toml"
fi

mkdir -p "$(dirname "$config_file")"

write_if_changed() {
    local target="$1" tmp="$2"
    if [ ! -e "$target" ] && [ ! -L "$target" ]; then
        mv "$tmp" "$target"
        return
    fi
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

# alacritty loads only the first config it finds, so a new user file would hide any of these.
find_other_config() {
    local dir dirs candidate
    local candidates=("$config_dir/alacritty.toml")
    IFS=: read -ra dirs <<<"${XDG_CONFIG_DIRS:-/etc/xdg}"
    for dir in "${dirs[@]}"; do
        [ -n "$dir" ] && candidates+=("$dir/alacritty/alacritty.toml" "$dir/alacritty.toml")
    done
    candidates+=("$HOME/.alacritty.toml" "/etc/alacritty/alacritty.toml")
    for candidate in "${candidates[@]}"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [ ! -f "$config_file" ]; then
    if other_config="$(find_other_config)"; then
        echo "Warning: not creating $config_file because it would hide $other_config; add \"$theme_path\" to [general] import in your alacritty config to apply the Noctalia theme" >&2
        exit 0
    fi
    cat >"$config_file" <<EOF
[general]
import = [
    "$theme_path"
]
EOF
    exit 0
fi

tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

if grep -q 'noctalia\.toml' "$config_file"; then
    sed -E 's|"themes/noctalia.toml"|"'"$theme_path"'"|g' "$config_file" >"$tmp_file"
elif grep -q '^\[general\]' "$config_file"; then
    if grep -q '^import\s*=' "$config_file"; then
        sed '/^import\s*=\s*\[/,/\]/{/\]/s|]|    "'"$theme_path"'",\n]|}' "$config_file" >"$tmp_file"
    else
        sed '/^\[general\]/a import = ["'"$theme_path"'"]' "$config_file" >"$tmp_file"
    fi
else
    sed '1i [general]\nimport = ["'"$theme_path"'"]\n' "$config_file" >"$tmp_file"
fi

trap - EXIT
write_if_changed "$config_file" "$tmp_file"
