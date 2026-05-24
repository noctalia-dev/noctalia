project('noctalia', ['c', 'cpp'],
  version: '5.0.0',
  default_options: [
    'cpp_std=c++20',
    'cpp_extensions=false',
    'warning_level=0',   # applied selectively to our target; avoids noise in vendored code
    'b_ndebug=if-release',
  ],
)

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Sanitizers are enabled via meson's built-in -Db_sanitize=address,undefined.
# pipewire_lsan.cpp is only needed when address sanitizer is active.
sanitize = get_option('b_sanitize') != 'none'

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-march=native', '-mtune=native',
    '-fomit-frame-pointer', '-ffunction-sections', '-fdata-sections',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c', 'cpp'],
  )
endif

# ── Dependencies ──────────────────────────────────────────────────────────────
sdbus_cpp_dep       = dependency('sdbus-c++')
wayland_client_dep  = dependency('wayland-client')
wayland_protos_dep  = dependency('wayland-protocols')
#egl_dep             = dependency('egl')
#glesv2_dep          = dependency('glesv2')
wayland_egl_dep     = dependency('wayland-egl')
freetype2_dep       = dependency('freetype2')
fontconfig_dep      = dependency('fontconfig')
cairo_dep           = dependency('cairo')
cairo_ft_dep        = dependency('cairo-ft')
pango_dep           = dependency('pango')
pangocairo_dep      = dependency('pangocairo')
pangoft2_dep        = dependency('pangoft2')
harfbuzz_dep        = dependency('harfbuzz')
librsvg_dep         = dependency('librsvg-2.0')
xkbcommon_dep       = dependency('xkbcommon')
glib_dep            = dependency('glib-2.0')
gobject_dep         = dependency('gobject-2.0')
gio_dep             = dependency('gio-2.0')
polkit_agent_dep    = dependency('polkit-agent-1')
polkit_gobject_dep  = dependency('polkit-gobject-1')
# PipeWire/SPA headers use GCC extensions that trigger -Wpedantic/-Wconversion;
# mark as system so the compiler suppresses those warnings.
pipewire_dep        = dependency('libpipewire-0.3', include_type: 'system')
curl_dep            = dependency('libcurl')
pam_dep             = cc.find_library('pam')
dl_dep              = cpp.find_library('dl', required: false)

jemalloc_option = get_option('jemalloc')
is_glibc = cpp.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
jemalloc_deps = []
jemalloc_cpp_args = []

if jemalloc_option.enabled() and not is_glibc
  error('jemalloc support requires glibc')
endif

if is_glibc
  jemalloc_dep = dependency('jemalloc', required: jemalloc_option)
  if jemalloc_dep.found()
    jemalloc_deps += [jemalloc_dep]
    jemalloc_cpp_args += ['-DNOCTALIA_USE_JEMALLOC=1']
  endif
endif

egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)


if not egl_dep.found()
  egl_dep = dependency('wayland-egl', required: false)
endif

if not egl_dep.found()
  egl_dep = dependency('epoxy')
endif

if not gles2_dep.found()
  gles2_dep = dependency('epoxy')
endif

# ── Vendored: tinyexpr (C, needs suppressed warnings) ─────────────────────────
_tinyexpr_lib = static_library('tinyexpr',
  'third_party/tinyexpr/tinyexpr.c',
  c_args: ['-Wno-pedantic', '-Wno-conversion'],
  override_options: ['warning_level=0'],
)
tinyexpr_dep = declare_dependency(
  link_with: _tinyexpr_lib,
  include_directories: include_directories('third_party/tinyexpr', is_system: true),
)

# ── Vendored: dr_wav (single-file WAV decoder) ───────────────────────────────
_drwav_lib = static_library('dr_wav',
  'third_party/dr_wav/dr_wav.c',
  override_options: ['warning_level=0'],
)
drwav_dep = declare_dependency(
  link_with: _drwav_lib,
  include_directories: include_directories('third_party/dr_wav', is_system: true),
)

# ── Vendored: fzy matcher (C fuzzy scoring) ──────────────────────────────────
_fzy_lib = static_library('fzy',
  'third_party/fzy/src/match.c',
  c_args: ['-Wno-pedantic', '-Wno-conversion'],
  override_options: ['warning_level=0'],
)
fzy_dep = declare_dependency(
  link_with: _fzy_lib,
  include_directories: include_directories('third_party/fzy/src', is_system: true),
)

