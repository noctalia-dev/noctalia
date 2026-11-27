#!/usr/bin/env bash

# apply.sh must leave a kitty.conf that already includes the theme untouched, timestamps
# included: a Home Manager kitty.conf is a read-only symlink into the Nix store, where
# touching it fails and stops the hook before it reloads kitty. Comparing timestamps
# catches that without a root-owned file. A missing kitty.conf is still created.

set -euo pipefail

apply_sh=${1:?apply.sh path}

fail() {
  printf '%s\n' "kitty_apply_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

mkdir -p "$work_dir/bin"
calls_file="$work_dir/calls"
cat >"$work_dir/bin/pkill" <<EOF
#!/bin/sh
printf 'pkill %s\n' "\$*" >>"$calls_file"
exit 0
EOF
chmod +x "$work_dir/bin/pkill"
export PATH="$work_dir/bin:$PATH"

# Prints the config home of a case with a rendered theme and no kitty.conf.
new_case() {
  local config_home="$work_dir/$1"
  mkdir -p "$config_home/kitty/themes"
  printf 'foreground #ffffff\n' >"$config_home/kitty/themes/noctalia.conf"
  printf '%s\n' "$config_home"
}

run_apply() {
  local name=$1 config_home=$2
  : >"$calls_file"
  XDG_CONFIG_HOME="$config_home" XDG_CONFIG_DIRS="$work_dir/no-system-config" \
    bash "$apply_sh" >/dev/null 2>"$work_dir/stderr" \
    || fail "$name: apply.sh exited non-zero: $(cat "$work_dir/stderr")"
  [ -s "$calls_file" ] || fail "$name: apply.sh did not reload kitty"
}

for layout in file symlink; do
  config_home=$(new_case "$layout")
  config_file="$work_dir/$layout-kitty.conf"
  printf 'font_size 11\ninclude themes/noctalia.conf\n' >"$config_file"
  touch -t 202001010000 "$config_file"
  cp -p "$config_file" "$work_dir/$layout-before"
  if [ "$layout" = symlink ]; then
    ln -s "$config_file" "$config_home/kitty/kitty.conf"
  else
    mv "$config_file" "$config_home/kitty/kitty.conf"
    config_file="$config_home/kitty/kitty.conf"
  fi
  run_apply "$layout" "$config_home"
  cmp -s "$config_file" "$work_dir/$layout-before" || fail "$layout: changed kitty.conf"
  [ "$(stat -c '%Y' "$config_file")" = "$(stat -c '%Y' "$work_dir/$layout-before")" ] \
    || fail "$layout: touched kitty.conf"
done
[ -L "$work_dir/symlink/kitty/kitty.conf" ] || fail "symlink: replaced the kitty.conf symlink"

config_home=$(new_case missing)
run_apply missing "$config_home"
[ "$(cat "$config_home/kitty/kitty.conf")" = 'include themes/noctalia.conf' ] \
  || fail "missing: did not create kitty.conf with the theme include"
