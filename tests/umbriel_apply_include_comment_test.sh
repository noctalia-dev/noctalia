#!/usr/bin/env bash

# apply.sh must close include.files on the real array ']', not on brackets
# inside a '#' comment or inside a quoted entry name. Otherwise wallpaper apply
# moves noctalia.toml into the comment and empties the array
# (noctalia-dev/noctalia#4332), or splices it into a file name.

set -euo pipefail

apply_sh=${1:?apply.sh path}

fail() {
  printf '%s\n' "umbriel_apply_include_comment_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

files_line() {
  sed -n '/^[[:space:]]*files[[:space:]]*=/,$p' "$1"
}

run_apply() {
  local name=$1
  shift
  local home="$work_dir/$name"
  mkdir -p "$home/umbriel"
  printf '%s\n' "$@" >"$home/umbriel/config.toml"
  XDG_CONFIG_HOME="$home" bash "$apply_sh" \
    || fail "$name: apply.sh exited non-zero"
  files_line "$home/umbriel/config.toml"
}

expect() {
  local name=$1
  shift
  local want=$1
  shift
  local got
  got=$(run_apply "$name" "$@")
  if [ "$got" != "$want" ]; then
    fail "$name: expected $(printf '%q' "$want") got $(printf '%q' "$got")"
  fi
}

expect_error() {
  local name=$1
  shift
  local home="$work_dir/$name"
  mkdir -p "$home/umbriel"
  printf '%s\n' "$@" >"$home/umbriel/config.toml"
  local before status=0
  before=$(cat "$home/umbriel/config.toml")
  XDG_CONFIG_HOME="$home" bash "$apply_sh" 2>/dev/null || status=$?
  if [ "$status" != 2 ]; then
    fail "$name: expected exit 2, got $status"
  fi
  if [ "$(cat "$home/umbriel/config.toml")" != "$before" ]; then
    fail "$name: config rewritten despite the error"
  fi
}

expect reporter \
  'files = ["noctalia.toml"] # []' \
  '[include]' \
  'files = ["noctalia.toml"] # []'

expect control_nocomment \
  'files = ["noctalia.toml"]' \
  '[include]' \
  'files = ["noctalia.toml"]'

expect control_comment \
  'files = ["noctalia.toml"] # keep this' \
  '[include]' \
  'files = ["noctalia.toml"] # keep this'

expect user_comment \
  'files = ["user.toml", "noctalia.toml"] # []' \
  '[include]' \
  'files = ["user.toml"] # []'

expect multiline \
  $'files = [\n  "user.toml",\n  "noctalia.toml",\n]' \
  '[include]' \
  'files = [' \
  '  "user.toml",' \
  ']'

expect quoted_hash \
  'files = ["a#b.toml", "noctalia.toml"]' \
  '[include]' \
  'files = ["a#b.toml"]'

expect quoted_close \
  'files = ["a]b.toml", "noctalia.toml"]' \
  '[include]' \
  'files = ["a]b.toml"]'

# A scalar value whose only brackets sit in a comment is not an array.
expect_error scalar_bracket_comment \
  '[include]' \
  'files = "user.toml" # [a]'
