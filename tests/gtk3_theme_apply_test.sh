#!/usr/bin/env bash
set -euo pipefail

templates_dir=${1:?usage: gtk3_theme_apply_test.sh <templates-dir>}

work_dir=$(mktemp -d)
cleanup() {
    rm -rf "$work_dir"
}
trap cleanup EXIT

fail() {
    printf 'gtk3_theme_apply_test: FAIL: %s\n' "$*" >&2
    exit 1
}

config_home="$work_dir/config"
data_home="$work_dir/data"
mkdir -p \
    "$config_home/gtk-3.0" \
    "$config_home/gtk-4.0" \
    "$data_home/themes/adw-gtk3/gtk-3.0" \
    "$data_home/themes/adw-gtk3-dark/gtk-3.0"

# Minimal stand-ins for adw-gtk3 and adw-gtk3-dark. The real themes are large;
# the apply hook only needs a gtk-3.0 directory, a gtk.css and a gtk-dark.css.
printf '@define-color window_bg_color #101010;\n' >"$data_home/themes/adw-gtk3/gtk-3.0/gtk.css"
printf '@define-color window_bg_color #202020;\n' >"$data_home/themes/adw-gtk3/gtk-3.0/gtk-dark.css"
printf 'asset\n' >"$data_home/themes/adw-gtk3/gtk-3.0/asset.txt"
printf '@define-color window_bg_color #303030;\n' >"$data_home/themes/adw-gtk3-dark/gtk-3.0/gtk.css"
printf '@define-color window_bg_color #404040;\n' >"$data_home/themes/adw-gtk3-dark/gtk-3.0/gtk-dark.css"

# Existing user CSS plus the old generated override that should be removed.
printf '/* user css */\n@import url("noctalia.css");\n' >"$config_home/gtk-3.0/gtk.css"
printf '/* stale gtk3 colors */\n' >"$config_home/gtk-3.0/noctalia.css"

# The dual-variant staging file rendered by the template engine.
cat >"$config_home/gtk-3.0/noctalia-gtk3.css" <<'EOF'
/* noctalia-gtk3-light-start */
@define-color window_bg_color #aaaaaa;
/* noctalia-gtk3-light-end */
/* noctalia-gtk3-dark-start */
@define-color window_bg_color #bbbbbb;
/* noctalia-gtk3-dark-end */
EOF

printf '/* user gtk4 css */\n' >"$config_home/gtk-4.0/gtk.css"
printf '/* gtk4 colors */\n' >"$config_home/gtk-4.0/noctalia.css"

run_apply() {
    XDG_CONFIG_HOME="$config_home" \
        XDG_DATA_HOME="$data_home" \
        NOCTALIA_GTK_SKIP_GSETTINGS=1 \
        bash "$templates_dir/gtk/apply.sh" "$1"
}

run_undo() {
    XDG_CONFIG_HOME="$config_home" \
        XDG_DATA_HOME="$data_home" \
        NOCTALIA_GTK_SKIP_GSETTINGS=1 \
        bash "$templates_dir/gtk/undo-gtk3.sh"
}

run_apply light >/dev/null || fail "apply.sh light exited non-zero"

[ -f "$data_home/themes/Noctalia/.noctalia-theme" ] || fail "Noctalia theme marker missing"
[ -f "$data_home/themes/Noctalia-dark/.noctalia-theme" ] || fail "Noctalia-dark theme marker missing"
[ -f "$data_home/themes/Noctalia/gtk-3.0/gtk.css" ] || fail "Noctalia gtk.css missing"
[ -f "$data_home/themes/Noctalia/gtk-3.0/gtk-dark.css" ] || fail "Noctalia gtk-dark.css missing"
[ -f "$data_home/themes/Noctalia/index.theme" ] || fail "Noctalia index.theme missing"
[ -f "$data_home/themes/Noctalia/gtk-3.0/asset.txt" ] || fail "base theme assets were not copied"

