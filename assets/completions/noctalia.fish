complete -c noctalia -f

set -l TOP_CMDS completions config msg plugins theme

set -l CONFIG_CMDS export replay-report settings-count validate

set -l MSG_CMDS \
    --help \
    bar-auto-hide-set \
    bar-hide \
    bar-layer-set \
    bar-reserve-toggle \
    bar-show \
    bar-toggle \
    bluetooth-disable \
    bluetooth-enable \
    bluetooth-status \
    bluetooth-toggle \
    brightness-down \
    brightness-list-backlight-devices \
    brightness-osd \
    brightness-set \
    brightness-up \
    caffeine-disable \
    caffeine-enable \
    caffeine-toggle \
    clipboard-clear \
    clipboard-copy \
    clipboard-text \
    color-scheme-get \
    color-scheme-set \
    config-reload \
    desktop-widgets-edit \
    desktop-widgets-exit \
    desktop-widgets-hide \
    desktop-widgets-show \
    desktop-widgets-toggle \
    desktop-widgets-toggle-edit \
    dock-hide \
    dock-reload \
    dock-show \
    dock-toggle \
    dpms-off \
    dpms-on \
    effects-profile-set \
    greeter-sync \
    keyboard-backlight-down \
    keyboard-backlight-osd \
    keyboard-backlight-set \
    keyboard-backlight-toggle \
    keyboard-backlight-up \
    keyboard-layout-cycle \
    lockscreen-widgets-edit \
    lockscreen-widgets-exit \
    lockscreen-widgets-toggle-edit \
    log-level-set \
    log-level-status \
    media \
    mic-mute \
    mic-volume-down \
    mic-volume-osd \
    mic-volume-set \
    mic-volume-up \
    network-toggle \
    nightlight-disable \
    nightlight-enable \
    nightlight-force-toggle \
    nightlight-toggle \
    notification-clear-active \
    notification-clear-history \
    notification-dnd-set \
    notification-dnd-status \
    notification-dnd-toggle \
    notification-invoke-latest \
    notification-show \
    osd-disable \
    osd-enable \
    osd-toggle \
    panel-close \
    panel-open \
    panel-toggle \
    plugin \
    plugins \
    power-cycle \
    power-set \
    screenshot-fullscreen \
    screenshot-region \
    session \
    settings-close \
    settings-open \
    settings-open-plugin \
    settings-open-widget \
    settings-toggle \
    status \
    taskbar-cycle \
    templates-apply \
    theme-mode-get \
    theme-mode-set \
    theme-mode-toggle \
    volume-down \
    volume-mute \
    volume-osd \
    volume-set \
    volume-up \
    wallpaper-get \
    wallpaper-next \
    wallpaper-previous \
    wallpaper-random \
    wallpaper-set \
    wifi-disable \
    wifi-enable \
    wifi-status \
    wifi-toggle \
    window-switcher \
    workspace-alert-add \
    workspace-alert-add-window \
    workspace-alert-clear \
    workspace-alert-clear-all \
    workspace-alert-status \
    workspace-switch

set -l BOOL_STATES 0 1 false off on true

set -l BUILTIN_PALETTES \
    Ayu \
    Catppuccin \
    Dracula \
    Eldritch \
    Gruvbox \
    Kanagawa \
    Noctalia \
    Nord \
    "Rosé\ Pine" \
    Tokyo-Night

set -l WALLPAPER_SCHEMES \
    dysfunctional \
    faithful \
    m3-content \
    m3-fruit-salad \
    m3-monochrome \
    m3-rainbow \
    m3-tonal-spot \
    muted \
    soft \
    vibrant

set -l THEME_OPTS \
    -l both \
    -l builtin-config \
    -s c \
    -l dark \
    -l default-mode \
    -l help \
    -l light \
    -l list-templates \
    -s o \
    -l pure-black \
    -s r \
    -l scheme \
    -l theme-json

function __noctalia_get_panels
    # run `msg panel-toggle` with dummy panel option
    set -l output (noctalia msg panel-toggle '[' 2>&1)
    set -l available (string match -r 'available: (.*)\)' $output)[2]
    string split ', ' $available
end

function __noctalia_get_plugin_sources
    # grab everything up to the first whitespace
    noctalia msg plugins source list 2>/dev/null | string replace -r '\s+.*' ''
end

function __noctalia_get_disabled_plugins
    # grab everything up to the first whitespace
    noctalia msg plugins list 2>/dev/null | string match -r "^.* disabled\$" | string split -f1 " "
end

function __noctalia_get_enabled_plugins
    # grab everything up to the first whitespace
    noctalia msg plugins list 2>/dev/null | string match -r "^.* enabled\$" | string split -f1 " "
