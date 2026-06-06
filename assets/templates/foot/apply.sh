#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
config_file="$config_dir/foot/foot.ini"

# foot expands ~ in include paths; keep it tilde'd so the path stays portable.
if [[ "$config_dir" == "$HOME"/* ]]; then
    include_dir="~/${config_dir#"$HOME"/}"
else
    include_dir="$config_dir"
fi
include_line="include=$include_dir/foot/themes/noctalia"

mkdir -p "$(dirname "$config_file")"

if [ ! -f "$config_file" ]; then
    cat >"$config_file" <<EOF
[main]
$include_line
EOF
elif ! grep -q 'include.*noctalia' "$config_file"; then
    sed -i '/include=.*themes/d' "$config_file"
    if grep -q '^\[main\]' "$config_file"; then
        sed -i '/^\[main\]/a '"$include_line" "$config_file"
    else
        sed -i '1i [main]\n'"$include_line"'\n' "$config_file"
    fi
fi
