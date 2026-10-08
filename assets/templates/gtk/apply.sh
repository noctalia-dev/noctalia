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

sync_system_appearance() {
    local mode="$1" update_gtk_theme="${2:-true}"
    local has_gsettings has_dconf
    has_gsettings=$(command -v gsettings || true)
    has_dconf=$(command -v dconf || true)

    if [ -z "$has_gsettings" ] && [ -z "$has_dconf" ]; then
        echo "No gsettings or dconf found, skip system appearance sync"
        return
    fi

    local target_theme
    [ "$mode" = "light" ] && target_theme="adw-gtk3" || target_theme="adw-gtk3-dark"

    local theme_available=false
    if [ "$update_gtk_theme" = "true" ]; then
        if theme_exists "$target_theme"; then
            theme_available=true
        else
            echo "Theme '$target_theme' not found, skipping GTK theme set"
        fi
    fi

    if [ -n "$has_gsettings" ]; then
        local schemas
        schemas=$(gsettings list-schemas 2>/dev/null || true)
        if [[ "$schemas" == *"org.gnome.desktop.interface"* ]]; then
            if [ "$theme_available" = "true" ]; then
                gsettings set org.gnome.desktop.interface gtk-theme "$target_theme" \
                    || echo "Error running gsettings set gtk-theme" >&2
            fi
            gsettings set org.gnome.desktop.interface color-scheme "prefer-$mode" \
                || echo "Error running gsettings set color-scheme" >&2
            return
        fi
    fi

    if [ -n "$has_dconf" ]; then
        if [ "$theme_available" = "true" ]; then
            dconf write /org/gnome/desktop/interface/gtk-theme "'$target_theme'" \
                || echo "Error running dconf write gtk-theme" >&2
        fi
        dconf write /org/gnome/desktop/interface/color-scheme "'prefer-$mode'" \
            || echo "Error running dconf write color-scheme" >&2
    fi
}

main() {
    local appearance_only=false mode=""

    if [ "${1:-}" = "--appearance-only" ]; then
        appearance_only=true
        shift
    fi
    if [ $# -ne 1 ] || { [ "$1" != "dark" ] && [ "$1" != "light" ]; }; then
        echo "Usage: apply.sh [--appearance-only] (dark|light)" >&2
        exit 1
    fi
    mode="$1"

    if [ "$appearance_only" = "true" ]; then
        sync_system_appearance "$mode" "false"
        return
    fi

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
        sync_system_appearance "$mode" "true"
    else
        sync_system_appearance "$mode" "false"
        exit 1
    fi
}

main "$@"
