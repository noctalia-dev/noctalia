#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/labwc"
theme_dir="${XDG_DATA_HOME:-$HOME/.local/share}/themes/noctalia/openbox-3"
theme_file="$theme_dir/themerc"
source_theme="$config_dir/noctalia.conf"
rc_file="$config_dir/rc.xml"

mkdir -p "$config_dir" "$theme_dir"

if [ -f "$source_theme" ]; then
    cp -f "$source_theme" "$theme_file"
fi

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

if [ ! -f "$rc_file" ]; then
    cat >"$rc_file" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<labwc_config>
  <theme>
    <name>noctalia</name>
  </theme>
</labwc_config>
EOF
    exit 0
fi

tmp_file="$(mktemp "${rc_file}.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

if grep -q '<theme>' "$rc_file"; then
    if ! grep -q '</theme>' "$rc_file"; then
        echo "Cannot update Labwc theme: $rc_file contains <theme> without </theme>" >&2
        exit 1
    fi

    if sed -n '/<theme>/,/<\/theme>/p' "$rc_file" | grep -q '<name>.*</name>'; then
        sed '/<theme>/,/<\/theme>/s|<name>.*</name>|<name>noctalia</name>|' "$rc_file" >"$tmp_file"
    else
        sed '/<theme>/a\    <name>noctalia</name>' "$rc_file" >"$tmp_file"
    fi
else
    if ! grep -qE '<labwc_config([[:space:]>])' "$rc_file"; then
        echo "Cannot update Labwc theme: $rc_file does not contain <labwc_config>" >&2
        exit 1
    fi

    sed '/<labwc_config[[:space:]>]/a\  <theme>\n    <name>noctalia</name>\n  </theme>' "$rc_file" >"$tmp_file"
fi

trap - EXIT
write_if_changed "$rc_file" "$tmp_file"
