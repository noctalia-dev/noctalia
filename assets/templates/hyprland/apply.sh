#!/usr/bin/env bash
set -euo pipefail

# Hyprland has two config modes as of v0.55, hyprlang and lua
#
# The template engine calls this script in three modes:
#   apply.sh input -> prints which template file to render
#   apply.sh output -> prints where the rendered file should be written
#   apply.sh apply -> updates user's hyprland config to load the file

command="${1:-apply}"

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
lua_config_file="$config_dir/hyprland.lua"
conf_config_file="$config_dir/hyprland.conf"

lua_output_file="$config_dir/noctalia.lua"
conf_output_file="$config_dir/noctalia.conf"

# hyprland expands ~ in source paths; tilde the include for portability while
# the rendered file is still written to the real conf_output_file path.
if [[ "$config_dir" == "$HOME"/* ]]; then
  conf_source_path="~/${config_dir#"$HOME"/}/noctalia.conf"
else
  conf_source_path="$conf_output_file"
fi

detect_mode() {
  # Prefer probing the runner compositor when available
  if command -v hyprctl >/dev/null 2>&1 &&
    hyprctl dispatch 'hl.dsp.no_op()' 2>/dev/null | grep -qx 'ok'; then
    printf 'lua\n'
    return
  fi

  # Fallback for cases where noctalia applies templates where hyprland
  # is not reachable. If the user has hyprland.lua, assume Lua config mode.
  if [ -f "$lua_config_file" ]; then
    printf 'lua\n'
  else
    printf 'conf\n'
  fi
}

apply_lua() {
  local include_line='-- For Noctalia Color templates
require("noctalia").apply_theme()'

  mkdir -p "$config_dir"

  if [ ! -f "$lua_config_file" ]; then
    printf '%s\n' "$include_line" >"$lua_config_file"
    return
  fi

  # Migrate the legacy bare include to the apply_theme() form.
  # Only matches a line that is *exactly* require("noctalia"), so user code
  # like require("noctalia").colors.primary is left untouched.
  sed -i 's/^[[:space:]]*require("noctalia")[[:space:]]*$/require("noctalia").apply_theme()/' "$lua_config_file"

  # Append only if there is no Noctalia include at all
  if ! grep -qF 'require("noctalia")' "$lua_config_file"; then
    printf '\n%s\n' "$include_line" >>"$lua_config_file"
  fi
}

apply_conf() {
  local include_line="# For Noctalia Color templates
source = $conf_source_path"

  mkdir -p "$config_dir"

  if [ ! -f "$conf_config_file" ]; then
    printf '%s\n' "$include_line" >"$conf_config_file"
    return
  fi

  # Avoid appending duplicate Noctalia includes
  if ! grep -qE 'source\s*=.*noctalia\.conf' "$conf_config_file"; then
    printf '\n%s\n' "$include_line" >>"$conf_config_file"
  fi
}

mode="$(detect_mode)"

case "$command" in
  input)
    # Used by input_path_dynamic
    if [ "$mode" = "lua" ]; then
      printf './hyprland/hyprland.lua\n'
    else
      printf './hyprland/hyprland.conf\n'
    fi
    ;;

  output)
    # Used by output_path_dynamic
    if [ "$mode" = "lua" ]; then
      printf '%s\n' "$lua_output_file"
    else
      printf '%s\n' "$conf_output_file"
    fi
    ;;

  apply)
    # Used by post_hook after the selected template has been rendered
    if [ "$mode" = "lua" ]; then
      apply_lua
    else
      apply_conf
    fi
    ;;

  detect)
    # Debug mode for convenience
    detect_mode
    ;;

  *)
    echo "Usage: $0 {input|output|apply|detect}" >&2
    exit 1
    ;;
esac
