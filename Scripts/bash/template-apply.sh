#!/usr/bin/env -S bash

# Ensure at least one argument is provided.
if [ "$#" -lt 1 ]; then
    # Print usage information to standard error.
    echo "Error: No application specified." >&2
    echo "Usage: $0 {kitty|ghostty|foot|alacritty|wezterm|starship|fuzzel|walker|pywalfox|cava|yazi|labwc|niri|hyprland|sway|scroll|mango|btop|zathura} [dark|light]" >&2
    exit 1
fi

APP_NAME="$1"
MODE="${2:-}" # Optional second argument for dark/light mode

# --- Apply theme based on the application name ---
case "$APP_NAME" in
kitty)
    # Many configs use: include ./current-theme.conf
    # Point it at the generated theme whenever the hook runs (including when noctalia.conf
    # was unchanged on disk and the hook was forced from the template processor).
    NOCTALIA_THEME="$HOME/.config/kitty/themes/noctalia.conf"
    CURRENT_THEME="$HOME/.config/kitty/current-theme.conf"
    if [ -f "$NOCTALIA_THEME" ]; then
        mkdir -p "$HOME/.config/kitty"
        ln -sf "themes/noctalia.conf" "$CURRENT_THEME"
    fi
    KITTY_CONF="$HOME/.config/kitty/kitty.conf"
    if [ -w "$KITTY_CONF" ]; then
        kitty +kitten themes --reload-in=all noctalia
    else
        kitty +runpy "from kitty.utils import *; reload_conf_in_all_kitties()"
    fi
    # Trigger kitty's live config reload after the template has been regenerated.
    pkill -USR1 kitty >/dev/null 2>&1 || true
    ;;

ghostty)
    # Check both potential config files
    CONFIG_FILES=("$HOME/.config/ghostty/config" "$HOME/.config/ghostty/config.ghostty")
    FOUND_CONFIG=false

    for CONFIG_FILE in "${CONFIG_FILES[@]}"; do
        if [ -f "$CONFIG_FILE" ]; then
            FOUND_CONFIG=true
            # Check if theme is already set to noctalia (flexible spacing)
            if grep -qE "^theme\s*=\s*noctalia$" "$CONFIG_FILE"; then
                : # Already correct
            elif grep -qE "^theme\s*=" "$CONFIG_FILE"; then
                # Replace existing theme line in-place
                sed -i -E 's/^theme\s*=.*/theme = noctalia/' "$CONFIG_FILE"
            else
                # Add the new theme line to the end of the file
                echo "theme = noctalia" >>"$CONFIG_FILE"
            fi
        fi
    done

    if [ "$FOUND_CONFIG" = true ]; then
        # Only signal if ghostty is running
        pgrep -f ghostty >/dev/null && pkill -SIGUSR2 ghostty || true
    else
        echo "Error: No ghostty config file found at ${CONFIG_FILES[*]}" >&2
        exit 1
    fi
    ;;

foot)
    CONFIG_FILE="$HOME/.config/foot/foot.ini"

    # Check if the config file exists, create it if it doesn't.
    if [ ! -f "$CONFIG_FILE" ]; then
        # Create the config directory if it doesn't exist
        mkdir -p "$(dirname "$CONFIG_FILE")"
        # Create the config file with the noctalia theme
        cat >"$CONFIG_FILE" <<'EOF'
[main]
include=~/.config/foot/themes/noctalia
EOF
    else
        # Check if theme is already set to noctalia
        if ! grep -q "include.*noctalia" "$CONFIG_FILE"; then
            # Remove any existing theme include line to prevent duplicates.
            sed -i '/include=.*themes/d' "$CONFIG_FILE"
            if grep -q '^\[main\]' "$CONFIG_FILE"; then
                # Insert the include line after the existing [main] section header
                sed -i '/^\[main\]/a include=~/.config/foot/themes/noctalia' "$CONFIG_FILE"
            else
                # If [main] doesn't exist, create it at the beginning with the include
                sed -i '1i [main]\ninclude=~/.config/foot/themes/noctalia\n' "$CONFIG_FILE"
            fi
        fi
    fi
    ;;

