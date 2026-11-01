#!/usr/bin/env bash

# Theme integration must preserve config discovery and still update existing configs.
set -euo pipefail

templates_dir=${1:?templates directory}

fail() {
  printf '%s\n' "compositor_template_config_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

mkdir -p "$work_dir/bin"
cat >"$work_dir/bin/hyprctl" <<'EOF'
#!/bin/sh
if [ "$TEST_HYPRLAND_MODE" = lua ]; then
  printf 'ok\n'
else
  exit 1
fi
EOF
cat >"$work_dir/bin/mmsg" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$work_dir/bin/hyprctl" "$work_dir/bin/mmsg"
export PATH="$work_dir/bin:$PATH"

configure_case() {
  local name=$1
  case_dir="$work_dir/$name"
  export HOME="$case_dir/home"
  export XDG_CONFIG_HOME="$case_dir/config"
  export XDG_DATA_HOME="$case_dir/data"
  export XDG_CONFIG_DIRS="$case_dir/system"
  export TEST_HYPRLAND_MODE=conf

  compositor=$name
  case "$name" in
    umbriel)
      primary_relative=umbriel/config.toml
      theme_relative=umbriel/noctalia.toml
      integration='files = ["noctalia.toml"]'
      manual_directive="files = [\"$XDG_CONFIG_HOME/$theme_relative\"]"
      ;;
    niri)
      primary_relative=niri/config.kdl
      theme_relative=niri/noctalia.kdl
      integration='include "noctalia.kdl"'
      manual_directive="include \"$XDG_CONFIG_HOME/$theme_relative\""
      ;;
    sway|scroll)
      primary_relative="$name/config"
      theme_relative="$name/noctalia"
      integration="include $XDG_CONFIG_HOME/$theme_relative"
      manual_directive="include \"$XDG_CONFIG_HOME/$theme_relative\""
      ;;
    mango)
      primary_relative=mango/config.conf
      theme_relative=mango/noctalia.conf
      integration="source=$XDG_CONFIG_HOME/$theme_relative"
      manual_directive=$integration
      ;;
    hyprland-conf)
      compositor=hyprland
      primary_relative=hypr/hyprland.conf
      theme_relative=hypr/noctalia.conf
      integration="source = $XDG_CONFIG_HOME/$theme_relative"
      manual_directive=$integration
      ;;
    hyprland-lua)
      compositor=hyprland
      export TEST_HYPRLAND_MODE=lua
      primary_relative=hypr/hyprland.lua
      theme_relative=hypr/noctalia.lua
      integration='require("noctalia").apply_theme()'
      manual_directive="dofile(\"$XDG_CONFIG_HOME/$theme_relative\").apply_theme()"
      ;;
    labwc)
      primary_relative=labwc/rc.xml
      theme_relative=labwc/noctalia.conf
      integration='<name>noctalia</name>'
      manual_directive=$integration
      ;;
  esac

  primary="$XDG_CONFIG_HOME/$primary_relative"
  theme="$XDG_CONFIG_HOME/$theme_relative"
  system_config="$XDG_CONFIG_DIRS/$primary_relative"
  mkdir -p "$HOME/.i3" "$(dirname "$primary")" "$(dirname "$system_config")"
  printf 'system configuration must survive\n' >"$system_config"
  cp "$system_config" "$case_dir/system-before"
  printf 'generated theme\n' >"$theme"
}

run_hook() {
  bash "$templates_dir/$compositor/$1.sh" >"$case_dir/stdout" 2>"$case_dir/stderr"
}

expect_missing_config() {
  local status=0
  run_hook apply || status=$?
  [ "$status" -ne 0 ] || fail "$name: missing config reported success"
  [ ! -e "$primary" ] || fail "$name: created missing config or dangling symlink target"
  grep -qF "$primary" "$case_dir/stderr" \
    || fail "$name: diagnostic does not identify missing config"
  grep -qF "$manual_directive" "$case_dir/stderr" \
    || fail "$name: diagnostic does not show manual theme integration"
  if [ "$name" = umbriel ]; then
    grep -qF '[include.optional]' "$case_dir/stderr" \
      || fail "$name: diagnostic omits optional include section"
  fi
}

for name in umbriel niri sway scroll mango hyprland-conf hyprland-lua labwc; do
  configure_case "$name"

  expect_missing_config
  printf 'alternative configuration must survive\n' >"$HOME/.i3/config"
  cp "$HOME/.i3/config" "$case_dir/alternative-before"
  expect_missing_config
  [ ! -L "$primary" ] || fail "$name: created primary config symlink"
  [ -f "$theme" ] || fail "$name: removed generated theme on missing config"
  if [ "$name" = labwc ]; then
    cmp -s "$theme" "$XDG_DATA_HOME/themes/noctalia/openbox-3/themerc" \
      || fail "$name: did not install generated theme before warning"
  fi
  run_hook undo || fail "$name: undo failed with missing config"
  [ ! -e "$primary" ] && [ ! -L "$primary" ] \
    || fail "$name: undo created missing config"

  dangling_target="$case_dir/missing-target"
  ln -s "$dangling_target" "$primary"
  expect_missing_config
  run_hook undo || fail "$name: undo failed with dangling config symlink"
  [ ! -e "$dangling_target" ] || fail "$name: created dangling symlink target"
  [ -L "$primary" ] && [ "$(readlink "$primary")" = "$dangling_target" ] \
    || fail "$name: changed dangling config symlink"
  rm "$primary"

  if [ "$name" = labwc ]; then
    printf '<labwc_config>\n  <!-- user configuration -->\n  <theme>\n    <name>existing</name>\n  </theme>\n</labwc_config>\n' >"$primary"
  elif [ "$name" = niri ]; then
    printf '// user configuration\n' >"$primary"
  elif [ "$name" = hyprland-lua ]; then
    printf -- '-- user configuration\n' >"$primary"
  else
    printf '# user configuration\n' >"$primary"
  fi
  run_hook apply || fail "$name: apply failed with existing config: $(cat "$case_dir/stderr")"
  [ "$(grep -cF "$integration" "$primary")" = 1 ] \
    || fail "$name: existing config did not get exactly one theme integration"
  grep -qF 'user configuration' "$primary" || fail "$name: lost existing config content"
  cp "$primary" "$case_dir/applied"
  run_hook apply || fail "$name: repeated apply failed"
  cmp -s "$primary" "$case_dir/applied" || fail "$name: repeated apply changed config"

  managed_config="$case_dir/managed-config"
  mv "$primary" "$managed_config"
  ln -s "$managed_config" "$primary"
  run_hook apply || fail "$name: apply failed with configured symlink"
  [ -L "$primary" ] && [ "$(readlink "$primary")" = "$managed_config" ] \
    || fail "$name: replaced configured symlink"
  cmp -s "$managed_config" "$case_dir/applied" \
    || fail "$name: changed already-configured symlink target"
  cmp -s "$system_config" "$case_dir/system-before" || fail "$name: changed system config"
  cmp -s "$HOME/.i3/config" "$case_dir/alternative-before" || fail "$name: changed alternative config"
done
