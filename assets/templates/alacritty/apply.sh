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

if [ ! -f "$config_file" ]; then
    cat >"$config_file" <<EOF
[general]
import = [
    "$theme_path"
]
EOF
    exit 0
fi

if grep -q 'noctalia\.toml' "$config_file"; then
    sed -i 's|"themes/noctalia.toml"|"'"$theme_path"'"|g' "$config_file"
elif grep -q '^\[general\]' "$config_file"; then
    if grep -q '^import\s*=' "$config_file"; then
        sed -i '/^import\s*=\s*\[/,/\]/{/\]/s|]|    "'"$theme_path"'",\n]|}' "$config_file"
    else
        sed -i '/^\[general\]/a import = ["'"$theme_path"'"]' "$config_file"
    fi
else
    sed -i '1i [general]\nimport = ["'"$theme_path"'"]\n' "$config_file"
fi
