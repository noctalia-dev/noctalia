#!/usr/bin/env bash
set -euo pipefail

GTK_IMPORT='@import url("noctalia.css");'

theme_exists() {
    local name="$1"
    local -a paths=(
        "$HOME/.themes"
        "${XDG_DATA_HOME:-$HOME/.local/share}/themes"
        /usr/share/themes
        /usr/local/share/themes
    )
    if [ -n "${XDG_DATA_DIRS:-}" ]; then
        IFS=':' read -ra xdg_dirs <<< "$XDG_DATA_DIRS"
        for dir in "${xdg_dirs[@]}"; do
            [ -n "$dir" ] && paths+=("$dir/themes")
        done
    fi
    for base in "${paths[@]}"; do
        [ -d "$base/$name" ] && return 0
    done
    return 1
}

ensure_gtk_css_import() {
    local gtk_css="$1" colors_file="$2" label="$3"

    if [ ! -f "$colors_file" ]; then
        echo "Error: $label noctalia.css not found at $colors_file" >&2
        return 1
    fi

    if [ -e "$gtk_css" ] || [ -L "$gtk_css" ]; then
        local content
        content=$(cat "$gtk_css")
        if [[ "$content" == *"noctalia.css"* ]] && [[ "$content" == *"@import"* ]]; then
            return 0
        fi
        local target="$gtk_css"
        if [ -L "$gtk_css" ]; then
            local resolved
            resolved=$(readlink -f "$gtk_css")
            if [ -w "$resolved" ]; then
                target="$resolved"
            else
                # Read-only symlink (e.g. NixOS): convert to a local file
                rm "$gtk_css"
            fi
        fi
        printf '%s\n\n%s\n' "$content" "$GTK_IMPORT" > "$target"
        echo "Appended $label noctalia.css import to gtk.css"
    else
        printf '%s\n' "$GTK_IMPORT" > "$gtk_css"
        echo "Created $label gtk.css with noctalia.css import"
    fi
}

# The color-scheme preference is owned by Noctalia's [theme].sync_color_scheme; this script only
# selects the matching adw-gtk3 variant.
sync_gtk_theme() {
    local mode="$1"
    local target_theme
    [ "$mode" = "light" ] && target_theme="adw-gtk3" || target_theme="adw-gtk3-dark"

    if ! theme_exists "$target_theme"; then
        echo "Theme '$target_theme' not found, skipping GTK theme set"
        return
    fi

    if command -v gsettings >/dev/null \
            && [[ "$(gsettings list-schemas 2>/dev/null || true)" == *"org.gnome.desktop.interface"* ]]; then
        gsettings set org.gnome.desktop.interface gtk-theme "$target_theme" \
            || echo "Error running gsettings set gtk-theme" >&2
    elif command -v dconf >/dev/null; then
        dconf write /org/gnome/desktop/interface/gtk-theme "'$target_theme'" \
            || echo "Error running dconf write gtk-theme" >&2
    else
        echo "No gsettings or dconf found, skip GTK theme set"
    fi
}

main() {
    if [ $# -ne 1 ] || { [ "$1" != "dark" ] && [ "$1" != "light" ]; }; then
        echo "Usage: apply.sh (dark|light)" >&2
        exit 1
    fi
    local mode="$1"

    local config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"

    if [ ! -d "$config_dir" ]; then
        echo "Error: Config directory not found: $config_dir" >&2
        exit 1
    fi

    mkdir -p "$config_dir/gtk-3.0" "$config_dir/gtk-4.0"

    local gtk3_ok=true gtk4_ok=true
    if ! ensure_gtk_css_import \
            "$config_dir/gtk-3.0/gtk.css" "$config_dir/gtk-3.0/noctalia.css" "GTK3"; then
        gtk3_ok=false
    fi
    if ! ensure_gtk_css_import \
            "$config_dir/gtk-4.0/gtk.css" "$config_dir/gtk-4.0/noctalia.css" "GTK4"; then
        gtk4_ok=false
    fi

    [ "$gtk3_ok" = "true" ] && echo "GTK3 colors applied successfully"
    [ "$gtk4_ok" = "true" ] && echo "GTK4 colors applied successfully"

    if [ "$gtk3_ok" = "true" ] && [ "$gtk4_ok" = "true" ]; then
        sync_gtk_theme "$mode"
    else
        exit 1
    fi
}

main "$@"