grep -q '#101010' "$data_home/themes/Noctalia/gtk-3.0/gtk.css" || fail "base light CSS missing"
grep -q '#aaaaaa' "$data_home/themes/Noctalia/gtk-3.0/gtk.css" || fail "light colors missing from Noctalia gtk.css"
grep -q '#bbbbbb' "$data_home/themes/Noctalia/gtk-3.0/gtk-dark.css" || fail "dark colors missing from Noctalia gtk-dark.css"
grep -q '#bbbbbb' "$data_home/themes/Noctalia-dark/gtk-3.0/gtk.css" || fail "dark colors missing from Noctalia-dark gtk.css"
grep -q '#bbbbbb' "$data_home/themes/Noctalia-dark/gtk-3.0/gtk-dark.css" || fail "dark colors missing from Noctalia-dark gtk-dark.css"

grep -q 'user css' "$config_home/gtk-3.0/gtk.css" || fail "user GTK3 CSS was removed"
grep -q '@import url("noctalia.css");' "$config_home/gtk-3.0/gtk.css" && fail "old noctalia.css import was left behind"
grep -q '@import url("noctalia-extensions.css");' "$config_home/gtk-3.0/gtk.css" \
    || fail "GTK extensions import missing"
[ ! -e "$config_home/gtk-3.0/noctalia.css" ] || fail "stale GTK3 noctalia.css was not removed"

grep -q '@import url("noctalia.css");' "$config_home/gtk-4.0/gtk.css" \
    || fail "GTK4 colors import missing"

run_apply dark >/dev/null || fail "apply.sh dark exited non-zero"
[ -f "$data_home/themes/Noctalia/.noctalia-theme" ] || fail "Noctalia theme disappeared on dark apply"
[ -f "$data_home/themes/Noctalia-dark/.noctalia-theme" ] || fail "Noctalia-dark theme disappeared on dark apply"

run_undo >/dev/null || fail "undo-gtk3.sh exited non-zero"

[ ! -e "$data_home/themes/Noctalia" ] || fail "Noctalia theme was not removed"
[ ! -e "$data_home/themes/Noctalia-dark" ] || fail "Noctalia-dark theme was not removed"
[ -f "$data_home/themes/adw-gtk3/gtk-3.0/gtk.css" ] || fail "base adw-gtk3 theme was removed"
[ ! -e "$config_home/gtk-3.0/noctalia-gtk3.css" ] || fail "GTK3 staging file was not removed"
grep -q '@import' "$config_home/gtk-3.0/gtk.css" && fail "GTK3 imports were left behind after undo"

# Without adw-gtk3 the hook must keep the old single-mode override working.
fallback_config="$work_dir/fallback-config"
fallback_data="$work_dir/fallback-data"
mkdir -p "$fallback_config/gtk-3.0" "$fallback_data/themes"
printf '/* fallback user */\n' >"$fallback_config/gtk-3.0/gtk.css"

cat >"$fallback_config/gtk-3.0/noctalia-gtk3.css" <<'EOF'
/* noctalia-gtk3-light-start */
@define-color window_bg_color #aaaaaa;
/* noctalia-gtk3-light-end */
/* noctalia-gtk3-dark-start */
@define-color window_bg_color #bbbbbb;
/* noctalia-gtk3-dark-end */
EOF

XDG_CONFIG_HOME="$fallback_config" \
    XDG_DATA_HOME="$fallback_data" \
    NOCTALIA_GTK_SKIP_SYSTEM_THEMES=1 \
    NOCTALIA_GTK_SKIP_GSETTINGS=1 \
    bash "$templates_dir/gtk/apply.sh" light >/dev/null 2>&1 \
    || fail "fallback apply.sh light exited non-zero"