# ── Vendored: Material Color Utilities (Google) ───────────────────────────────
# Upstream uses #include "cpp/..." prefixes, so the include root is the
# material_color_utilities/ dir itself. Built as a quiet static library.
_mcu_sources = files(
  'third_party/material_color_utilities/cpp/blend/blend.cc',
  'third_party/material_color_utilities/cpp/cam/cam.cc',
  'third_party/material_color_utilities/cpp/cam/hct.cc',
  'third_party/material_color_utilities/cpp/cam/hct_solver.cc',
  'third_party/material_color_utilities/cpp/cam/viewing_conditions.cc',
  'third_party/material_color_utilities/cpp/contrast/contrast.cc',
  'third_party/material_color_utilities/cpp/dislike/dislike.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_color.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_scheme.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/material_dynamic_colors.cc',
  'third_party/material_color_utilities/cpp/palettes/tones.cc',
  'third_party/material_color_utilities/cpp/quantize/celebi.cc',
  'third_party/material_color_utilities/cpp/quantize/lab.cc',
  'third_party/material_color_utilities/cpp/quantize/wsmeans.cc',
  'third_party/material_color_utilities/cpp/quantize/wu.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_content.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_expressive.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_fidelity.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_fruit_salad.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_monochrome.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_neutral.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_rainbow.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_tonal_spot.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_vibrant.cc',
  'third_party/material_color_utilities/cpp/score/score.cc',
  'third_party/material_color_utilities/cpp/temperature/temperature_cache.cc',
  'third_party/material_color_utilities/cpp/utils/utils.cc',
)
_mcu_lib = static_library('material_color_utilities',
  _mcu_sources,
  include_directories: include_directories('third_party/material_color_utilities', is_system: true),
  cpp_args: ['-Wno-pedantic', '-Wno-conversion', '-Wno-shadow', '-Wno-unused-parameter'],
  override_options: ['warning_level=0'],
)
mcu_dep = declare_dependency(
  link_with: _mcu_lib,
  include_directories: include_directories('third_party/material_color_utilities', is_system: true),
)

# ── Vendored: stb_image_resize2 (header-only) ─────────────────────────────────
stb_dep = declare_dependency(
  include_directories: include_directories('third_party/stb', is_system: true),
)

# ── System: libwebp (WebP decode and thumbnail encode) ───────────────────────
libwebp_dep = dependency('libwebp', required: true)

# ── Vendored: Luau (Roblox's Lua fork) ────────────────────────────────────────
# Built as a quiet static library; only VM + Compiler + Ast + Common are vendored.
_luau_sources = files(
  'third_party/luau/Common/src/StringUtils.cpp',
  'third_party/luau/Common/src/TimeTrace.cpp',
  'third_party/luau/Ast/src/Allocator.cpp',
  'third_party/luau/Ast/src/Ast.cpp',
  'third_party/luau/Ast/src/Confusables.cpp',
  'third_party/luau/Ast/src/Cst.cpp',
  'third_party/luau/Ast/src/Lexer.cpp',
  'third_party/luau/Ast/src/Location.cpp',
  'third_party/luau/Ast/src/Parser.cpp',
  'third_party/luau/Ast/src/PrettyPrinter.cpp',
  'third_party/luau/Compiler/src/BuiltinFolding.cpp',
  'third_party/luau/Compiler/src/Builtins.cpp',
  'third_party/luau/Compiler/src/BytecodeBuilder.cpp',
  'third_party/luau/Compiler/src/Compiler.cpp',
  'third_party/luau/Compiler/src/ConstantFolding.cpp',
  'third_party/luau/Compiler/src/CostModel.cpp',
  'third_party/luau/Compiler/src/lcode.cpp',
  'third_party/luau/Compiler/src/TableShape.cpp',
  'third_party/luau/Compiler/src/Types.cpp',
  'third_party/luau/Compiler/src/ValueTracking.cpp',
  'third_party/luau/VM/src/lapi.cpp',
  'third_party/luau/VM/src/laux.cpp',
  'third_party/luau/VM/src/lbaselib.cpp',
  'third_party/luau/VM/src/lbitlib.cpp',
  'third_party/luau/VM/src/lbuffer.cpp',
  'third_party/luau/VM/src/lbuflib.cpp',
  'third_party/luau/VM/src/lbuiltins.cpp',
  'third_party/luau/VM/src/lcorolib.cpp',
  'third_party/luau/VM/src/ldblib.cpp',
  'third_party/luau/VM/src/ldebug.cpp',
  'third_party/luau/VM/src/ldo.cpp',
  'third_party/luau/VM/src/lfunc.cpp',
  'third_party/luau/VM/src/lgc.cpp',
  'third_party/luau/VM/src/lgcdebug.cpp',
  'third_party/luau/VM/src/linit.cpp',
  'third_party/luau/VM/src/lintlib.cpp',
  'third_party/luau/VM/src/lmathlib.cpp',
  'third_party/luau/VM/src/lmem.cpp',
  'third_party/luau/VM/src/lnumprint.cpp',
  'third_party/luau/VM/src/lobject.cpp',
  'third_party/luau/VM/src/loslib.cpp',
  'third_party/luau/VM/src/lperf.cpp',
  'third_party/luau/VM/src/lstate.cpp',
  'third_party/luau/VM/src/lstring.cpp',
  'third_party/luau/VM/src/lstrlib.cpp',
  'third_party/luau/VM/src/ltable.cpp',
  'third_party/luau/VM/src/ltablib.cpp',
  'third_party/luau/VM/src/ltm.cpp',
  'third_party/luau/VM/src/ludata.cpp',
  'third_party/luau/VM/src/lutf8lib.cpp',
  'third_party/luau/VM/src/lveclib.cpp',
  'third_party/luau/VM/src/lvmexecute.cpp',
  'third_party/luau/VM/src/lvmload.cpp',
  'third_party/luau/VM/src/lvmutils.cpp',
)
_luau_inc = include_directories(
  'third_party/luau/Common/include',
  'third_party/luau/Ast/include',
  'third_party/luau/Compiler/include',
  'third_party/luau/VM/include',
  'third_party/luau/VM/src',
  is_system: true,
)
_luau_lib = static_library('luau',
  _luau_sources,
  include_directories: _luau_inc,
  cpp_args: ['-w'],
  override_options: ['warning_level=0'],
)
luau_dep = declare_dependency(
  link_with: _luau_lib,
  include_directories: _luau_inc,
)