alacritty)
    CONFIG_FILE="$HOME/.config/alacritty/alacritty.toml"
    NEW_THEME_PATH='~/.config/alacritty/themes/noctalia.toml'

    # Check if the config file exists, create it if it doesn't.
    if [ ! -f "$CONFIG_FILE" ]; then
        # Create the config directory if it doesn't exist
        mkdir -p "$(dirname "$CONFIG_FILE")"
        # Create the config file with the noctalia theme import
        cat >"$CONFIG_FILE" <<'EOF'
[general]
import = [
    "~/.config/alacritty/themes/noctalia.toml"
]
EOF
    else
        # Check if noctalia theme is already imported (any path variant)
        if grep -q 'noctalia\.toml' "$CONFIG_FILE"; then
            # Update old relative path to new absolute path if needed
            if grep -q '"themes/noctalia.toml"' "$CONFIG_FILE"; then
                sed -i 's|"themes/noctalia.toml"|"'"$NEW_THEME_PATH"'"|g' "$CONFIG_FILE"
            fi
            # Already has noctalia import with correct path, nothing to do
        else
            # No noctalia import found, add it
            if grep -q '^\[general\]' "$CONFIG_FILE"; then
                # Check if import line already exists under [general]
                if grep -q '^import\s*=' "$CONFIG_FILE"; then
                    # Append to existing import array (before the closing bracket)
                    sed -i '/^import\s*=\s*\[/,/\]/{/\]/s|]|    "'"$NEW_THEME_PATH"'",\n]|}' "$CONFIG_FILE"
                else
                    # Add import line after [general] section header
                    sed -i '/^\[general\]/a import = ["'"$NEW_THEME_PATH"'"]' "$CONFIG_FILE"
                fi
            else
                # Create [general] section with import at the beginning of the file
                sed -i '1i [general]\nimport = ["'"$NEW_THEME_PATH"'"]\n' "$CONFIG_FILE"
            fi
        fi
    fi
    ;;

wezterm)
    CONFIG_FILE="$HOME/.config/wezterm/wezterm.lua"
    WEZTERM_SCHEME_LINE='config.color_scheme = "Noctalia"'

    # Check if the config file exists.
    if [ -f "$CONFIG_FILE" ]; then

        # Check if theme is already set to Noctalia (matches 'Noctalia' or "Noctalia")
        if ! grep -q "^\s*config\.color_scheme\s*=\s*['\"]Noctalia['\"]\s*" "$CONFIG_FILE"; then
            # Not set to Noctalia. Check if *any* color_scheme line exists.
            if grep -q '^\s*config\.color_scheme\s*=' "$CONFIG_FILE"; then
                # It exists, so we replace it with our desired line.
                sed -i "s|^\(\s*config\.color_scheme\s*=\s*\).*$|\1\"Noctalia\"|" "$CONFIG_FILE"
            else
                # It doesn't exist, so we add it before the 'return config' line.
                if grep -q '^\s*return\s*config' "$CONFIG_FILE"; then
                    # 'return config' exists. Insert the line before it.
                    sed -i '/^\s*return\s*config/i\'"$WEZTERM_SCHEME_LINE" "$CONFIG_FILE"
                else
                    # This is a problem. We can't find the insertion point.
                    echo "Warning: 'config.color_scheme' not set and 'return config' line not found." >&2
                    echo "         Make sure $CONFIG_FILE is correct: https://wezterm.org/config/files.html" >&2
                fi
            fi
        fi
        # touching the config file fools wezterm into reloading it
        touch "$CONFIG_FILE"
    else
        echo "Error: wezterm.lua not found at $CONFIG_FILE" >&2
        echo "Instructions to create it: https://wezterm.org/config/files.html" >&2
        exit 1
    fi
    ;;

fuzzel)
    CONFIG_FILE="$HOME/.config/fuzzel/fuzzel.ini"

    # Check if the config file exists, create it if it doesn't.
    if [ ! -f "$CONFIG_FILE" ]; then
        # Create the config directory if it doesn't exist
        mkdir -p "$(dirname "$CONFIG_FILE")"
        # Create the config file with the noctalia theme
        cat >"$CONFIG_FILE" <<'EOF'
