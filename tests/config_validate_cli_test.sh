#!/bin/sh

set -eu

noctalia_bin=$1
empty_fontconfig=$PWD/tests/config_validate/empty-fontconfig.conf

fail() {
  printf '%s\n' "config_validate_cli_test: FAIL: $*" >&2
  exit 1
}

valid_output=$(FONTCONFIG_FILE="$empty_fontconfig" \
  "$noctalia_bin" config validate tests/config_validate/generated-config 2>&1) || {
  printf '%s\n' "$valid_output" >&2
  fail "generated single-file config should validate"
}
case "$valid_output" in
  *"Config is valid"*) ;;
  *) fail "generated single-file config did not print success" ;;
esac
case "$valid_output" in
  *"WARN"*) fail "generated single-file config reported a warning" ;;
esac

warn_output=$("$noctalia_bin" config validate tests/config_validate/warn-only.toml 2>&1) \
  || fail "warning-only config should validate"
case "$warn_output" in
  *"WARN  accessibility.ui_scl: unknown setting"*) ;;
  *) fail "warning-only config did not report the unknown setting" ;;
esac

syntax_output=$("$noctalia_bin" config validate tests/config_validate/syntax-error.toml 2>&1) \
  && fail "syntax-error config should fail"
case "$syntax_output" in
  *"ERROR syntax: tests/config_validate/syntax-error.toml:"*) ;;
  *) fail "syntax-error config did not report the source path" ;;
esac

timezone_output=$("$noctalia_bin" config validate tests/config_validate/invalid-timezone.toml 2>&1) \
  && fail "invalid timezone config should fail"
case "$timezone_output" in
  *'ERROR widget.world-clock.timezone: unknown timezone "Europe/Berln"'*) ;;
  *) fail "invalid timezone config did not report the widget setting path" ;;
esac