[ -f "$fallback_config/gtk-3.0/noctalia.css" ] || fail "fallback noctalia.css missing"
grep -q '#aaaaaa' "$fallback_config/gtk-3.0/noctalia.css" || fail "fallback light colors missing"
grep -q '@import url("noctalia.css");' "$fallback_config/gtk-3.0/gtk.css" \
    || fail "fallback import missing"

XDG_CONFIG_HOME="$fallback_config" \
    XDG_DATA_HOME="$fallback_data" \
    NOCTALIA_GTK_SKIP_GSETTINGS=1 \
    bash "$templates_dir/gtk/undo-gtk3.sh" >/dev/null \
    || fail "fallback undo-gtk3.sh exited non-zero"
[ ! -e "$fallback_config/gtk-3.0/noctalia.css" ] || fail "fallback noctalia.css was not removed"
grep -q '@import' "$fallback_config/gtk-3.0/gtk.css" && fail "fallback import was left behind"

# Undo removes the staging file, so keep a copy to restore before each apply.
stage_src="$work_dir/noctalia-gtk3.css.src"
cat >"$stage_src" <<'EOF'
/* noctalia-gtk3-light-start */
@define-color window_bg_color #aaaaaa;
/* noctalia-gtk3-light-end */
/* noctalia-gtk3-dark-start */
@define-color window_bg_color #bbbbbb;
/* noctalia-gtk3-dark-end */
EOF

# gsettings stub: records gtk-theme writes so the GNOME paths can be checked.
gsettings_state="$work_dir/gsettings"
mkdir -p "$gsettings_state" "$work_dir/bin"
cat >"$work_dir/bin/gsettings" <<'EOF'
#!/usr/bin/env bash
case "$1" in
    list-schemas) echo org.gnome.desktop.interface ;;
    get) cat "$GSETTINGS_STATE/$3" ;;
    set)
        printf "'%s'\n" "$4" >"$GSETTINGS_STATE/$3"
        printf '%s\n' "$4" >>"$GSETTINGS_STATE/set.log"
        ;;
    *) exit 1 ;;
esac
EOF
chmod +x "$work_dir/bin/gsettings"

run_with_gsettings() {
    PATH="$work_dir/bin:$PATH" GSETTINGS_STATE="$gsettings_state" "$@"
}

# Apply must point gtk-theme at the generated Noctalia theme.
cp "$stage_src" "$config_home/gtk-3.0/noctalia-gtk3.css"
printf "'adw-gtk3'\n" >"$gsettings_state/gtk-theme"
printf "'prefer-light'\n" >"$gsettings_state/color-scheme"
rm -f "$gsettings_state/set.log"
run_with_gsettings env XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
    bash "$templates_dir/gtk/apply.sh" light >/dev/null || fail "gsettings apply light exited non-zero"
[ "$(tail -n1 "$gsettings_state/set.log")" = "Noctalia" ] || fail "apply did not select Noctalia theme"

# A foreign Noctalia directory forces the fallback; gtk-theme must not point at it.
foreign_config="$work_dir/foreign-config"
foreign_data="$work_dir/foreign-data"
mkdir -p "$foreign_config/gtk-3.0" "$foreign_data/themes/adw-gtk3/gtk-3.0" \
    "$foreign_data/themes/adw-gtk3-dark/gtk-3.0" "$foreign_data/themes/Noctalia/gtk-3.0"
printf '@define-color window_bg_color #101010;\n' >"$foreign_data/themes/adw-gtk3/gtk-3.0/gtk.css"
printf '@define-color window_bg_color #202020;\n' >"$foreign_data/themes/adw-gtk3-dark/gtk-3.0/gtk.css"
printf 'user foreign theme\n' >"$foreign_data/themes/Noctalia/gtk-3.0/gtk.css"
cp "$stage_src" "$foreign_config/gtk-3.0/noctalia-gtk3.css"
printf "'adw-gtk3-dark'\n" >"$gsettings_state/gtk-theme"
rm -f "$gsettings_state/set.log"
run_with_gsettings env XDG_CONFIG_HOME="$foreign_config" XDG_DATA_HOME="$foreign_data" \
    NOCTALIA_GTK_SKIP_SYSTEM_THEMES=1 \
    bash "$templates_dir/gtk/apply.sh" light >/dev/null 2>&1 \
    || fail "foreign-theme apply exited non-zero"