include=~/.config/fuzzel/themes/noctalia
EOF
    else
        # Check if theme is already set to noctalia
        if grep -q "^include=~/.config/fuzzel/themes/noctalia$" "$CONFIG_FILE"; then
            : # Already correct
        elif grep -q "^include=.*themes" "$CONFIG_FILE"; then
            # Replace existing theme include line in-place
            sed -i 's|^include=.*themes.*|include=~/.config/fuzzel/themes/noctalia|' "$CONFIG_FILE"
        else
            # Add the new theme include line
            echo "include=~/.config/fuzzel/themes/noctalia" >>"$CONFIG_FILE"
        fi
    fi
    ;;

walker)
    CONFIG_FILE="$HOME/.config/walker/config.toml"

    # Check if the config file exists.
    if [ -f "$CONFIG_FILE" ]; then
        # Check if theme is already set to noctalia (flexible spacing)
        if grep -qE '^theme\s*=\s*"noctalia"' "$CONFIG_FILE"; then
            : # Already correct
        elif grep -qE '^theme\s*=' "$CONFIG_FILE"; then
            # Replace existing theme line in-place
            sed -i -E 's/^theme\s*=.*/theme = "noctalia"/' "$CONFIG_FILE"
        else
            echo 'theme = "noctalia"' >>"$CONFIG_FILE"
        fi
    else
        echo "Error: walker config file not found at $CONFIG_FILE" >&2
        exit 1
    fi
    ;;

vicinae)
    # Apply the theme
    vicinae theme set noctalia
    ;;

pywalfox)
    # Set dark/light mode first if MODE is specified
    if [ -n "$MODE" ]; then
        if [ "$MODE" = "dark" ] || [ "$MODE" = "light" ]; then
            pywalfox "$MODE"
        else
            echo "Warning: Invalid mode '$MODE'. Expected 'dark' or 'light'. Skipping mode switch." >&2
        fi
    fi
    # Update the theme
    pywalfox update
    ;;

cava)
    CONFIG_FILE="$HOME/.config/cava/config"
    THEME_MODIFIED=false

    # Check if the config file exists.
    if [ -f "$CONFIG_FILE" ]; then
        # Check if [color] section exists
        if grep -q '^\[color\]' "$CONFIG_FILE"; then
            # Check if theme is already set to noctalia under [color] (flexible spacing)
            if sed -n '/^\[color\]/,/^\[/p' "$CONFIG_FILE" | grep -qE '^theme\s*=\s*"noctalia"'; then
                : # Already correct
            elif sed -n '/^\[color\]/,/^\[/p' "$CONFIG_FILE" | grep -qE '^theme\s*='; then
                # Replace existing theme line under [color]
                sed -i -E '/^\[color\]/,/^\[/{s/^theme\s*=.*/theme = "noctalia"/}' "$CONFIG_FILE"
                THEME_MODIFIED=true
            else
                # Add theme line after [color]
                sed -i '/^\[color\]/a theme = "noctalia"' "$CONFIG_FILE"
                THEME_MODIFIED=true
            fi
        else
            # Add [color] section with theme at the end of file
            echo "" >>"$CONFIG_FILE"
            echo "[color]" >>"$CONFIG_FILE"
            echo 'theme = "noctalia"' >>"$CONFIG_FILE"
            THEME_MODIFIED=true
        fi

        # Reload cava if it's running, but only if it's not using stdin config
        if pgrep -f cava >/dev/null; then
            # Check if Cava is running with -p /dev/stdin (standalone cava)
            if ! pgrep -af cava | grep -q -- "-p.*stdin"; then
                pkill -USR1 cava
            fi
        fi
    else
        echo "Error: cava config file not found at $CONFIG_FILE" >&2
        exit 1
    fi
    ;;

yazi)
    CONFIG_FILE="$HOME/.config/yazi/theme.toml"

    # Create config directory if it doesn't exist
    mkdir -p "$(dirname "$CONFIG_FILE")"

    if [ ! -f "$CONFIG_FILE" ]; then
        cat >"$CONFIG_FILE" <<'EOF'