# ── Wayland protocol generation ───────────────────────────────────────────────
wayland_scanner    = find_program('wayland-scanner')
wayland_protos_dir = wayland_protos_dep.get_variable('pkgdatadir')

# Generates a (header, source) pair from a protocol XML path/target.
# Returns [h_target, c_target] — add c_target to sources.
# The c_target has an order-only dep on h_target so the header exists when
# the generated .c is compiled (it does #include "foo-client-protocol.h").
_protocol_sources = []
_protocol_headers = []

# Helper macro via foreach — each entry: [name, xml_input]
# xml_input can be a string (path) or a custom_target.

foreach _p : [
  # ── wlr-layer-shell needs namespace→name_space substitution first ──────────
  # Handled separately below.

  # ── system wayland-protocols ──────────────────────────────────────────────
  ['xdg-output-unstable-v1',
    wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell',
    wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['ext-workspace-v1',
    wayland_protos_dir / 'staging/ext-workspace/ext-workspace-v1.xml'],
  ['ext-foreign-toplevel-list-v1',
    wayland_protos_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'],
  ['ext-data-control-v1',
    wayland_protos_dir / 'staging/ext-data-control/ext-data-control-v1.xml'],
  ['xdg-activation-v1',
    wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['ext-session-lock-v1',
    wayland_protos_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'],
  ['ext-idle-notify-v1',
    wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'],
  ['ext-background-effect-v1',
    wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['fractional-scale-v1',
    wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter',
    wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['idle-inhibit-unstable-v1',
    wayland_protos_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],
  ['virtual-keyboard-unstable-v1',
    'protocols/virtual-keyboard-unstable-v1.xml'],

  # ── project-local custom protocols ────────────────────────────────────────
  ['dwl-ipc-unstable-v2',
    'protocols/dwl-ipc-unstable-v2.xml'],
  ['cursor-shape-v1',
    'protocols/cursor-shape-v1.xml'],
  ['wlr-foreign-toplevel-management-unstable-v1',
    'protocols/wlr-foreign-toplevel-management-unstable-v1.xml'],
  ['wlr-data-control-unstable-v1',
    'protocols/wlr-data-control-unstable-v1.xml'],
  ['hyprland-focus-grab-v1',
    'protocols/hyprland-focus-grab-v1.xml'],
  ['hyprland-toplevel-mapping-v1',
    'protocols/hyprland-toplevel-mapping-v1.xml'],
  ['wlr-gamma-control-unstable-v1',
    'protocols/wlr-gamma-control-unstable-v1.xml'],
]
  _name = _p[0]
  _xml  = _p[1]

  _h = custom_target(_name + '-client-header',
    input:   _xml,
    output:  _name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  _c = custom_target(_name + '-client-source',
    input:   _xml,
    output:  _name + '-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: _h,  # ensure .h exists before this .c is compiled (it #includes it)
  )
  _protocol_sources += _c
  _protocol_headers += _h
endforeach

# wlr-layer-shell: 'namespace' is a C++ keyword; rename it in the generated
# header only (private-code is compiled as C so it's fine as-is).
_wlr_ls_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
_wlr_ls_h = custom_target('wlr-layer-shell-unstable-v1-client-header',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [
    'sh', '-c',
    '"$1" client-header "$2" "$3" && sed -i s/namespace/name_space/g "$3"',
    '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@',
  ],
)
_wlr_ls_c = custom_target('wlr-layer-shell-unstable-v1-client-source',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  depends: _wlr_ls_h,
)
_protocol_sources += _wlr_ls_c
_protocol_headers += _wlr_ls_h

# ── Main sources ──────────────────────────────────────────────────────────────
_noctalia_sources = files(
  'src/app/application.cpp',
  'src/app/main_loop.cpp',
  'src/auth/pam_authenticator.cpp',
  'src/config/config_export.cpp',
  'src/config/config_overrides.cpp',
  'src/config/config_service.cpp',
  'src/config/config_types.cpp',
  'src/config/cli.cpp',
  'src/core/deferred_call.cpp',
  'src/core/file_watcher.cpp',
  'src/core/key_chord.cpp',
  'src/core/log.cpp',
  'src/core/process.cpp',
  'src/core/resource_paths.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/dbus/bluetooth/bluetooth_agent.cpp',
  'src/dbus/bluetooth/bluetooth_service.cpp',
  'src/dbus/logind/logind_service.cpp',
  'src/dbus/mpris/mpris_art.cpp',
  'src/dbus/mpris/mpris_service.cpp',
  'src/dbus/network/network_glyphs.cpp',
  'src/dbus/network/network_manager_service.cpp',
  'src/dbus/network/network_secret_agent.cpp',
  'src/dbus/network/wpa_supplicant_service.cpp',
  'src/dbus/polkit/polkit_agent.cpp',
  'src/dbus/notification/notification_poll_source.cpp',
  'src/dbus/notification/notification_service.cpp',
  'src/dbus/power/power_profiles_service.cpp',
  'src/dbus/session_bus.cpp',
  'src/dbus/system_bus.cpp',
  'src/dbus/tray/tray_service.cpp',
  'src/dbus/upower/upower_service.cpp',
  'src/debug/debug_service.cpp',
  'src/i18n/i18n.cpp',
  'src/i18n/i18n_service.cpp',
  'src/hooks/hook_manager.cpp',
  'src/idle/idle_inhibitor.cpp',
  'src/idle/idle_grace_overlay.cpp',
  'src/idle/idle_manager.cpp',
  'src/ipc/ipc_client.cpp',
  'src/ipc/ipc_service.cpp',
  'src/launcher/app_provider.cpp',
  'src/launcher/emoji_provider.cpp',
  'src/launcher/math_provider.cpp',
  'src/launcher/wallpaper_provider.cpp',
  'src/launcher/usage_tracker.cpp',
  'src/main.cpp',
  'src/net/http_client.cpp',
  'src/net/uri.cpp',
  'src/notification/notification_manager.cpp',
  'src/notification/notification_history_store.cpp',
  'src/pipewire/pipewire_service.cpp',
  'src/pipewire/pipewire_spectrum.cpp',
  'src/pipewire/sound_player.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/async_texture_cache.cpp',
  'src/render/core/color.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/core/stb_image_resize_impl.cpp',
  'src/render/core/thumbnail_service.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/linear_gradient_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/screen_corner_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/programs/audio_spectrum_program.cpp',
  'src/render/programs/effect_program.cpp',
  'src/render/programs/graph_program.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/wallpaper_program.cpp',
  'src/render/core/blur_cache.cpp',
  'src/render/core/cached_layer.cpp',
  'src/render/core/shared_texture_cache.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/graph_node.cpp',
  'src/render/scene/node.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/wallpaper_renderer.cpp',
  'src/scripting/luau_host.cpp',
  'src/scripting/script_runtime.cpp',
  'src/scripting/script_worker_pool.cpp',
  'src/scripting/scripted_widget_bindings.cpp',
  'src/scripting/scripted_widget_manifest.cpp',
  'src/shell/bar/bar.cpp',
  'src/shell/clipboard/clipboard_paste.cpp',
  'src/shell/dock/dock.cpp',
  'src/shell/desktop/desktop_widget.cpp',
  'src/shell/desktop/desktop_widget_factory.cpp',
  'src/shell/desktop/desktop_widget_settings_registry.cpp',
  'src/shell/desktop/desktop_widgets_controller.cpp',
  'src/shell/desktop/desktop_widgets_editor.cpp',
  'src/shell/desktop/desktop_widgets_editor_settings.cpp',
  'src/shell/desktop/desktop_widgets_host.cpp',
  'src/shell/desktop/widgets/desktop_audio_visualizer_widget.cpp',
  'src/shell/desktop/widgets/desktop_clock_widget.cpp',
  'src/shell/desktop/widgets/desktop_media_player_widget.cpp',
  'src/shell/desktop/widgets/desktop_sticker_widget.cpp',
  'src/shell/desktop/widgets/desktop_sysmon_widget.cpp',
  'src/shell/desktop/widgets/desktop_weather_widget.cpp',
  'src/shell/clipboard/clipboard_panel.cpp',
  'src/shell/control_center/calendar_tab.cpp',
  'src/shell/control_center/audio_tab.cpp',
  'src/shell/control_center/bluetooth_tab.cpp',
  'src/shell/control_center/control_center_panel.cpp',
  'src/shell/control_center/display_tab.cpp',
  'src/shell/control_center/media_tab.cpp',
  'src/shell/control_center/network_tab.cpp',
  'src/shell/control_center/notifications_tab.cpp',
  'src/shell/control_center/screen_time_tab.cpp',
  'src/shell/control_center/home_tab.cpp',
  'src/shell/control_center/shortcut_registry.cpp',
  'src/shell/control_center/system_tab.cpp',
  'src/shell/control_center/tab.cpp',
  'src/shell/control_center/weather_tab.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/ui/dialogs/color_picker_dialog.cpp',
  'src/ui/dialogs/color_picker_dialog_popup.cpp',
  'src/ui/dialogs/dialog_popup_host.cpp',
  'src/ui/dialogs/file_dialog.cpp',
  'src/ui/dialogs/file_dialog_popup.cpp',
  'src/ui/dialogs/file_dialog_view.cpp',
  'src/ui/dialogs/file_entry_row.cpp',
  'src/ui/dialogs/file_entry_tile.cpp',
  'src/ui/dialogs/glyph_picker_dialog.cpp',
  'src/ui/dialogs/glyph_picker_dialog_popup.cpp',
  'src/ui/dialogs/layer_popup_host.cpp',
  'src/shell/launcher/launcher_panel.cpp',
  'src/shell/lockscreen/lock_screen.cpp',
  'src/shell/lockscreen/lock_surface.cpp',
  'src/shell/notification/notification_toast.cpp',
  'src/shell/osd/audio_osd.cpp',
  'src/shell/osd/brightness_osd.cpp',
  'src/shell/osd/lock_keys_osd.cpp',
  'src/shell/osd/keyboard_layout_osd.cpp',
  'src/shell/osd/osd_overlay.cpp',
  'src/shell/tooltip/tooltip_manager.cpp',
  'src/shell/screen_corners/screen_corners.cpp',
  'src/shell/panel/panel_click_shield.cpp',
  'src/wayland/hyprland/focus_grab_service.cpp',
  'src/shell/panel/panel_manager.cpp',
  'src/shell/polkit/polkit_panel.cpp',
  'src/shell/session/session_panel.cpp',
  'src/shell/setup_wizard/setup_wizard_panel.cpp',
  'src/shell/surface_shadow.cpp',
  'src/system/telemetry_service.cpp',
  'src/shell/settings/bar_widget_editor.cpp',
  'src/shell/settings/color_spec_picker.cpp',
  'src/shell/settings/config_export_dialog_popup.cpp',
  'src/shell/settings/settings_content.cpp',
  'src/shell/settings/settings_bar_management.cpp',
  'src/shell/settings/settings_registry.cpp',
  'src/shell/settings/settings_sidebar.cpp',
  'src/shell/settings/widget_settings_registry.cpp',
  'src/shell/settings/widget_add_popup.cpp',
  'src/shell/settings/search_picker_popup.cpp',
  'src/shell/settings/session_actions_editor_popup.cpp',
  'src/shell/settings/settings_window_mutations.cpp',
  'src/shell/settings/settings_window_popups.cpp',
  'src/shell/settings/settings_window_scene.cpp',
  'src/shell/settings/settings_window.cpp',
  'src/shell/test/test_panel.cpp',
  'src/shell/tray/tray_drawer_panel.cpp',
  'src/shell/tray/tray_menu.cpp',
  'src/shell/backdrop/backdrop.cpp',
  'src/shell/backdrop/backdrop_surface.cpp',
  'src/shell/overview/overview_launcher_capture.cpp',
  'src/system/app_identity.cpp',
  'src/shell/wallpaper/wallpaper.cpp',
  'src/shell/wallpaper/panel/wallpaper_panel.cpp',
  'src/shell/wallpaper/panel/wallpaper_scanner.cpp',
  'src/shell/wallpaper/panel/wallpaper_tile.cpp',
  'src/shell/bar/widget.cpp',
  'src/shell/bar/widget_factory.cpp',
  'src/shell/bar/widgets/active_window_widget.cpp',
  'src/shell/bar/widgets/audio_visualizer_widget.cpp',
  'src/shell/bar/widgets/battery_widget.cpp',
  'src/shell/bar/widgets/bluetooth_widget.cpp',
  'src/shell/bar/widgets/control_center_widget.cpp',
  'src/shell/bar/widgets/custom_button_widget.cpp',
  'src/shell/bar/widgets/clipboard_widget.cpp',
  'src/shell/bar/widgets/clock_widget.cpp',
  'src/shell/bar/widgets/debug_indicator_widget.cpp',
  'src/shell/bar/widgets/idle_inhibitor_widget.cpp',
  'src/shell/bar/widgets/keyboard_layout_widget.cpp',
  'src/shell/bar/widgets/lock_keys_widget.cpp',
  'src/shell/bar/widgets/nightlight_widget.cpp',
  'src/shell/bar/widgets/launcher_widget.cpp',
  'src/shell/bar/widgets/media_widget.cpp',
  'src/shell/bar/widgets/network_widget.cpp',
  'src/shell/bar/widgets/notification_widget.cpp',
  'src/shell/bar/widgets/power_profile_widget.cpp',
  'src/shell/bar/widgets/scripted_widget.cpp',
  'src/shell/bar/widgets/session_widget.cpp',
  'src/shell/bar/widgets/settings_widget.cpp',
  'src/shell/bar/widgets/spacer_widget.cpp',
  'src/shell/bar/widgets/sysmon_widget.cpp',
  'src/shell/bar/widgets/taskbar_widget.cpp',
  'src/shell/bar/widgets/test_widget.cpp',
  'src/shell/bar/widgets/theme_mode_widget.cpp',
  'src/shell/bar/widgets/tray_widget.cpp',
  'src/shell/bar/widgets/volume_widget.cpp',
  'src/shell/bar/widgets/brightness_widget.cpp',
  'src/shell/bar/widgets/wallpaper_widget.cpp',
  'src/shell/bar/widgets/weather_widget.cpp',
  'src/shell/bar/widgets/workspaces_widget.cpp',
  'src/system/dependency_service.cpp',
  'src/system/desktop_entry.cpp',
  'src/system/brightness_service.cpp',
  'src/system/lock_keys_service.cpp',
  'src/system/distro_info.cpp',
  'src/system/format_units.cpp',
  'src/system/hardware_info.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/internal_app_metadata.cpp',
  'src/system/gamma_service.cpp',
  'src/system/rfkill_helper.cpp',
  'src/system/screen_time_service.cpp',
  'src/system/system_monitor_service.cpp',
  'src/system/weather_service.cpp',
  'src/time/time_format.cpp',
  'src/time/time_service.cpp',
  'src/ui/controls/audio_spectrum.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/color_picker.cpp',
  'src/ui/controls/chip.cpp',
  'src/ui/controls/collapsible.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/context_menu_popup.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/glyph_picker.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/ui/controls/grid_view.cpp',
  'src/ui/controls/grid_tile.cpp',
  'src/ui/controls/image.cpp',
  'src/ui/controls/input.cpp',
  'src/core/keybind_matcher.cpp',
  'src/ui/controls/keybind_recorder.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/list_editor.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/search_picker.cpp',
  'src/ui/controls/screen_corner.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/select.cpp',
  'src/ui/controls/select_dropdown_popup.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/builders.cpp',
  'src/ui/palette.cpp',
  'src/ui/popup_chrome.cpp',
  'src/ui/style.cpp',
  'src/util/fuzzy_match.cpp',
  'src/wayland/clipboard_service.cpp',
  'src/wayland/virtual_keyboard_service.cpp',
  'src/compositors/compositor_platform.cpp',
  'src/compositors/compositor_detect.cpp',
  'src/compositors/compositor_runtime.cpp',
  'src/compositors/ext_workspace/ext_workspace_output_backend.cpp',
  'src/compositors/ext_workspace/ext_workspace_backend.cpp',
  'src/compositors/hyprland/hyprland_runtime.cpp',
  'src/compositors/hyprland/hyprland_event_handler.cpp',
  'src/compositors/hyprland/hyprland_output_backend.cpp',
  'src/compositors/hyprland/hyprland_workspace_backend.cpp',
  'src/compositors/hyprland/hyprland_keyboard_backend.cpp',
  'src/compositors/hyprland/hyprland_window_id.cpp',
  'src/compositors/hyprland/hyprland_toplevel_mapping.cpp',
  'src/compositors/mango/mango_keyboard_backend.cpp',
  'src/compositors/mango/mango_output_backend.cpp',
  'src/compositors/mango/mango_workspace_backend.cpp',
  'src/compositors/niri/niri_runtime.cpp',
  'src/compositors/niri/niri_output_backend.cpp',
  'src/compositors/niri/niri_keyboard_backend.cpp',
  'src/compositors/niri/niri_workspace_backend.cpp',
  'src/compositors/triad/triad_runtime.cpp',
  'src/compositors/triad/triad_output_backend.cpp',
  'src/compositors/triad/triad_keyboard_backend.cpp',
  'src/compositors/triad/triad_workspace_backend.cpp',
  'src/compositors/sway/sway_runtime.cpp',
  'src/compositors/sway/sway_keyboard_backend.cpp',
  'src/compositors/sway/sway_output_backend.cpp',
  'src/compositors/sway/sway_workspace_backend.cpp',
  'src/wayland/layer_surface.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/subsurface.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_seat.cpp',
  'src/wayland/wayland_toplevels.cpp',
  'src/wayland/ext_foreign_toplevels.cpp',
  'src/wayland/wayland_workspaces.cpp',
  'src/theme/scheme.cpp',
  'src/theme/color.cpp',
  'src/theme/contrast.cpp',
  'src/theme/image_loader.cpp',
  'src/theme/palette_generator.cpp',
  'src/theme/m3_schemes.cpp',
  'src/theme/custom_schemes.cpp',
  'src/theme/builtin_palettes.cpp',
  'src/theme/builtin_templates.cpp',
  'src/theme/community_palettes.cpp',
  'src/theme/community_templates.cpp',
  'src/theme/custom_palettes.cpp',
  'src/theme/fixed_palette.cpp',
  'src/theme/template_apply_service.cpp',
  'src/theme/theme_service.cpp',
  'src/theme/json_output.cpp',
  'src/theme/template_engine.cpp',
  'src/theme/cli.cpp',
  'src/ipc/cli.cpp',
)

if sanitize
  _noctalia_sources += files('src/pipewire/pipewire_lsan.cpp')
endif

git_program = find_program('git', required: false)
if git_program.found()
  _git_revision_h = vcs_tag(
    input: 'src/core/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  _git_revision_config = configuration_data()
  _git_revision_config.set('VCS_TAG', 'unknown')
  _git_revision_h = configure_file(
    input: 'src/core/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    configuration: _git_revision_config,
  )
endif

# ── Include directories ────────────────────────────────────────────────────────
_noctalia_inc = [
  include_directories('src'),
  include_directories('third_party/tomlplusplus'),
  include_directories('third_party/wuffs', is_system: true),
  # nlohmann is header-only and uses GCC extensions in some paths; treat as system.
  include_directories('third_party/nlohmann', is_system: true),
  # '.' adds both the source root and the corresponding build-dir root, so
  # generated protocol headers (written to the build root) are found by #include.
  include_directories('.'),
]

# ── Executable ────────────────────────────────────────────────────────────────
executable('noctalia',
  sources: _noctalia_sources + _protocol_sources + _protocol_headers + [_git_revision_h],
  dependencies: [
    sdbus_cpp_dep,
    wayland_client_dep,
    egl_dep,
    gles2_dep,
    wayland_egl_dep,
    freetype2_dep,
    fontconfig_dep,
    cairo_dep,
    cairo_ft_dep,
    pango_dep,
    pangocairo_dep,
    pangoft2_dep,
    harfbuzz_dep,
    librsvg_dep,
    xkbcommon_dep,
    glib_dep,
    gobject_dep,
    gio_dep,
    polkit_agent_dep,
    polkit_gobject_dep,
    pipewire_dep,
    curl_dep,
    pam_dep,
    tinyexpr_dep,
    drwav_dep,
    fzy_dep,
    mcu_dep,
    stb_dep,
    libwebp_dep,
    luau_dep,
  ] + jemalloc_deps + (dl_dep.found() ? [dl_dep] : []),
  include_directories: _noctalia_inc,
  cpp_args: [
    '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wshadow',
    '-DNOCTALIA_SOURCE_ASSETS_DIR="' + meson.project_source_root() / 'assets' + '"',
    '-DNOCTALIA_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DNOCTALIA_INSTALL_DATADIR="' + get_option('datadir') + '"',
    '-DNOCTALIA_VERSION="' + meson.project_version() + '"',
  ] + jemalloc_cpp_args,
  install: true,
)

install_subdir('assets',
  install_dir: get_option('datadir') / 'noctalia',
)

notification_history_utf8_test = executable('notification_history_utf8_test',
  sources: files(
    'tests/notification_history_utf8_test.cpp',
    'src/core/log.cpp',
    'src/notification/notification_history_store.cpp',
    'src/render/core/image_decoder.cpp',
  ),
  dependencies: [
    libwebp_dep,
  ],
  include_directories: _noctalia_inc,
)

test('notification_history_utf8', notification_history_utf8_test)

animation_manager_test = executable('animation_manager_test',
  sources: files(
    'tests/animation_manager_test.cpp',
    'src/render/animation/animation.cpp',
    'src/render/animation/animation_manager.cpp',
    'src/render/animation/motion_service.cpp',
  ),
  include_directories: _noctalia_inc,
)

test('animation_manager', animation_manager_test)

log_test = executable('log_test',
  sources: files(
    'tests/log_test.cpp',
    'src/core/log.cpp',
  ),
  include_directories: _noctalia_inc,
)

test('log', log_test)

string_utils_test = executable('string_utils_test',
  sources: files(
    'tests/string_utils_test.cpp',
  ),
  include_directories: _noctalia_inc,
)

test('string_utils', string_utils_test)

image_source_log_test = executable('image_source_log_test',
  sources: files(
    'tests/image_source_log_test.cpp',
  ),
  include_directories: _noctalia_inc,
)

test('image_source_log', image_source_log_test)

image_file_loader_data_uri_test = executable('image_file_loader_data_uri_test',
  sources: files(
    'tests/image_file_loader_data_uri_test.cpp',
    'src/render/core/image_file_loader.cpp',
    'src/render/core/image_decoder.cpp',
    'src/render/core/stb_image_resize_impl.cpp',
  ),
  dependencies: [
    cairo_dep,
    librsvg_dep,
    libwebp_dep,
    stb_dep,
  ],
  include_directories: _noctalia_inc,
)

test('image_file_loader_data_uri', image_file_loader_data_uri_test)

ico_decoder_test = executable('ico_decoder_test',
  sources: files(
    'tests/ico_decoder_test.cpp',
    'src/render/core/image_decoder.cpp',
  ),
  dependencies: [
    libwebp_dep,
  ],
  include_directories: _noctalia_inc,
)

test('ico_decoder', ico_decoder_test)

app_identity_test = executable('app_identity_test',
  sources: files(
    'tests/app_identity_test.cpp',
    'src/system/app_identity.cpp',
  ),
  include_directories: _noctalia_inc,
)

test('app_identity', app_identity_test)

wayland_toplevels_identity_test = executable('wayland_toplevels_identity_test',
  sources: files(
    'tests/wayland_toplevels_identity_test.cpp',
    'src/system/app_identity.cpp',
    'src/wayland/wayland_toplevels.cpp',
  ) + _protocol_sources + _protocol_headers,
  dependencies: [
    wayland_client_dep,
  ],
  include_directories: _noctalia_inc,
)

test('wayland_toplevels_identity', wayland_toplevels_identity_test)
