#!/usr/bin/env bash
set -euo pipefail

GTK4_IMPORT='@import url("noctalia.css");'
GTK3_EXT_IMPORT='@import url("noctalia-extensions.css");'
THEME_LIGHT='Noctalia'
THEME_DARK='Noctalia-dark'

theme_search_paths() {
    local -a paths=(
        "$HOME/.themes"
        "${XDG_DATA_HOME:-$HOME/.local/share}/themes"
    )

    # Test hook: keep the fallback path reachable on machines that have adw-gtk3.
    if [ "${NOCTALIA_GTK_SKIP_SYSTEM_THEMES:-0}" = "1" ]; then
        printf '%s\n' "${paths[@]}"
        return
    fi

    paths+=(/usr/share/themes /usr/local/share/themes)
    if [ -n "${XDG_DATA_DIRS:-}" ]; then
        local dir
        IFS=':' read -ra xdg_dirs <<< "$XDG_DATA_DIRS"
        for dir in "${xdg_dirs[@]}"; do
            [ -n "$dir" ] && paths+=("$dir/themes")
        done
    fi
    printf '%s\n' "${paths[@]}"
}

# Prints the first existing theme directory for a theme name.
theme_path() {
    local name="$1" base
    while IFS= read -r base; do
        if [ -d "$base/$name" ]; then
            printf '%s\n' "$base/$name"
            return 0
        fi
    done < <(theme_search_paths)
    return 1
}

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

ensure_import() {
    local file="$1" line="$2" needle="$3"
    mkdir -p "$(dirname "$file")"

    if [ -e "$file" ] || [ -L "$file" ]; then
        grep -qF "$needle" "$file" && return 0

        make_writable "$file"
        printf '\n%s\n' "$line" >> "$file"
    else
        printf '%s\n' "$line" > "$file"
    fi
}

extract_block() {
    local stage="$1" mode="$2" out="$3"
    awk -v start="/* noctalia-gtk3-${mode}-start */" \
        -v end="/* noctalia-gtk3-${mode}-end */" '
        $0 == start { inblock = 1; next }
        $0 == end   { inblock = 0; exit }
        inblock     { print }
    ' "$stage" > "$out"
    [ -s "$out" ]
}

append_colors() {
    local file="$1" colors="$2"
    {
        printf '\n/* Noctalia colors */\n'
        cat "$colors"
        printf '\n'
    } >> "$file"
}

install_theme_variant() {
    local name="$1" base_dir="$2" normal_colors="$3" dark_colors="$4"
    local dest="${XDG_DATA_HOME:-$HOME/.local/share}/themes/$name"

    if [ ! -d "$base_dir/gtk-3.0" ]; then
        echo "Error: base GTK3 theme not found at $base_dir/gtk-3.0" >&2
        return 1
    fi

    if [ -e "$dest" ] && [ ! -f "$dest/.noctalia-theme" ]; then
        echo "Refusing to overwrite existing theme directory: $dest" >&2
        return 1
    fi

    rm -rf "$dest" || return 1
    mkdir -p "$dest" || return 1
    cp -a "$base_dir/gtk-3.0" "$dest/" || return 1
    printf 'noctalia\n' > "$dest/.noctalia-theme" || return 1

    append_colors "$dest/gtk-3.0/gtk.css" "$normal_colors" || return 1
    if [ -f "$dest/gtk-3.0/gtk-dark.css" ]; then
        append_colors "$dest/gtk-3.0/gtk-dark.css" "$dark_colors" || return 1
    fi

    cat > "$dest/index.theme" <<EOF
[Desktop Entry]
Type=X-GNOME-Metatheme
Name=$name
Comment=Noctalia GTK3 theme
Encoding=UTF-8
EOF
}