[flavor]
dark  = "noctalia"
light = "noctalia"
EOF
    else
        # Check if [flavor] section exists
        if grep -q '^\[flavor\]' "$CONFIG_FILE"; then
            # Update or add dark/light lines under [flavor]
            if sed -n '/^\[flavor\]/,/^\[/p' "$CONFIG_FILE" | grep -q '^dark\s*='; then
                sed -i '/^\[flavor\]/,/^\[/{s/^dark\s*=.*/dark  = "noctalia"/}' "$CONFIG_FILE"
            else
                sed -i '/^\[flavor\]/a dark  = "noctalia"' "$CONFIG_FILE"
            fi
            if sed -n '/^\[flavor\]/,/^\[/p' "$CONFIG_FILE" | grep -q '^light\s*='; then
                sed -i '/^\[flavor\]/,/^\[/{s/^light\s*=.*/light = "noctalia"/}' "$CONFIG_FILE"
            else
                sed -i '/^\[flavor\]/,/^dark/a light = "noctalia"' "$CONFIG_FILE"
            fi
        else
            # Add [flavor] section at the end
            echo "" >>"$CONFIG_FILE"
            echo "[flavor]" >>"$CONFIG_FILE"
            echo 'dark  = "noctalia"' >>"$CONFIG_FILE"
            echo 'light = "noctalia"' >>"$CONFIG_FILE"
        fi
    fi
    ;;

labwc)
    # Update the theme
    labwc -r
    ;;

niri)
    CONFIG_FILE="$HOME/.config/niri/config.kdl"
    INCLUDE_LINE='include "./noctalia.kdl"'

    # Check if the config file exists.
    if [ ! -f "$CONFIG_FILE" ]; then
        mkdir -p "$(dirname "$CONFIG_FILE")"
        echo -e "\n$INCLUDE_LINE\n" >"$CONFIG_FILE"
    else
        # Check if noctalia include already exists (flexible: quotes, ./ prefix)
        if grep -qE 'include\s+["'"'"'](\./)?noctalia\.kdl["'"'"']' "$CONFIG_FILE"; then
            : # Already included
        else
            # Add the include line to the end of the file
            echo -e "\n$INCLUDE_LINE\n" >>"$CONFIG_FILE"
        fi
    fi
    ;;

hyprland)
    echo "🎨 Applying 'noctalia' theme to Hyprland..."
    CONFIG_DIR="$HOME/.config/hypr"

    CONF_CONFIG_FILE="$CONFIG_DIR/hyprland.conf"
    LUA_CONFIG_FILE="$CONFIG_DIR/hyprland.lua"

    CONF_THEME_FILE="$CONFIG_DIR/noctalia/noctalia-colors.conf"
    LUA_THEME_FILE="$CONFIG_DIR/noctalia/noctalia-colors.lua"

    CONF_INCLUDE_LINE="source = $CONF_THEME_FILE"
    LUA_INCLUDE_LINE="dofile(\"$LUA_THEME_FILE\")"

    # Prefer Lua config when present. .conf file is kept
    # only for legacy compatibility
    if [ -f "$LUA_CONFIG_FILE" ]; then
      if grep -qF 'noctalia-colors.lua' "$LUA_CONFIG_FILE"; then
        echo "Lua theme already included, skipping modification."
      else
        if [ -L "$LUA_CONFIG_FILE" ] && [ ! -w "$LUA_CONFIG_FILE" ]; then
          echo "Detected read-only symlink, converting to local file..."
          cp --remove-destination "$(readlink -f "$LUA_CONFIG_FILE")" "$LUA_CONFIG_FILE"
          chmod +w "$LUA_CONFIG_FILE"
        fi

        printf "\n%s\n%s\n" \
          "-- This loads Noctalia-generated Hyprland colors." \
          "$LUA_INCLUDE_LINE" >> "$LUA_CONFIG_FILE"

        echo "Added Noctalia Lua theme include to config."
      fi

    else
      # Existing hyprlang behavior preserved for legacy users.
      if [ ! -f "$CONF_CONFIG_FILE" ]; then
        echo "Config file not found, creating $CONF_CONFIG_FILE..."
        mkdir -p "$(dirname "$CONF_CONFIG_FILE")"
        printf "\n%s\n" "$CONF_INCLUDE_LINE" > "$CONF_CONFIG_FILE"
        echo "Created new config file with noctalia theme."
      else
        if grep -qE 'source\s*=\s*.*noctalia.*\.conf' "$CONF_CONFIG_FILE"; then
          echo "Theme already included, skipping modification."
        else
          if [ -L "$CONF_CONFIG_FILE" ] && [ ! -w "$CONF_CONFIG_FILE" ]; then
            echo "Detected read-only symlink, converting to local file..."
            cp --remove-destination "$(readlink -f "$CONF_CONFIG_FILE")" "$CONF_CONFIG_FILE"
            chmod +w "$CONF_CONFIG_FILE"
          fi

          printf "\n%s\n" "$CONF_INCLUDE_LINE" >> "$CONF_CONFIG_FILE"
          echo "Added noctalia theme include to config."
        fi
      fi
    fi

    # Reload hyprland
    hyprctl reload
    ;;