end

# `noctalia <TAB>` completions
complete -c noctalia -n "not __fish_seen_subcommand_from $TOP_CMDS" -a "$TOP_CMDS"
complete -c noctalia -n "not __fish_seen_subcommand_from $TOP_CMDS" -s d -l daemon
complete -c noctalia -n "not __fish_seen_subcommand_from $TOP_CMDS" -s h -l help
complete -c noctalia -n "not __fish_seen_subcommand_from $TOP_CMDS" -s v -l version

# `noctalia completions <TAB>` completions
complete -c noctalia -n "__fish_seen_subcommand_from completions" -a "bash fish zsh"

# `noctalia config <TAB>` completions
complete -c noctalia -F -n "__fish_seen_subcommand_from config; and __fish_prev_arg_in replay-report validate"
complete -c noctalia -n "__fish_prev_arg_in export" -a "full merged"
complete -c noctalia -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from $CONFIG_CMDS" -a "$CONFIG_CMDS --help"
complete -c noctalia -n "__fish_seen_subcommand_from replay-report" -l flattened -l force -l help -l target

# `noctalia msg <TAB>` completions
complete -c noctalia -F -n "__fish_prev_arg_in add"
complete -c noctalia -F -n "__fish_prev_arg_in wallpaper-set"
complete -c noctalia -n "__fish_prev_arg_in bar-auto-hide-set" -a "$BOOL_STATES smart"
complete -c noctalia -n "__fish_prev_arg_in bar-layer-set" -a "overlay top"
complete -c noctalia -n "__fish_prev_arg_in builtin" -a "$BUILTIN_PALETTES"
complete -c noctalia -n "__fish_prev_arg_in color-scheme-set" -a "builtin community custom wallpaper"
complete -c noctalia -n "__fish_prev_arg_in disable" -a "(__noctalia_get_enabled_plugins)"
complete -c noctalia -n "__fish_prev_arg_in enable" -a "(__noctalia_get_disabled_plugins)"
complete -c noctalia -n "__fish_prev_arg_in log-level-set" -a "debug error info warn"
complete -c noctalia -n "__fish_prev_arg_in media" -a "next next-player pause play previous previous-player stop toggle"
complete -c noctalia -n "__fish_prev_arg_in notification-dnd-set" -a "$BOOL_STATES"
complete -c noctalia -n "__fish_prev_arg_in panel-close panel-open panel-toggle" -a "(__noctalia_get_panels)"
complete -c noctalia -n "__fish_prev_arg_in plugin" -a "(__noctalia_get_enabled_plugins)"
complete -c noctalia -n "__fish_prev_arg_in plugins; and __fish_seen_subcommand_from msg" -a "disable enable list source update"
complete -c noctalia -n "__fish_prev_arg_in power-cycle taskbar-cycle workspace-switch" -a "next prev"
complete -c noctalia -n "__fish_prev_arg_in power-set" -a "balanced performance power-saver"
complete -c noctalia -n "__fish_prev_arg_in remove" -a "(__noctalia_get_plugin_sources)"
complete -c noctalia -n "__fish_prev_arg_in screenshot-fullscreen" -a "all monitor pick"
complete -c noctalia -n "__fish_prev_arg_in session" -a "lock lock-and-suspend logout reboot shutdown suspend"
complete -c noctalia -n "__fish_prev_arg_in source" -a "add list remove"
complete -c noctalia -n "__fish_prev_arg_in theme-mode-set" -a "auto dark light"
complete -c noctalia -n "__fish_prev_arg_in update" -a "(__noctalia_get_plugin_sources)"
complete -c noctalia -n "__fish_prev_arg_in wallpaper; and __fish_seen_subcommand_from color-scheme-set" -a "$WALLPAPER_SCHEMES"
complete -c noctalia -n "__fish_seen_subcommand_from msg; and not __fish_seen_subcommand_from $MSG_CMDS" -a "$MSG_CMDS"
complete -c noctalia -n "__fish_seen_subcommand_from plugin; and not __fish_prev_arg_in plugin" -a "all focused"

# `noctalia plugins <TAB>` completions
complete -c noctalia -F -n "__fish_seen_subcommand_from lint; and __fish_seen_subcommand_from plugins"
complete -c noctalia -n "__fish_seen_subcommand_from plugins; and not __fish_seen_subcommand_from msg lint" -a "lint --help"

# `noctalia theme <TAB>` completions
complete -c noctalia -F -n "__fish_prev_arg_in --theme-json -o -r -c"
complete -c noctalia -n "__fish_prev_arg_in --scheme" -a "$WALLPAPER_SCHEMES"
complete -c noctalia -n "__fish_seen_subcommand_from theme" $THEME_OPTS