[ -f "$foreign_config/gtk-3.0/noctalia.css" ] || fail "foreign-theme fallback override missing"
grep -q 'user foreign theme' "$foreign_data/themes/Noctalia/gtk-3.0/gtk.css" \
    || fail "foreign Noctalia theme was overwritten"
[ "$(tail -n1 "$gsettings_state/set.log")" = "adw-gtk3" ] \
    || fail "fallback did not select adw-gtk3 (got $(tail -n1 "$gsettings_state/set.log"))"

# Undo must leave a non-Noctalia gtk-theme alone.
printf "'Adwaita'\n" >"$gsettings_state/gtk-theme"
rm -f "$gsettings_state/set.log"
run_with_gsettings env XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
    bash "$templates_dir/gtk/undo-gtk3.sh" >/dev/null || fail "undo with Adwaita exited non-zero"
[ ! -e "$gsettings_state/set.log" ] || fail "undo changed a non-Noctalia gtk-theme"

# Undo restores the adw-gtk3 variant when the Noctalia theme is active.
printf "'Noctalia-dark'\n" >"$gsettings_state/gtk-theme"
printf "'prefer-dark'\n" >"$gsettings_state/color-scheme"
run_with_gsettings env XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
    bash "$templates_dir/gtk/undo-gtk3.sh" >/dev/null || fail "undo with Noctalia exited non-zero"
[ "$(tail -n1 "$gsettings_state/set.log")" = "adw-gtk3-dark" ] \
    || fail "undo did not restore adw-gtk3-dark"

# A read-only gtk.css symlink (NixOS-style) must keep the user's CSS.
ro_config="$work_dir/ro-config"
ro_dir="$work_dir/ro-source"
mkdir -p "$ro_config/gtk-3.0" "$ro_dir"
printf '/* user css */\n@import url("noctalia.css");\n' >"$ro_dir/gtk.css"
chmod 444 "$ro_dir/gtk.css"
ln -s "$ro_dir/gtk.css" "$ro_config/gtk-3.0/gtk.css"
cp "$stage_src" "$ro_config/gtk-3.0/noctalia-gtk3.css"

run_ro() {
    XDG_CONFIG_HOME="$ro_config" XDG_DATA_HOME="$data_home" \
        NOCTALIA_GTK_SKIP_GSETTINGS=1 bash "$@"
}

run_ro "$templates_dir/gtk/apply.sh" light >/dev/null || fail "read-only symlink apply exited non-zero"
[ -f "$ro_config/gtk-3.0/gtk.css" ] && [ ! -L "$ro_config/gtk-3.0/gtk.css" ] \
    || fail "read-only gtk.css symlink was not replaced by a regular file"
grep -q 'user css' "$ro_config/gtk-3.0/gtk.css" || fail "user CSS lost from read-only symlink"
grep -q '@import url("noctalia.css");' "$ro_config/gtk-3.0/gtk.css" && fail "old import left after read-only apply"
grep -q '@import url("noctalia-extensions.css");' "$ro_config/gtk-3.0/gtk.css" \
    || fail "extensions import missing after read-only apply"
grep -q 'user css' "$ro_dir/gtk.css" || fail "read-only source file was modified"

run_ro "$templates_dir/gtk/undo-gtk3.sh" >/dev/null || fail "read-only symlink undo exited non-zero"
grep -q 'user css' "$ro_config/gtk-3.0/gtk.css" || fail "user CSS lost after read-only undo"
grep -q '@import' "$ro_config/gtk-3.0/gtk.css" && fail "imports left behind after read-only undo"

printf 'gtk3_theme_apply_test: OK\n'