sway)
    echo "🎨 Applying 'noctalia' theme to Sway..."
    CONFIG_DIR="$HOME/.config/sway"
    CONFIG_FILE="$CONFIG_DIR/config"
    INCLUDE_LINE='include ~/.config/sway/noctalia'

    # Check if the config file exists.
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Config file not found, creating $CONFIG_FILE..."
        mkdir -p "$(dirname "$CONFIG_FILE")"
        echo -e "\n$INCLUDE_LINE\n" >"$CONFIG_FILE"
        echo "Created new config file with noctalia theme."
    else
        # Check if noctalia include already exists (flexible matching)
        if grep -qE 'include\s+.*noctalia' "$CONFIG_FILE"; then
            echo "Theme already included, skipping modification."
        else
            # Only convert symlink when we actually need to write (NixOS read-only symlinks)
            if [ -L "$CONFIG_FILE" ] && [ ! -w "$CONFIG_FILE" ]; then
                echo "Detected read-only symlink, converting to local file..."
                cp --remove-destination "$(readlink -f "$CONFIG_FILE")" "$CONFIG_FILE"
                chmod +w "$CONFIG_FILE"
            fi
            # Add the include line to the end of the file
            echo -e "\n$INCLUDE_LINE\n" >>"$CONFIG_FILE"
            echo "✅ Added noctalia theme include to config."
        fi
    fi

    # Reload sway
    swaymsg reload
    ;;

scroll)
    echo "Applying 'noctalia' theme to Scroll..."
    CONFIG_DIR="$HOME/.config/scroll"
    CONFIG_FILE="$CONFIG_DIR/config"
    INCLUDE_LINE='include ~/.config/scroll/noctalia'

    # Check if the config file exists.
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Config file not found, creating $CONFIG_FILE..."
        mkdir -p "$(dirname "$CONFIG_FILE")"
        echo -e "\n$INCLUDE_LINE\n" >"$CONFIG_FILE"
        echo "Created new config file with noctalia theme."
    else
        # Check if noctalia include already exists (flexible matching)
        if grep -qE 'include\s+.*noctalia' "$CONFIG_FILE"; then
            echo "Theme already included, skipping modification."
        else
            # Only convert symlink when we actually need to write
            if [ -L "$CONFIG_FILE" ] && [ ! -w "$CONFIG_FILE" ]; then
                echo "Detected read-only symlink, converting to local file..."
                cp --remove-destination "$(readlink -f "$CONFIG_FILE")" "$CONFIG_FILE"
                chmod +w "$CONFIG_FILE"
            fi
            # Add the include line to the end of the file
            echo -e "\n$INCLUDE_LINE\n" >>"$CONFIG_FILE"
            echo "Added noctalia theme include to config."
        fi
    fi

    # Reload scroll
    scrollmsg reload
    ;;

