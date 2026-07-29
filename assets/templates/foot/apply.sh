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

if [ ! -f "$config_file" ]; then
    cat >"$config_file" <<EOF
[main]
$include_line
EOF
elif ! grep -q 'include.*noctalia' "$config_file"; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT

    # Drop other theme includes, then ensure the noctalia include is present.
    sed '/include=.*themes/d' "$config_file" >"$tmp_file"
    if grep -q '^\[main\]' "$tmp_file"; then
        sed -i '/^\[main\]/a '"$include_line" "$tmp_file"
    else
        sed -i '1i [main]\n'"$include_line"'\n' "$tmp_file"
    fi

    trap - EXIT
    write_if_changed "$config_file" "$tmp_file"
fi
