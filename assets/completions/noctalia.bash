_NOCTALIA_TOP_COMMANDS="\
    --help \
    -h \
    --version \
    -v \
    --daemon \
    -d \
    completions \
    config \
    msg \
    plugins \
    theme"

_NOCTALIA_MSG_COMMANDS="\
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
    workspace-switch"

_NOCTALIA_BOOL_STATES="0 1 false off on true"

_NOCTALIA_WALLPAPER_SCHEMES="\
    dysfunctional \
    faithful \
    m3-content \
    m3-fruit-salad \
    m3-monochrome \
    m3-rainbow \
    m3-tonal-spot \
    muted \
    soft \
    vibrant"

_NOCTALIA_THEME_OPTS="\
    --both \
    --builtin-config \
    --dark \
    --default-mode \
    --help \
    --light \
    --list-templates \
    --pure-black \
    --scheme \
    --theme-json \
    -c \
    -o \
    -r"

declare -a _NOCTALIA_BUILTIN_PALETTES=(
    Ayu
    Catppuccin
    Dracula
    Eldritch
    Gruvbox
    Kanagawa
    Noctalia
    Nord
    "Rosé\ Pine"
    Tokyo-Night
)

# set NOCTALIA_COMP_DEBUG_FILE to debug
__noctalia_debug() {
    local file="$NOCTALIA_COMP_DEBUG_FILE"
    if [[ -n ${file} ]]; then
        echo "$*" >> "${file}"
    fi
}

# helper
_complete_words() {
    COMPREPLY=( $(compgen -W "$1" -- "$cur") )
}

_noctalia_get_plugin_sources() {
    # grab everything up to the first whitespace
    noctalia msg plugins source list 2>/dev/null | awk '{print $1}'
}

_noctalia_get_enabled_plugins() {
    # grab everything up to the first whitespace
    noctalia msg plugins list 2>/dev/null | grep 'enabled$' | awk '{print $1}'
}

_noctalia_get_disabled_plugins() {
    # grab everything up to the first whitespace
    noctalia msg plugins list 2>/dev/null | grep 'disabled$' | awk '{print $1}'
}

# `noctalia <TAB>` completions
_noctalia_completions() {
    local cur prev first_cmd second_cmd

    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    first_cmd="${COMP_WORDS[1]}"
    second_cmd="${COMP_WORDS[2]}"

    __noctalia_debug "Bash debug: cur='$cur', prev='$prev', COMP_CWORD=$COMP_CWORD"

    if [[ ${COMP_CWORD} -eq 1 ]]; then
        _complete_words "$_NOCTALIA_TOP_COMMANDS"
        return 0
    fi

    case "${first_cmd}" in
        completions)
            if [[ ${COMP_CWORD} -eq 2 ]]; then
                _complete_words "bash fish zsh"
            fi
            ;;
        config)
            _noctalia_config
            ;;
        msg)
            _noctalia_msg
            ;;
        plugins)
            _noctalia_plugins
            ;;
        theme)
            _noctalia_theme
            ;;
    esac

    return 0
}

# `noctalia config <TAB>` completions
_noctalia_config() {
    if [[ ${COMP_CWORD} -eq 2 ]]; then
        _complete_words "--help export replay-report settings-count validate"
        return 0
    fi

    if [[ "$cur" == -* ]]; then
        if [[ "${second_cmd}" == "replay-report" ]]; then
            _complete_words "--flattened --force --help --target"
        else
            _complete_words "--help"
        fi
        return 0
    fi

    case "${second_cmd}" in
        export)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "full merged"
            fi
            ;;
        replay-report|validate)
            if [[ ${COMP_CWORD} -eq 3 ]] && [[ "$cur" != -* ]]; then
                COMPREPLY=( $(compgen -f -- "$cur") )
            fi
            ;;
    esac
}