mango)
    CONFIG_DIR="$HOME/.config/mango"
    MAIN_CONFIG="$CONFIG_DIR/config.conf"
    THEME_FILE="$CONFIG_DIR/noctalia.conf"
    BACKUP_FILE="$CONFIG_DIR/theme.conf.bak"
    # This sources the noctalia theme file
    SOURCE_LINE="source = ~${THEME_FILE#$HOME}"

    # Color variables that should be moved to theme file
    COLOR_VARS="shadowscolor|rootcolor|bordercolor|focuscolor|maximizescreencolor|urgentcolor|scratchpadcolor|globalcolor|overlaycolor"

    # Create config directory if it doesn't exist
    mkdir -p "$CONFIG_DIR"

    # Check if theme is already sourced in main config (matches $HOME, ~, or expanded paths)
    if [ -f "$MAIN_CONFIG" ] && grep -qE 'source\s*=\s*.*noctalia\.conf' "$MAIN_CONFIG"; then
        : # Theme already set
    else
        # First-time setup: backup and remove legacy color definitions

        # Scan all .conf files in config directory for legacy color variables
        for conf_file in "$CONFIG_DIR"/*.conf; do
            # Skip if no .conf files exist or if it's the theme file itself
            [ -e "$conf_file" ] || continue
            [ "$conf_file" = "$THEME_FILE" ] && continue

            # Check if this file contains any color variable definitions
            if grep -qE "^($COLOR_VARS)\s*=" "$conf_file"; then
                # Extract and append color definitions to backup file
                grep -E "^($COLOR_VARS)\s*=" "$conf_file" >>"$BACKUP_FILE"

                # Remove color definitions from original file
                if [ -L "$conf_file" ] && [ ! -w "$conf_file" ]; then
                    # Read-only symlink (e.g. NixOS): convert to local file
                    cp --remove-destination "$(readlink -f "$conf_file")" "$conf_file"
                    chmod +w "$conf_file"
                    sed -i -E "/^($COLOR_VARS)\s*=/d" "$conf_file"
                else
                    # Edit the real file, preserving any writable symlink
                    sed -i -E "/^($COLOR_VARS)\s*=/d" "$(readlink -f "$conf_file")"
                fi
            fi
        done

        # Only convert symlink when we actually need to write
        if [ -L "$MAIN_CONFIG" ] && [ ! -w "$MAIN_CONFIG" ]; then
            echo "Detected read-only symlink, converting to local file..."
            cp --remove-destination "$(readlink -f "$MAIN_CONFIG")" "$MAIN_CONFIG"
            chmod +w "$MAIN_CONFIG"
        fi

        # Add source line to main config
        if [ -f "$MAIN_CONFIG" ]; then
            echo "" >>"$MAIN_CONFIG"
            echo "# This sources the noctalia theme" >>"$MAIN_CONFIG"
            echo -e "\n$SOURCE_LINE\n" >>"$MAIN_CONFIG"
        else
            echo "# This sources the noctalia theme" >"$MAIN_CONFIG"
            echo -e "\n$SOURCE_LINE\n" >>"$MAIN_CONFIG"
        fi
    fi

    # Trigger live reload
    if command -v mmsg >/dev/null 2>&1; then
        mmsg -s -d reload_config
    else
        echo "Warning: mmsg command not found, manual restart may be needed." >&2
    fi
    ;;

btop)
    CONFIG_FILE="$HOME/.config/btop/btop.conf"

    if [ -f "$CONFIG_FILE" ]; then
        # Check if theme is already set to noctalia (flexible spacing)
        if grep -qE '^color_theme\s*=\s*"noctalia"' "$CONFIG_FILE"; then
            : # Already correct
        elif grep -qE '^color_theme\s*=' "$CONFIG_FILE"; then
            # Replace existing color_theme line in-place
            sed -i -E 's/^color_theme\s*=.*/color_theme = "noctalia"/' "$CONFIG_FILE"
        else
            echo 'color_theme = "noctalia"' >>"$CONFIG_FILE"
        fi

        if pgrep -x btop >/dev/null; then
            pkill -SIGUSR2 -x btop
        fi
    else
        echo "Warning: btop config file not found at $CONFIG_FILE" >&2
    fi
    ;;