install_gtk3_theme() {
    local light_colors="$1" dark_colors="$2"
    local light_base dark_base

    if ! light_base=$(theme_path adw-gtk3); then
        echo "Theme 'adw-gtk3' not found, cannot build the Noctalia GTK3 theme" >&2
        return 1
    fi
    if ! dark_base=$(theme_path adw-gtk3-dark); then
        echo "Theme 'adw-gtk3-dark' not found, cannot build the Noctalia GTK3 theme" >&2
        return 1
    fi

    # set -e does not apply inside functions called from an if, so each failure is checked here.
    install_theme_variant "$THEME_LIGHT" "$light_base" "$light_colors" "$dark_colors" || return 1
    install_theme_variant "$THEME_DARK" "$dark_base" "$dark_colors" "$dark_colors" || return 1
}

sync_gtk_theme() {
    local target_theme="$1"

    if ! theme_path "$target_theme" >/dev/null; then
        echo "Theme '$target_theme' not found, skipping GTK theme set" >&2
        return 0
    fi

    if [ "${NOCTALIA_GTK_SKIP_GSETTINGS:-0}" = "1" ]; then
        echo "Skipping gsettings gtk-theme update (NOCTALIA_GTK_SKIP_GSETTINGS=1)"
        return 0
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

    local gtk3_ok=false gtk4_ok=false gtk3_theme=""
    local gtk3_stage="$config_dir/gtk-3.0/noctalia-gtk3.css"
    local gtk4_colors="$config_dir/gtk-4.0/noctalia.css"

    if [ -f "$gtk3_stage" ]; then
        local tmpdir
        tmpdir=$(mktemp -d)
        trap "rm -rf '$tmpdir'" EXIT

        if extract_block "$gtk3_stage" light "$tmpdir/light.css" \
                && extract_block "$gtk3_stage" dark "$tmpdir/dark.css"; then
            if install_gtk3_theme "$tmpdir/light.css" "$tmpdir/dark.css"; then
                # The old override is not used any more; colors live in the theme.
                strip_import "$config_dir/gtk-3.0/gtk.css" '@import url("noctalia.css");'
                ensure_import "$config_dir/gtk-3.0/gtk.css" "$GTK3_EXT_IMPORT" 'noctalia-extensions.css'
                rm -f "$config_dir/gtk-3.0/noctalia.css"
                gtk3_theme="$THEME_DARK"
                [ "$mode" = "light" ] && gtk3_theme="$THEME_LIGHT"
                gtk3_ok=true
            else
                # Keep the old single-mode override working when the adw-gtk3 base
                # theme is unavailable or a foreign Noctalia theme directory exists.
                strip_import "$config_dir/gtk-3.0/gtk.css" '@import url("noctalia-extensions.css");'
                {
                    printf '/* Generated by Noctalia (fallback) */\n'
                    printf '%s\n\n' "$GTK3_EXT_IMPORT"
                    cat "$tmpdir/$mode.css"
                } > "$config_dir/gtk-3.0/noctalia.css"
                ensure_import "$config_dir/gtk-3.0/gtk.css" '@import url("noctalia.css");' 'noctalia.css'
                gtk3_theme="adw-gtk3-dark"
                [ "$mode" = "light" ] && gtk3_theme="adw-gtk3"
                gtk3_ok=true
            fi
        else
            echo "Error: could not extract light/dark colors from $gtk3_stage" >&2
        fi
    fi

    if [ -f "$gtk4_colors" ]; then
        ensure_import "$config_dir/gtk-4.0/gtk.css" "$GTK4_IMPORT" 'noctalia.css'
        gtk4_ok=true
    fi

    [ "$gtk3_ok" = "true" ] && echo "GTK3 colors applied successfully"
    [ "$gtk4_ok" = "true" ] && echo "GTK4 colors applied successfully"

    if [ "$gtk3_ok" = "true" ]; then
        sync_gtk_theme "$gtk3_theme"
    fi

    if [ "$gtk3_ok" = "true" ] || [ "$gtk4_ok" = "true" ]; then
        exit 0
    fi
    exit 1
}

main "$@"
