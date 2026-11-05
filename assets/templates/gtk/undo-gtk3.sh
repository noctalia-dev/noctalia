#!/usr/bin/env bash
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/gtk-3.0"
config_file="$config_dir/gtk.css"
data_theme_dir="${XDG_DATA_HOME:-$HOME/.local/share}/themes"

# Replaces a read-only symlink (e.g. NixOS) with a local copy of its target so the
# file can be edited. Writable symlinks are left alone and written through.
make_writable() {
    local file="$1" target tmp
    [ -L "$file" ] || return 0
    target=$(readlink -f "$file") || return 0
    [ -w "$target" ] && return 0

    tmp=$(mktemp "${file}.tmp.XXXXXX")
    if [ -r "$target" ]; then
        cat "$target" > "$tmp"
    fi
    rm -f "$file"
    mv "$tmp" "$file"
}

strip_import() {
    local file="$1" needle="$2"
    [ -e "$file" ] || [ -L "$file" ] || return 0
    grep -qF "$needle" "$file" || return 0

    make_writable "$file"

    local tmp
    tmp=$(mktemp "${file}.tmp.XXXXXX")
    awk -v needle="$needle" 'index($0, needle) == 0' "$file" > "$tmp"
    if ! cmp -s "$file" "$tmp"; then
        cat "$tmp" > "$file"
    fi
    rm -f "$tmp"
}

if [ -f "$config_file" ] || [ -L "$config_file" ]; then
    strip_import "$config_file" '@import url("noctalia.css");'
    strip_import "$config_file" '@import url("noctalia-extensions.css");'
fi

rm -f -- "$config_dir/noctalia.css" "$config_dir/noctalia-gtk3.css"
rm -f -- "$config_dir/noctalia-extensions.css"
rm -rf -- "$config_dir/noctalia-extensions"

for theme in Noctalia Noctalia-dark; do
    if [ -f "$data_theme_dir/$theme/.noctalia-theme" ]; then
        rm -rf -- "$data_theme_dir/$theme"
    fi
done

if [ "${NOCTALIA_GTK_SKIP_GSETTINGS:-0}" != "1" ] \
        && command -v gsettings >/dev/null 2>&1 \
        && [[ "$(gsettings list-schemas 2>/dev/null || true)" == *"org.gnome.desktop.interface"* ]]; then
    current_theme="$(gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null || true)"
    case "$current_theme" in
        "'Noctalia'"|"'Noctalia-dark'")
            scheme="$(gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null || true)"
            case "$scheme" in
                *prefer-dark*) target_theme="adw-gtk3-dark" ;;
                *) target_theme="adw-gtk3" ;;
            esac
            gsettings set org.gnome.desktop.interface gtk-theme "$target_theme" \
                || echo "Error restoring gtk-theme to $target_theme" >&2
            ;;
    esac
fi