# `noctalia msg <TAB>` completions
_noctalia_msg() {
    local output available panels IFS

    if [[ ${COMP_CWORD} -eq 2 ]]; then
        _complete_words "$_NOCTALIA_MSG_COMMANDS"
        return 0
    fi

    if [[ "$cur" == -* ]]; then
        _complete_words "--help"
        return 0
    fi

    case "${second_cmd}" in
        bar-auto-hide-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "$_NOCTALIA_BOOL_STATES smart"
            fi
            ;;
        bar-layer-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "overlay top"
            fi
            ;;
        color-scheme-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "builtin community custom wallpaper"
            elif [[ ${COMP_CWORD} -eq 4 ]]; then
                if [[ "${prev}" == "builtin" ]]; then
                    COMPREPLY=()
                    for palette in "${_NOCTALIA_BUILTIN_PALETTES[@]}"; do
                        if [[ "$palette" == "$cur"* ]]; then
                            COMPREPLY+=("$palette")
                        fi
                    done
                elif [[ "${prev}" == "wallpaper" ]]; then
                    _complete_words "$_NOCTALIA_WALLPAPER_SCHEMES"
                fi
            fi
            ;;
        log-level-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "debug error info warn"
            fi
            ;;
        media)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "next next-player pause play previous previous-player stop toggle"
            fi
            ;;
        notification-dnd-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "$_NOCTALIA_BOOL_STATES"
            fi
            ;;
        panel-close|panel-open|panel-toggle)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                # run `msg panel-toggle` with dummy panel option
                output=$(noctalia msg panel-toggle '[' 2>&1)
                available=${output#*available: }
                available=${available%)}
                panels=${available//,/}
                _complete_words "$panels"
            fi
            ;;
        plugin)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "$(_noctalia_get_enabled_plugins)"
            elif [[ ${COMP_CWORD} -eq 4 ]]; then
                _complete_words "all focused"
            fi
            ;;
        plugins)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "disable enable list source update"
            else
                case "${COMP_WORDS[3]}" in
                    disable)
                        if [[ ${COMP_CWORD} -eq 4 ]]; then
                            _complete_words "$(_noctalia_get_enabled_plugins)"
                        fi
                        ;;
                    enable)
                        if [[ ${COMP_CWORD} -eq 4 ]]; then
                            _complete_words "$(_noctalia_get_disabled_plugins)"
                        fi
                        ;;
                    source)
                        if [[ ${COMP_CWORD} -eq 4 ]]; then
                            _complete_words "add list remove"
                        elif [[ ${COMP_CWORD} -eq 5 ]] && [[ "${COMP_WORDS[4]}" == "remove" ]]; then
                            _complete_words "$(_noctalia_get_plugin_sources)"
                        elif [[ ${COMP_CWORD} -eq 6 ]] && [[ "${COMP_WORDS[4]}" == "add" ]]; then
                            _complete_words "git path"
                        fi
                        ;;
                    update)
                        if [[ ${COMP_CWORD} -eq 4 ]]; then
                            _complete_words "$(_noctalia_get_plugin_sources)"
                        fi
                        ;;
                esac
            fi
            ;;
        power-cycle|taskbar-cycle|workspace-switch)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "next prev"
            fi
            ;;
        power-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "balanced performance power-saver"
            fi
            ;;
        screenshot-fullscreen)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "all monitor pick"
            fi
            ;;
        session)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "lock lock-and-suspend logout reboot shutdown suspend"
            fi
            ;;
        theme-mode-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                _complete_words "auto dark light"
            fi
            ;;
        wallpaper-set)
            if [[ ${COMP_CWORD} -eq 3 ]]; then
                COMPREPLY=( $(compgen -f -- "$cur") )
            fi
            ;;
    esac
}

# `noctalia plugins <TAB>` completions
_noctalia_plugins() {
    if [[ ${COMP_CWORD} -eq 2 ]]; then
        _complete_words "--help lint"
        return 0
    fi

    if [[ "$cur" == -* ]]; then
        _complete_words "--help"
        return 0
    fi

    if [[ "${second_cmd}" == "lint" ]] && [[ ${COMP_CWORD} -ge 3 ]]; then
        COMPREPLY=( $(compgen -d -- "$cur") )
    fi
}

# `noctalia theme <TAB>` completions
_noctalia_theme() {
    if [[ "$cur" == -* ]]; then
        _complete_words "$_NOCTALIA_THEME_OPTS"
        return 0
    fi

    if [[ "${prev}" == "--scheme" ]]; then
        _complete_words "$_NOCTALIA_WALLPAPER_SCHEMES"
        return 0
    fi

    if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -f -- "$cur") )
    fi
}

complete -o default -F _noctalia_completions noctalia
