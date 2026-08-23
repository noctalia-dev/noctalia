#!/bin/sh

set -eu

noctalia_bin=$1

fail() {
  printf '%s\n' "config_validate_cli_test: FAIL: $*" >&2
  exit 1
}

if valid_output=$("$noctalia_bin" config validate tests/config_validate/generated-config 2>&1); then
  :
else
  status=$?
  fail "generated single-file config should validate (exit $status): $valid_output"
fi
case "$valid_output" in
  *"Config is valid"*) ;;
  *) fail "generated single-file config did not print success" ;;
esac
case "$valid_output" in
  *"WARN"*) fail "generated single-file config reported a warning" ;;
esac

warn_output=$("$noctalia_bin" config validate tests/config_validate/warn-only.toml 2>&1) \
  || fail "warning-only config should validate"
# Every diagnostic is prefixed with the file:line:column it came from.
case "$warn_output" in
  *"WARN  tests/config_validate/warn-only.toml:3:10: accessibility.ui_scl: unknown setting"*) ;;
  *) fail "warning-only config did not report the unknown setting with its origin" ;;
esac
case "$warn_output" in
  *"WARN  tests/config_validate/warn-only.toml:8:1: shell.launcher.providers.applications: custom settings are not allowed"*) ;;
  *) fail "warning-only config did not report the disallowed applications provider setting" ;;
esac
case "$warn_output" in
  *"WARN  tests/config_validate/warn-only.toml:6:19: shell.launcher.provider_prefix: is empty"*) ;;
  *) fail "warning-only config did not report the empty provider_prefix" ;;
esac
case "$warn_output" in
  *"WARN  tests/config_validate/warn-only.toml:11:1: shell.launcher.providers.nonexistent: provider is nonexistent"*) ;;
  *) fail "warning-only config did not report the nonexistent provider setting" ;;
esac
case "$warn_output" in
  *"shell.launcher.providers.author/my-plugin:entry: plugin 'author/my-plugin' is not enabled"*) ;;
  *) fail "warning-only config did not report the disabled plugin provider setting" ;;
esac
case "$warn_output" in
  *"duplicates the prefix of"*) ;;
  *) fail "warning-only config did not report the duplicate provider prefix" ;;
esac

syntax_output=$("$noctalia_bin" config validate tests/config_validate/syntax-error.toml 2>&1) \
  && fail "syntax-error config should fail"
case "$syntax_output" in
  *"ERROR tests/config_validate/syntax-error.toml:2:25: syntax: "*) ;;
  *) fail "syntax-error config did not report the source position" ;;
esac

timezone_output=$("$noctalia_bin" config validate tests/config_validate/invalid-timezone.toml 2>&1) \
  && fail "invalid timezone config should fail"
case "$timezone_output" in
  *'ERROR tests/config_validate/invalid-timezone.toml:3:12: widget.world-clock.timezone: unknown timezone "Europe/Berln"'*) ;;
  *) fail "invalid timezone config did not report the widget setting path" ;;
esac

# The exporter and the validator must agree on every section: whatever `config export
# full` emits, `config validate` has to recognize. A section wired into one but not the
# other (the historical failure mode) shows up here as an unknown section/setting.
export_dir=$(mktemp -d)
trap 'rm -rf "$export_dir"' EXIT
mkdir -p "$export_dir/config" "$export_dir/state"
XDG_CONFIG_HOME="$export_dir/config" XDG_STATE_HOME="$export_dir/state" \
  "$noctalia_bin" config export full > "$export_dir/full.toml" \
  || fail "config export full failed"

if export_output=$("$noctalia_bin" config validate "$export_dir/full.toml" 2>&1); then
  :
else
  status=$?
  fail "the exported full config should validate (exit $status): $export_output"
fi
case "$export_output" in
  *"WARN"*) fail "exported full config reported a warning: $export_output" ;;
esac
case "$export_output" in
  *"Config is valid"*) ;;
  *) fail "exported full config did not print success" ;;
esac
