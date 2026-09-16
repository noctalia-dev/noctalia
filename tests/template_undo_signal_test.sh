#!/usr/bin/env bash

# An undo hook must only reload the client it just un-themed. A reload sent to an
# unrelated process that happens to be starting (SIGUSR2 before Ghostty installs its
# handler) kills it, so a no-op undo has to stay completely silent.

set -euo pipefail

templates_dir=$1

fail() {
  printf '%s\n' "template_undo_signal_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

stub_dir="$work_dir/bin"
mkdir -p "$stub_dir"
calls_file="$work_dir/calls"

# Every command an undo hook can use to reload a client records its invocation instead.
for stub in pkill mmsg gdbus emacsclient; do
  cat >"$stub_dir/$stub" <<EOF
#!/bin/sh
printf '%s %s\n' "$stub" "\$*" >>"$calls_file"
exit 0
EOF
  chmod +x "$stub_dir/$stub"
done

# The guards in front of a reload must not decide the outcome: report every client as
# running, in a mode that accepts the reload.
cat >"$stub_dir/pgrep" <<'EOF'
#!/bin/sh
echo "4242 $*"
exit 0
EOF
chmod +x "$stub_dir/pgrep"

export PATH="$stub_dir:$PATH"

run_undo() {
  local script=$1 config_home=$2
  : >"$calls_file"
  XDG_CONFIG_HOME="$config_home" \
    XDG_DATA_HOME="$config_home/data" \
    XDG_CACHE_HOME="$config_home/cache" \
    bash "$templates_dir/$script" >/dev/null 2>&1 \
    || fail "$script exited non-zero"
}

# script:theme file relative to XDG_CONFIG_HOME
cases="
ghostty/undo.sh:ghostty/themes/noctalia
kitty/undo.sh:kitty/themes/noctalia.conf
btop/undo.sh:btop/themes/noctalia.theme
cava/undo.sh:cava/themes/noctalia
mango/undo.sh:mango/noctalia.conf
"

for entry in $cases; do
  script=${entry%%:*}
  theme_file=${entry#*:}

  config_home="$work_dir/$(dirname "$script")-clean"
  mkdir -p "$config_home"
  run_undo "$script" "$config_home"
  if [ -s "$calls_file" ]; then
    fail "$script reloaded its client with nothing to undo: $(cat "$calls_file")"
  fi

  config_home="$work_dir/$(dirname "$script")-themed"
  mkdir -p "$config_home/$(dirname "$theme_file")"
  printf 'noctalia\n' >"$config_home/$theme_file"
  run_undo "$script" "$config_home"
  if [ ! -s "$calls_file" ]; then
    fail "$script removed its theme without reloading its client"
  fi
  if [ -e "$config_home/$theme_file" ]; then
    fail "$script left its theme file behind"
  fi
done

# A no-op undo must not rewrite the user's config either, and wezterm's reload is an
# mtime bump on wezterm.lua rather than a signal.
config_home="$work_dir/wezterm-clean"
mkdir -p "$config_home/wezterm"
config_file="$config_home/wezterm/wezterm.lua"
printf 'local config = {}\nreturn config\n' >"$config_file"
touch -t 202001010000 "$config_file"
before=$(stat -c '%Y %s' "$config_file")
run_undo "wezterm/undo.sh" "$config_home"
after=$(stat -c '%Y %s' "$config_file")
if [ "$before" != "$after" ]; then
  fail "wezterm/undo.sh touched wezterm.lua with nothing to undo"
fi

config_home="$work_dir/wezterm-themed"
mkdir -p "$config_home/wezterm/colors"
config_file="$config_home/wezterm/wezterm.lua"
printf 'local config = {}\nconfig.color_scheme = "Noctalia"\nreturn config\n' >"$config_file"
printf 'colors\n' >"$config_home/wezterm/colors/Noctalia.toml"
touch -t 202001010000 "$config_file"
run_undo "wezterm/undo.sh" "$config_home"
if grep -q 'Noctalia' "$config_file"; then
  fail "wezterm/undo.sh left the Noctalia color scheme in wezterm.lua"
fi
if [ "$(stat -c '%Y' "$config_file")" = "$(date -d '2020-01-01 00:00' +%s)" ]; then
  fail "wezterm/undo.sh did not bump wezterm.lua after removing the color scheme"
fi