zathura)
    ZATHURA_INSTANCES=$(dbus-send --session \
        --dest=org.freedesktop.DBus \
        --type=method_call \
        --print-reply \
        /org/freedesktop/DBus \
        org.freedesktop.DBus.ListNames |
        grep -o 'org.pwmt.zathura.PID-[0-9]*')

    for id in $ZATHURA_INSTANCES; do
        dbus-send --session \
            --dest="$id" \
            --type=method_call \
            /org/pwmt/zathura \
            org.pwmt.zathura.ExecuteCommand \
            string:"source"
    done
    ;;

starship)
            PALETTE_FILE="$HOME/.cache/noctalia/starship-palette.toml"

            # Respect STARSHIP_CONFIG env var, then fall back to standard lookup order
            if [ -n "$STARSHIP_CONFIG" ]; then
                CONFIG_FILE="$STARSHIP_CONFIG"
            elif [ -f "$HOME/.config/starship.toml" ]; then
                CONFIG_FILE="$HOME/.config/starship.toml"
            elif [ -f "$HOME/.config/starship/starship.toml" ]; then
                CONFIG_FILE="$HOME/.config/starship/starship.toml"
            else
                CONFIG_FILE="$HOME/.config/starship.toml"
            fi

            if [ ! -f "$PALETTE_FILE" ]; then
                echo "Error: Starship palette file not found at $PALETTE_FILE" >&2
                return 1
            fi

            MARKER_BEGIN='# >>> NOCTALIA STARSHIP PALETTE >>>'
            MARKER_END='# <<< NOCTALIA STARSHIP PALETTE <<<'

            # Create config file from scratch if it doesn't exist yet
            if [ ! -f "$CONFIG_FILE" ]; then
                mkdir -p "$(dirname "$CONFIG_FILE")"
                {
                    printf 'palette = "noctalia"\n\n'
                    printf '%s\n' "$MARKER_BEGIN"
                    cat "$PALETTE_FILE"
                    printf '%s\n' "$MARKER_END"
                } > "$CONFIG_FILE"
                return 0
            fi

            # Follow symlinks so we edit the real file (safe for stow / dotfile managers)
            if [ -L "$CONFIG_FILE" ]; then
                CONFIG_FILE="$(readlink -f "$CONFIG_FILE")"
            fi

            # Set or insert top-level  palette = "noctalia"
            if grep -qE '^[[:space:]]*palette[[:space:]]*=' "$CONFIG_FILE"; then
                sed -i -E 's/^([[:space:]]*)palette([[:space:]]*)=.*/\1palette\2= "noctalia"/' "$CONFIG_FILE"
            elif grep -qE '^[[:space:]]*"\$schema"' "$CONFIG_FILE"; then
                sed -i '/^[[:space:]]*"\$schema"/a palette = "noctalia"' "$CONFIG_FILE"
            else
                sed -i '1i palette = "noctalia"' "$CONFIG_FILE"
            fi

            # Remove existing palette block using awk for literal string matching
            # (avoids sed misinterpreting >, #, or other chars in the markers as regex)
            if grep -qF "$MARKER_BEGIN" "$CONFIG_FILE"; then
                awk -v begin="$MARKER_BEGIN" -v end="$MARKER_END" '
                    $0 == begin { skip = 1; next }
                    $0 == end   { skip = 0; next }
                    !skip
                ' "$CONFIG_FILE" > "${CONFIG_FILE}.noctalia.tmp" \
                    && mv "${CONFIG_FILE}.noctalia.tmp" "$CONFIG_FILE"
            fi

            # Append fresh palette block, ensuring a clean newline boundary
            {
                printf '\n%s\n' "$MARKER_BEGIN"
                cat "$PALETTE_FILE"
                # Guard: ensure palette file ends with newline before closing marker
                tail -c1 "$PALETTE_FILE" | grep -q $'\n' || printf '\n'
                printf '%s\n' "$MARKER_END"
            } >> "$CONFIG_FILE"
            ;;

*)
    # Handle unknown application names.
    echo "Error: Unknown application '$APP_NAME'." >&2
    exit 1
    ;;
esac
