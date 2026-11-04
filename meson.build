project('noctalia', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',   # applied selectively to our target; avoids noise in vendored code
    'b_ndebug=if-release',
  ],
)

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')


# Clang requires an opt-in flag to properly use experimental C++ libraries.
# GCC/libstdc++ doesn't need this since these libraries are unconditionally
# exposed.
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>' ) != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Sanitizers are enabled via meson's built-in -Db_sanitize=address,undefined.
# lsan_suppressions.cpp is only needed when address sanitizer is active.
sanitize = get_option('b_sanitize') != 'none'

# Hardened standard-library assertions in debug builds only. A std::clamp(v, lo, hi)
# with hi < lo aborts here (distro packagers enable this in release, so users see
# the same abort), surfacing inverted-bound bugs loudly during development so they
# can be routed through util::clampOrdered. Release stays unhardened on purpose: a
# not-yet-converted raw std::clamp there should degrade to a wrong-but-non-fatal
# value rather than abort a user's session (converted sites log and return lo via
# clampOrdered). The cost is a few precondition branches (not the heavy _GLIBCXX_DEBUG).
if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif

# Auto mode defines tests for unsanitized debug builds. `just build` targets
# only the shell executable, while `just test` builds and runs the test targets.
tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') == 'debug' and not sanitize)

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-ffunction-sections', '-fdata-sections',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c', 'cpp'],
  )
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: ['c', 'cpp'])
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
pango_dep           = dependency('pango', version: '>=1.50')
pangocairo_dep      = dependency('pangocairo')
pangoft2_dep        = dependency('pangoft2')
harfbuzz_dep        = dependency('harfbuzz')
librsvg_dep         = dependency('librsvg-2.0')
xkbcommon_dep       = dependency('xkbcommon')
glib_dep            = dependency('glib-2.0')
gobject_dep         = dependency('gobject-2.0')
gio_dep             = dependency('gio-2.0')
libsecret_dep       = dependency('libsecret-1', version: '>=0.20')
libsodium_dep       = dependency('libsodium', version: '>=1.0.18')
polkit_agent_dep    = dependency('polkit-agent-1')
polkit_gobject_dep  = dependency('polkit-gobject-1')
# PipeWire/SPA headers use GCC extensions that trigger -Wpedantic/-Wconversion;
# mark as system so the compiler suppresses those warnings.
pipewire_dep        = dependency('libpipewire-0.3', include_type: 'system')
wireplumber_dep     = dependency('wireplumber-0.5', include_type: 'system')
curl_dep            = dependency('libcurl')
libqalculate_dep    = dependency('libqalculate')
libxml2_dep         = dependency('libxml-2.0')
md4c_dep            = dependency('md4c')
nlohmann_dep        = dependency('nlohmann_json')
tomlplusplus_dep    = dependency('tomlplusplus')
libical_dep         = dependency('libical')
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

# ── System: stb_image_resize2 and stb_image_write (header-only) ─────────────────────────────────
if not cc.has_header('stb/stb_image_resize2.h')
  error('stb/stb_image_resize2.h not found')
endif
if not cc.has_header('stb/stb_image_write.h')
  error('stb/stb_image_write.h not found')
endif

# ── System: libwebp (WebP decode; thumbnail and screenshot encode; demux for animations) ─
libwebp_dep       = dependency('libwebp', required: true)
libwebpdemux_dep  = dependency('libwebpdemux', required: true)

# ── System: libjxl (JPEG XL decode and screenshot encode) ─────────────────────
libjxl_dep          = dependency('libjxl', required: true)
libjxl_threads_dep  = dependency('libjxl_threads', required: true)

# ── System: libsndfile (sound effect decoding) ───────────────────────────────
sndfile_dep = dependency('sndfile', required: true)

# ── Wuffs, fzy, Material Color Utilities, Luau ──────────────────────────────
# Vendored under third_party/ by default. -Dsystem_libraries=true links system
# copies instead. None of them ship pkg-config files, so they are found by
# library and header name using the vendored include layout.
if get_option('system_libraries')
  if not cc.has_header('wuffs-v0.4.c')
    error('system_libraries: wuffs-v0.4.c not found')
  endif
  _wuffs_inc = []

  fzy_dep = cc.find_library('fzy', has_headers: ['fzy/match.h'])
  mcu_dep = cpp.find_library('material-color-utilities', has_headers: ['cpp/cam/hct.h'])
  # Static archive order: dependents before their dependencies.
  luau_dep = declare_dependency(dependencies: [
    cpp.find_library('Luau.Compiler', has_headers: ['luacode.h']),
    cpp.find_library('Luau.Bytecode'),
    cpp.find_library('Luau.Ast'),
    cpp.find_library('Luau.VM', has_headers: ['lua.h', 'lualib.h']),
    cpp.find_library('Luau.Common'),
  ])

  _fzy_selective_dep = fzy_dep
  _mcu_selective_dep = mcu_dep
  _luau_selective_dep = luau_dep
else
  _wuffs_inc = [include_directories('third_party/wuffs', is_system: true)]

  # ── Vendored: fzy matcher (C fuzzy scoring) ──────────────────────────────────
  # match.h sits under include/fzy/ to match the system install layout.
  _fzy_inc = include_directories('third_party/fzy/include', is_system: true)
  _fzy_lib = static_library('fzy',
    'third_party/fzy/src/match.c',
    include_directories: include_directories('third_party/fzy/include/fzy', is_system: true),
    c_args: ['-Wno-pedantic', '-Wno-conversion'],
    override_options: ['warning_level=0'],
  )
  fzy_dep = declare_dependency(
    link_whole: _fzy_lib,
    include_directories: _fzy_inc,
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
    link_whole: _mcu_lib,
    include_directories: include_directories('third_party/material_color_utilities', is_system: true),
  )

  # ── Vendored: Luau (Roblox's Lua fork) ────────────────────────────────────────
  # Built as a quiet static library; only VM + Compiler + Bytecode + Ast + Common are vendored.
  _luau_sources = files(
    'third_party/luau/Common/src/BytecodeWire.cpp',
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
    'third_party/luau/Bytecode/src/BytecodeBuilder.cpp',
    'third_party/luau/Bytecode/src/BytecodeGraph.cpp',
    'third_party/luau/Compiler/src/BuiltinFolding.cpp',
    'third_party/luau/Compiler/src/Builtins.cpp',
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
    'third_party/luau/VM/src/lclass.cpp',
    'third_party/luau/VM/src/lclasslib.cpp',
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
    'third_party/luau/VM/src/lvector.cpp',
    'third_party/luau/VM/src/lveclib.cpp',
    'third_party/luau/VM/src/lvmexecute.cpp',
    'third_party/luau/VM/src/lvmload.cpp',
    'third_party/luau/VM/src/lvmutils.cpp',
  )
  _luau_inc = include_directories(
    'third_party/luau/Common/include',
    'third_party/luau/Ast/include',
    'third_party/luau/Bytecode/include',
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
    link_whole: _luau_lib,
    include_directories: _luau_inc,
  )

  # Tests link vendored archives selectively. The application keeps link_whole so
  # release LTO sees every member, but propagating that policy to every small test
  # would embed all vendored code in every test executable.
  _fzy_selective_dep = declare_dependency(
    link_with: _fzy_lib,
    include_directories: _fzy_inc,
  )
  _mcu_selective_dep = declare_dependency(
    link_with: _mcu_lib,
    include_directories: include_directories('third_party/material_color_utilities', is_system: true),
  )
  _luau_selective_dep = declare_dependency(
    link_with: _luau_lib,
    include_directories: _luau_inc,
  )
endif

# ── Wayland protocol generation ───────────────────────────────────────────────
wayland_scanner    = find_program('wayland-scanner')
wayland_protos_dir = wayland_protos_dep.get_variable('pkgdatadir')

# ── Compile arguments ─────────────────────────────────────────────────────────
_noctalia_compile_definitions = [
  '-DNOCTALIA_INSTALL_PREFIX="' + get_option('prefix') + '"',
  '-DNOCTALIA_INSTALL_DATADIR="' + get_option('datadir') + '"',
  '-DNOCTALIA_VERSION="' + meson.project_version() + '"',
] + jemalloc_cpp_args

# PipeWire's pkg-config adds -fno-strict-aliasing and -fno-strict-overflow to
# every target that uses it. lib/ does not depend on PipeWire, so both trees set
# them here and compile under the same rules.
_noctalia_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
  '-fno-strict-aliasing', '-fno-strict-overflow',
] + _noctalia_compile_definitions

# ── UI library ────────────────────────────────────────────────────────────────
# Provides noctalia_ui_lib, noctalia_ui_dep and noctalia_ui_protocol_headers.
subdir('lib')

# ── Shell Wayland protocols ───────────────────────────────────────────────────
# Generates a (header, source) pair from a protocol XML path/target.
# Returns [h_target, c_target]: add c_target to sources.
# The c_target has an order-only dep on h_target so the header exists when
# the generated .c is compiled (it does #include "foo-client-protocol.h").
_protocol_sources = []
_protocol_headers = []

# Helper macro via foreach; each entry: [name, xml_input]
# xml_input can be a string (path) or a custom_target.

foreach _p : [
  # ── system wayland-protocols ──────────────────────────────────────────────
  ['ext-background-effect-v1',
    wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['ext-workspace-v1',
    wayland_protos_dir / 'staging/ext-workspace/ext-workspace-v1.xml'],
  ['ext-foreign-toplevel-list-v1',
    wayland_protos_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'],
  ['ext-data-control-v1',
    wayland_protos_dir / 'staging/ext-data-control/ext-data-control-v1.xml'],
  ['ext-session-lock-v1',
    wayland_protos_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'],
  ['ext-idle-notify-v1',
    wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'],
  ['ext-image-capture-source-v1',
    wayland_protos_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml'],
  ['ext-image-copy-capture-v1',
    wayland_protos_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'],
  ['idle-inhibit-unstable-v1',
    wayland_protos_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],

  # ── vendored / project-local protocols (loaded from protocols/) ───────────
  ['virtual-keyboard-unstable-v1',
    'protocols/virtual-keyboard-unstable-v1.xml'],
  ['dwl-ipc-unstable-v2',
    'protocols/dwl-ipc-unstable-v2.xml'],
  ['org-kde-plasma-virtual-desktop',
    'protocols/org-kde-plasma-virtual-desktop.xml'],
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
  ['wlr-screencopy-unstable-v1',
    'protocols/wlr-screencopy-unstable-v1.xml'],
  ['wlr-output-management-unstable-v1',
    'protocols/wlr-output-management-unstable-v1.xml'],
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
  'src/app/application_events.cpp',
  'src/app/application_ipc.cpp',
  'src/app/application_plugins.cpp',
  'src/app/application_poll_sources.cpp',
  'src/app/application_services.cpp',
  'src/app/application_ui.cpp',
  'src/app/build_info.cpp',
  'src/app/main_loop.cpp',
  'src/app/single_instance_lock.cpp',
  'src/auth/fingerprint_authenticator.cpp',
  'src/auth/pam_authenticator.cpp',
  'src/calendar/caldav_client.cpp',
  'src/calendar/caldav_discovery.cpp',
  'src/calendar/calendar_cache.cpp',
  'src/calendar/calendar_credential_store.cpp',
  'src/calendar/calendar_discovery_state.cpp',
  'src/calendar/calendar_reminder_monitor.cpp',
  'src/calendar/calendar_reminders.cpp',
  'src/calendar/calendar_service.cpp',
  'src/calendar/event_link.cpp',
  'src/calendar/event_merge.cpp',
  'src/calendar/google_client.cpp',
  'src/calendar/google_oauth.cpp',
  'src/calendar/ical_parser.cpp',
  'src/calendar/sync_failure.cpp',
  'src/calendar/vdir_reader.cpp',
  'src/config/atomic_file.cpp',
  'src/config/config_credentials.cpp',
  'src/config/config_export.cpp',
  'src/config/config_merge.cpp',
  'src/config/config_migrations.cpp',
  'src/config/config_origins.cpp',
  'src/config/config_overrides.cpp',
  'src/config/config_service.cpp',
  'src/config/config_types.cpp',
  'src/config/config_validate.cpp',
  'src/config/widget_config.cpp',
  'src/config/schema/config_schema.cpp',
  'src/config/schema/config_sections.cpp',
  'src/config/state_store.cpp',
  'src/config/cli.cpp',
  'src/dbus/accounts/accounts_service.cpp',
  'src/dbus/bluetooth/bluetooth_agent.cpp',
  'src/dbus/bluetooth/bluetooth_service.cpp',
  'src/dbus/bluetooth/bluetooth_uuid_names.cpp',
  'src/dbus/idle/screensaver_service.cpp',
  'src/dbus/logind/logind_service.cpp',
  'src/dbus/modem/modem_manager_service.cpp',
  'src/dbus/mpris/mpris_art.cpp',
  'src/dbus/mpris/mpris_service.cpp',
  'src/dbus/network/external_ip_service.cpp',
  'src/dbus/network/inetwork_service.cpp',
  'src/dbus/network/iwd_diagnostics.cpp',
  'src/dbus/network/iwd_secret_agent.cpp',
  'src/dbus/network/iwd_service.cpp',
  'src/dbus/network/network_display.cpp',
  'src/dbus/network/network_manager_service.cpp',
  'src/dbus/network/network_secret_agent.cpp',
  'src/dbus/network/network_secret_keyring.cpp',
  'src/dbus/network/vpn_auth_dialog.cpp',
  'src/dbus/network/wpa_supplicant_service.cpp',
  'src/dbus/polkit/polkit_agent.cpp',
  'src/dbus/polkit/polkit_session_support.cpp',
  'src/dbus/notification/kde_notification_client.cpp',
  'src/dbus/notification/notification_poll_source.cpp',
  'src/dbus/notification/notification_service.cpp',
  'src/dbus/power/power_profiles_service.cpp',
  'src/dbus/secret/secret_collection_probe.cpp',
  'src/dbus/session_bus.cpp',
  'src/dbus/system_bus.cpp',
  'src/dbus/tray/fcitx_status.cpp',
  'src/dbus/tray/tray_service.cpp',
  'src/dbus/upower/upower_charge_limit_support.cpp',
  'src/dbus/upower/upower_service.cpp',
  'src/debug/debug_service.cpp',
  'src/hooks/battery_hook_state.cpp',
  'src/hooks/hook_manager.cpp',
  'src/idle/idle_inhibitor.cpp',
  'src/idle/idle_grace_overlay.cpp',
  'src/idle/idle_manager.cpp',
  'src/idle/plugin_idle_watches.cpp',
  'src/ipc/ipc_client.cpp',
  'src/ipc/ipc_service.cpp',
  'src/ipc/pixel_stream_client.cpp',
  'src/ipc/pixel_stream_service.cpp',
  'src/launcher/app_provider.cpp',
  'src/launcher/dmenu_cli.cpp',
  'src/launcher/dmenu_ipc.cpp',
  'src/launcher/dmenu_provider.cpp',
  'src/launcher/dmenu_socket_path.cpp',
  'src/launcher/dmenu_stdin_provider.cpp',
  'src/launcher/emoji_provider.cpp',
  'src/launcher/fuzzy_match.cpp',
  'src/launcher/math_provider.cpp',
  'src/launcher/panel_catalog.cpp',
  'src/launcher/panel_provider.cpp',
  'src/launcher/plugin_launcher_provider.cpp',
  'src/launcher/session_provider.cpp',
  'src/launcher/wallpaper_provider.cpp',
  'src/launcher/window_provider.cpp',
  'src/launcher/usage_tracker.cpp',
  'src/net/http_client.cpp',
  'src/net/uri.cpp',
  'src/net/url_open.cpp',
  'src/notification/notification_manager.cpp',
  'src/notification/notification_filter.cpp',
  'src/notification/notification_display_name.cpp',
  'src/notification/notification_history_store.cpp',
  'src/pipewire/pipewire_service.cpp',
  'src/pipewire/pipewire_error.cpp',
  'src/pipewire/audio_glyphs.cpp',
  'src/pipewire/audio_media_class.cpp',
  'src/pipewire/audio_route_selection.cpp',
  'src/pipewire/privacy_capture_classification.cpp',
  'src/pipewire/wireplumber_mixer.cpp',
  'src/pipewire/privacy_filter.cpp',
  'src/pipewire/pipewire_spectrum.cpp',
  'src/pipewire/sound_player.cpp',
  'src/scripting/luau_host.cpp',
  'src/scripting/plugin_catalog.cpp',
  'src/scripting/plugin_file_cache.cpp',
  'src/scripting/plugin_git.cpp',
  'src/scripting/plugin_i18n.cpp',
  'src/scripting/plugin_ipc.cpp',
  'src/scripting/plugin_lint.cpp',
  'src/scripting/plugin_manager.cpp',
  'src/scripting/plugin_recommendations.cpp',
  'src/scripting/plugin_manifest.cpp',
  'src/scripting/plugin_layer_config.cpp',
  'src/scripting/plugin_panel_shell.cpp',
  'src/scripting/plugin_registry.cpp',
  'src/scripting/plugin_script_watcher.cpp',
  'src/scripting/plugin_service_host.cpp',
  'src/scripting/plugin_source_locks.cpp',
  'src/scripting/plugin_source_paths.cpp',
  'src/scripting/plugin_state_store.cpp',
  'src/scripting/script_io_pool.cpp',
  'src/scripting/script_runtime.cpp',
  'src/scripting/script_worker_pool.cpp',
  'src/scripting/plugin_bindings.cpp',
  'src/security/encrypted_file_store.cpp',
  'src/security/secret_store.cpp',
  'src/security/secure_buffer.cpp',
  'src/security/storage_key_provider.cpp',
  'src/system/battery_warning_monitor.cpp',
  'src/shell/bar/bar.cpp',
  'src/shell/clipboard/clipboard_paste.cpp',
  'src/shell/dock/dock.cpp',
  'src/shell/dock/dock_context_menu.cpp',
  'src/shell/dock/dock_geometry.cpp',
  'src/shell/dock/dock_instance.cpp',
  'src/shell/dock/dock_items.cpp',
  'src/shell/dock/icon_shadow.cpp',
  'src/shell/dock/dock_model.cpp',
  'src/shell/dock/pinned_apps.cpp',
  'src/shell/desktop/desktop_widget.cpp',
  'src/shell/desktop/desktop_widget_factory.cpp',
  'src/shell/desktop/desktop_widget_settings_registry.cpp',
  'src/shell/desktop/desktop_widgets_controller.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor_types.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor_settings.cpp',
  'src/shell/desktop/desktop_widgets_host.cpp',
  'src/shell/lockscreen/lockscreen_login_box.cpp',
  'src/shell/lockscreen/lockscreen_widgets_controller.cpp',
  'src/shell/lockscreen/lockscreen_widgets_host.cpp',
  'src/shell/desktop/widgets/desktop_audio_visualizer_widget.cpp',
  'src/shell/desktop/widgets/desktop_button_widget.cpp',
  'src/shell/desktop/widgets/desktop_calendar_widget.cpp',
  'src/shell/desktop/widgets/desktop_clock_widget.cpp',
  'src/shell/desktop/widgets/desktop_fancy_audio_visualizer_widget.cpp',
  'src/shell/desktop/widgets/desktop_label_widget.cpp',
  'src/shell/desktop/widgets/desktop_login_box_widget.cpp',
  'src/shell/desktop/widgets/desktop_media_player_widget.cpp',
  'src/shell/desktop/widgets/desktop_sticker_widget.cpp',
  'src/shell/desktop/widgets/desktop_sysmon_widget.cpp',
  'src/shell/desktop/widgets/desktop_volume_widget.cpp',
  'src/shell/desktop/widgets/plugin_desktop_widget.cpp',
  'src/shell/desktop/widgets/desktop_weather_widget.cpp',
  'src/shell/clipboard/clipboard_panel.cpp',
  'src/shell/control_center/tabs/calendar_tab.cpp',
  'src/shell/control_center/tabs/audio_tab.cpp',
  'src/shell/control_center/tabs/bluetooth_tab.cpp',
  'src/shell/control_center/control_center_panel.cpp',
  'src/shell/control_center/tabs/monitor_tab.cpp',
  'src/shell/control_center/tabs/media_tab.cpp',
  'src/shell/control_center/tabs/network_tab.cpp',
  'src/shell/control_center/tabs/notifications_tab.cpp',
  'src/shell/control_center/tabs/power_tab.cpp',
  'src/shell/control_center/tabs/screen_time_tab.cpp',
  'src/shell/control_center/tabs/home_tab.cpp',
  'src/shell/control_center/plugin_shortcut.cpp',
  'src/shell/control_center/shortcut_registry.cpp',
  'src/shell/control_center/tabs/system_tab.cpp',
  'src/shell/control_center/tab.cpp',
  'src/shell/control_center/tabs/weather_tab.cpp',
  'src/shell/launcher/launcher_panel.cpp',
  'src/shell/switcher/window_switcher.cpp',
  'src/shell/switcher/window_switcher_style.cpp',
  'src/shell/switcher/window_switcher_carousel_style.cpp',
  'src/shell/switcher/window_switcher_compact_style.cpp',
  'src/shell/switcher/window_switcher_icons_style.cpp',
  'src/shell/switcher/window_switcher_tile.cpp',
  'src/shell/profile/avatar_path.cpp',
  'src/shell/greeter/greeter_appearance_sync.cpp',
  'src/shell/lockscreen/lock_screen.cpp',
  'src/shell/lockscreen/lockscreen_session_actions.cpp',
  'src/shell/lockscreen/lock_surface.cpp',
  'src/shell/lockscreen/lockscreen_transition_cover.cpp',
  'src/shell/notification/notification_toast.cpp',
  'src/shell/osd/audio_osd.cpp',
  'src/shell/osd/media_osd.cpp',
  'src/shell/osd/brightness_osd.cpp',
  'src/shell/osd/keyboard_backlight_osd.cpp',
  'src/shell/osd/lock_keys_osd.cpp',
  'src/shell/osd/keyboard_layout_osd.cpp',
  'src/shell/osd/privacy_osd.cpp',
  'src/shell/osd/osd_overlay.cpp',
  'src/shell/tooltip/tooltip_manager.cpp',
  'src/shell/hot_corners/hot_corners.cpp',
  'src/shell/screen_corners/screen_corner.cpp',
  'src/shell/screen_corners/screen_corners.cpp',
  'src/shell/panel/panel_click_shield.cpp',
  'src/shell/wayland/hyprland/focus_grab_service.cpp',
  'src/shell/panel/panel_manager.cpp',
  'src/shell/panel/persistent_panel_host.cpp',
  'src/shell/panel/plugin_panel.cpp',
  'src/shell/plugin_layer/plugin_layer.cpp',
  'src/shell/plugin_layer/plugin_layer_host.cpp',
  'src/shell/polkit/polkit_panel.cpp',
  'src/shell/render/audio_spectrum_node.cpp',
  'src/shell/render/countdown_ring_node.cpp',
  'src/shell/render/fancy_audio_visualizer_node.cpp',
  'src/shell/render/graph_node.cpp',
  'src/shell/render/lockscreen_transition_node.cpp',
  'src/shell/render/programs/audio_spectrum_program.cpp',
  'src/shell/render/programs/countdown_ring_program.cpp',
  'src/shell/render/programs/fancy_audio_visualizer_program.cpp',
  'src/shell/render/programs/graph_program.cpp',
  'src/shell/render/programs/lockscreen_transition_program.cpp',
  'src/shell/render/programs/screen_corner_program.cpp',
  'src/shell/render/programs/wallpaper_mask_program.cpp',
  'src/shell/render/programs/wallpaper_program.cpp',
  'src/shell/render/programs/weather_effect_program.cpp',
  'src/shell/render/screen_corner_node.cpp',
  'src/shell/render/wallpaper_mask_node.cpp',
  'src/shell/render/wallpaper_node.cpp',
  'src/shell/render/weather_effect_node.cpp',
  'src/shell/render/weather_effect_rain_simulation.cpp',
  'src/shell/render/weather_effect_snow_simulation.cpp',
  'src/shell/visuals/audio_visualizer.cpp',
  'src/shell/visuals/fancy_audio_visualizer.cpp',
  'src/shell/visuals/graph.cpp',
  'src/shell/session/countdown_ring.cpp',
  'src/shell/session/session_action_meta.cpp',
  'src/shell/session/session_action_runner.cpp',
  'src/shell/session/session_ipc.cpp',
  'src/shell/session/session_panel.cpp',
  'src/shell/setup_wizard/setup_wizard_panel.cpp',
  'src/system/telemetry_service.cpp',
  'src/shell/settings/bar_widget_editor.cpp',
  'src/shell/settings/color_spec_picker.cpp',
  'src/shell/settings/config_export_dialog_modal.cpp',
  'src/shell/settings/settings_content.cpp',
  'src/shell/settings/settings_content_common.cpp',
  'src/shell/settings/settings_content_plugins.cpp',
  'src/shell/settings/path_browse.cpp',
  'src/shell/settings/plugin_store_content.cpp',
  'src/shell/settings/plugin_store_tile.cpp',
  'src/shell/settings/template_store_content.cpp',
  'src/shell/settings/template_store_tile.cpp',
  'src/shell/settings/settings_control_factory.cpp',
  'src/shell/settings/settings_content_idle_behavior.cpp',
  'src/shell/settings/settings_content_notification_filter.cpp',
  'src/shell/settings/settings_content_session_action.cpp',
  'src/shell/settings/settings_bar_management.cpp',
  'src/shell/settings/settings_registry.cpp',
  'src/shell/settings/settings_sidebar.cpp',
  'src/shell/settings/font_family_catalog.cpp',
  'src/shell/settings/font_weight_catalog.cpp',
  'src/shell/settings/widget_settings_registry.cpp',
  'src/shell/settings/widget_add_popup.cpp',
  'src/shell/settings/settings_dialog_presenter.cpp',
  'src/shell/settings/settings_modal_host.cpp',
  'src/shell/settings/settings_sheet.cpp',
  'src/shell/settings/settings_sheet_modal.cpp',
  'src/shell/settings/settings_window_mutations.cpp',
  'src/shell/settings/settings_window_popups.cpp',
  'src/shell/settings/settings_window_palette.cpp',
  'src/shell/settings/settings_window_scene.cpp',
  'src/shell/settings/settings_window.cpp',
  'src/shell/test/test_panel.cpp',
  'src/shell/tray/tray_drawer_panel.cpp',
  'src/shell/tray/tray_settings.cpp',
  'src/shell/tray/tray_menu.cpp',
  'src/shell/tray/tray_theme_path_index.cpp',
  'src/shell/backdrop/backdrop.cpp',
  'src/shell/backdrop/backdrop_surface.cpp',
  'src/shell/backdrop/wallpaper_renderer.cpp',
  'src/shell/overview/overview_launcher_capture.cpp',
  'src/system/app_identity.cpp',
  'src/shell/wallpaper/wallpaper_paths.cpp',
  'src/shell/wallpaper/wallpaper_geometry.cpp',
  'src/shell/wallpaper/wallpaper_shuffle_state.cpp',
  'src/shell/wallpaper/wallpaper.cpp',
  'src/shell/wallpaper/panel/wallpaper_panel.cpp',
  'src/shell/wallpaper/panel/wallpaper_scanner.cpp',
  'src/shell/wallpaper/panel/wallpaper_tile.cpp',
  'src/shell/keyboard_layout_label.cpp',
  'src/shell/app_icon_colorization_settings.cpp',
  'src/shell/controls/color_picker.cpp',
  'src/shell/controls/glyph_picker.cpp',
  'src/shell/controls/markdown_view.cpp',
  'src/shell/controls/search_picker.cpp',
  'src/shell/dialogs/color_picker_dialog.cpp',
  'src/shell/dialogs/color_picker_dialog_popup.cpp',
  'src/shell/dialogs/dialog_popup_host.cpp',
  'src/shell/dialogs/file_dialog.cpp',
  'src/shell/dialogs/file_dialog_popup.cpp',
  'src/shell/dialogs/file_dialog_view.cpp',
  'src/shell/dialogs/file_entry_row.cpp',
  'src/shell/dialogs/file_entry_tile.cpp',
  'src/shell/dialogs/glyph_picker_dialog.cpp',
  'src/shell/dialogs/glyph_picker_dialog_popup.cpp',
  'src/shell/dialogs/layer_popup_host.cpp',
  'src/shell/dialogs/search_picker_popup.cpp',
  'src/shell/bar/widget.cpp',
  'src/shell/bar/widget_action.cpp',
  'src/shell/bar/widget_action_dispatcher.cpp',
  'src/shell/bar/widget_custom_image.cpp',
  'src/shell/bar/widget_factory.cpp',
  'src/shell/bar/widget_gesture.cpp',
  'src/shell/bar/widget_gesture_defaults.cpp',
  'src/shell/bar/widgets/active_window_widget.cpp',
  'src/shell/bar/widgets/active_window_widget_definition.cpp',
  'src/shell/bar/widgets/audio_visualizer_widget.cpp',
  'src/shell/bar/widgets/audio_visualizer_widget_definition.cpp',
  'src/shell/bar/widgets/battery_widget.cpp',
  'src/shell/bar/widgets/battery_widget_definition.cpp',
  'src/shell/bar/widgets/bluetooth_widget.cpp',
  'src/shell/bar/widgets/bluetooth_widget_definition.cpp',
  'src/shell/bar/widgets/control_center_widget.cpp',
  'src/shell/bar/widgets/control_center_widget_definition.cpp',
  'src/shell/bar/widgets/custom_button_widget.cpp',
  'src/shell/bar/widgets/custom_button_widget_definition.cpp',
  'src/shell/bar/widgets/clipboard_widget.cpp',
  'src/shell/bar/widgets/clipboard_widget_definition.cpp',
  'src/shell/bar/widgets/clock_widget.cpp',
  'src/shell/bar/widgets/clock_widget_definition.cpp',
  'src/shell/bar/widgets/debug_indicator_widget.cpp',
  'src/shell/bar/widgets/idle_inhibitor_widget.cpp',
  'src/shell/bar/widgets/caffeine_widget_definition.cpp',
  'src/shell/bar/widgets/keyboard_layout_widget.cpp',
  'src/shell/bar/widgets/keyboard_layout_widget_definition.cpp',
  'src/shell/bar/widgets/lock_keys_widget.cpp',
  'src/shell/bar/widgets/lock_keys_widget_definition.cpp',
  'src/shell/bar/widgets/nightlight_widget.cpp',
  'src/shell/bar/widgets/nightlight_widget_definition.cpp',
  'src/shell/bar/widgets/launcher_widget.cpp',
  'src/shell/bar/widgets/launcher_widget_definition.cpp',
  'src/shell/bar/widgets/media_widget.cpp',
  'src/shell/bar/widgets/media_widget_definition.cpp',
  'src/shell/bar/widgets/network_widget.cpp',
  'src/shell/bar/widgets/network_widget_definition.cpp',
  'src/shell/bar/widgets/notification_widget.cpp',
  'src/shell/bar/widgets/notification_widget_definition.cpp',
  'src/shell/bar/widgets/plugin_widget.cpp',
  'src/shell/bar/widgets/power_profile_widget.cpp',
  'src/shell/bar/widgets/power_profile_widget_definition.cpp',
  'src/shell/bar/widgets/privacy_widget.cpp',
  'src/shell/bar/widgets/privacy_widget_definition.cpp',
  'src/shell/bar/widgets/screenshot_widget_definition.cpp',
  'src/shell/bar/widgets/session_widget.cpp',
  'src/shell/bar/widgets/session_widget_definition.cpp',
  'src/shell/bar/widgets/settings_widget.cpp',
  'src/shell/bar/widgets/settings_widget_definition.cpp',
  'src/shell/bar/widgets/spacer_widget.cpp',
  'src/shell/bar/widgets/spacer_widget_definition.cpp',
  'src/shell/bar/widgets/text_widget.cpp',
  'src/shell/bar/widgets/text_widget_definition.cpp',
  'src/shell/bar/widgets/sysmon_widget.cpp',
  'src/shell/bar/widgets/sysmon_widget_definition.cpp',
  'src/shell/bar/widgets/taskbar_widget.cpp',
  'src/shell/bar/widgets/taskbar_widget_definition.cpp',
  'src/shell/bar/widgets/test_widget.cpp',
  'src/shell/bar/widgets/test_widget_definition.cpp',
  'src/shell/bar/widgets/theme_mode_widget.cpp',
  'src/shell/bar/widgets/theme_mode_widget_definition.cpp',
  'src/shell/bar/widgets/tray_widget.cpp',
  'src/shell/bar/widgets/tray_widget_definition.cpp',
  'src/shell/bar/widgets/volume_widget.cpp',
  'src/shell/bar/widgets/volume_widget_definition.cpp',
  'src/shell/bar/widgets/brightness_widget.cpp',
  'src/shell/bar/widgets/brightness_widget_definition.cpp',
  'src/shell/bar/widgets/wallpaper_widget.cpp',
  'src/shell/bar/widgets/wallpaper_widget_definition.cpp',
  'src/shell/bar/widgets/weather_widget.cpp',
  'src/shell/bar/widgets/weather_widget_definition.cpp',
  'src/shell/bar/widgets/workspaces_widget.cpp',
  'src/shell/bar/widgets/workspaces_widget_definition.cpp',
  'src/system/dependency_service.cpp',
  'src/system/v4l2_camera.cpp',
  'src/system/directory_scanner.cpp',
  'src/system/desktop_entry.cpp',
  'src/system/freedesktop_key_file.cpp',
  'src/system/desktop_entry_launch.cpp',
  'src/system/terminal_launch.cpp',
  'src/system/brightness_service.cpp',
  'src/system/keyboard_backlight_service.cpp',
  'src/system/lock_keys_service.cpp',
  'src/system/distro_info.cpp',
  'src/system/disk_mounts.cpp',
  'src/system/format_units.cpp',
  'src/system/hardware_info.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/icon_theme_poll_source.cpp',
  'src/system/internal_app_metadata.cpp',
  'src/system/easyeffects_service.cpp',
  'src/system/day_night_schedule.cpp',
  'src/system/gamma_service.cpp',
  'src/system/location_service.cpp',
  'src/capture/screencopy_capture.cpp',
  'src/capture/pointer_cursor_capture.cpp',
  'src/capture/toplevel_thumbnail_capture.cpp',
  'src/capture/screencopy_util.cpp',
  'src/capture/screenshot_capture.cpp',
  'src/capture/screenshot_image.cpp',
  'src/capture/screenshot_backdrop.cpp',
  'src/capture/screenshot_service.cpp',
  'src/capture/screenshot_region_overlay.cpp',
  'src/capture/screenshot_region_badge.cpp',
  'src/capture/annotation_document.cpp',
  'src/capture/annotation_raster.cpp',
  'src/capture/annotation_overlay.cpp',
  'src/shell/bar/widgets/screenshot_widget.cpp',
  'src/system/rfkill_helper.cpp',
  'src/system/screen_time_service.cpp',
  'src/system/cpu_stat.cpp',
  'src/system/cpu_freq.cpp',
  'src/system/cpu_temp_sensor.cpp',
  'src/system/intel_gpu.cpp',
  'src/system/system_monitor_service.cpp',
  'src/system/weather_service.cpp',
  'src/time/time_service.cpp',
  'src/time/time_format.cpp',
  'src/shell/calendar/calendar_view.cpp',
  'src/scripting/ui/pixel_stream_view.cpp',
  'src/scripting/ui/ui_tree_reconciler.cpp',
  'src/shell/palette_swatch_preview.cpp',
  'src/shell/wayland/clipboard_service.cpp',
  'src/shell/wayland/virtual_keyboard_service.cpp',
  'src/compositors/compositor_platform.cpp',
  'src/compositors/workspace_alert_service.cpp',
  'src/compositors/compositor_detect.cpp',
  'src/compositors/compositor_runtime.cpp',
  'src/compositors/dwl/dwl_workspace_backend.cpp',
  'src/compositors/ext_workspace/ext_workspace_output_backend.cpp',
  'src/compositors/ext_workspace/ext_workspace_backend.cpp',
  'src/compositors/hyprland/hyprland_runtime.cpp',
  'src/compositors/hyprland/hyprland_event_handler.cpp',
  'src/compositors/hyprland/hyprland_output_backend.cpp',
  'src/compositors/hyprland/hyprland_workspace_backend.cpp',
  'src/compositors/hyprland/hyprland_keyboard_backend.cpp',
  'src/compositors/hyprland/hyprland_window_id.cpp',
  'src/compositors/hyprland/hyprland_toplevel_mapping.cpp',
  'src/compositors/mango/mango_runtime.cpp',
  'src/compositors/mango/mango_keyboard_backend.cpp',
  'src/compositors/mango/mango_output_backend.cpp',
  'src/compositors/mango/mango_workspace_backend.cpp',
  'src/compositors/niri/niri_runtime.cpp',
  'src/compositors/niri/niri_event_handler.cpp',
  'src/compositors/niri/niri_output_backend.cpp',
  'src/compositors/niri/niri_keyboard_backend.cpp',
  'src/compositors/niri/niri_workspace_backend.cpp',
  'src/compositors/umbriel/umbriel_runtime.cpp',
  'src/compositors/umbriel/umbriel_audio_provider.cpp',
  'src/compositors/umbriel/umbriel_event_handler.cpp',
  'src/compositors/umbriel/umbriel_workspace_backend.cpp',
  'src/compositors/umbriel/umbriel_keyboard_backend.cpp',
  'src/compositors/umbriel/umbriel_output_backend.cpp',
  'src/compositors/sway/sway_runtime.cpp',
  'src/compositors/sway/sway_keyboard_backend.cpp',
  'src/compositors/sway/sway_output_backend.cpp',
  'src/compositors/sway/sway_workspace_backend.cpp',
  'src/compositors/kde/kwin_active_window.cpp',
  'src/compositors/kde/kwin_workspace_backend.cpp',
  'src/compositors/labwc/labwc_workspace_backend.cpp',
  'src/shell/wayland/wayland_toplevels.cpp',
  'src/shell/wayland/ext_foreign_toplevels.cpp',
  'src/compositors/wayland_workspaces.cpp',
  'src/shell/wayland/capture_globals.cpp',
  'src/shell/wayland/clipboard_globals.cpp',
  'src/shell/wayland/shell_globals.cpp',
  'src/shell/wayland/compositor_globals.cpp',
  'src/shell/wayland/foreign_toplevels.cpp',
  'src/shell/wayland/output_management_globals.cpp',
  'src/shell/wayland/background_effect_globals.cpp',
  'src/shell/wayland/layer_shell.cpp',
  'src/shell/wayland/layer_surface.cpp',
  'src/shell/wayland/output_probe.cpp',
  'src/theme/scheme.cpp',
  'src/theme/color.cpp',
  'src/theme/contrast.cpp',
  'src/theme/image_loader.cpp',
  'src/theme/palette_generator.cpp',
  'src/theme/palette_transform.cpp',
  'src/theme/m3_schemes.cpp',
  'src/theme/custom_schemes.cpp',
  'src/theme/builtin_palettes.cpp',
  'src/theme/builtin_templates.cpp',
  'src/theme/community_palettes.cpp',
  'src/theme/community_templates.cpp',
  'src/theme/custom_palettes.cpp',
  'src/theme/fixed_palette.cpp',
  'src/theme/fixed_palette_json.cpp',
  'src/theme/template_apply_service.cpp',
  'src/theme/gtk_extensions.cpp',
  'src/theme/theme_service.cpp',
  'src/theme/json_output.cpp',
  'src/theme/kde_color_scheme.cpp',
  'src/theme/firefox_theme/css.cpp',
  'src/theme/firefox_theme/firefox_theme.cpp',
  'src/theme/firefox_theme/manifest.cpp',
  'src/theme/firefox_theme/native_messaging.cpp',
  'src/theme/firefox_theme/settings.cpp',
  'src/theme/hook_runner.cpp',
  'src/theme/template_engine.cpp',
  'src/cli/completions.cpp',
  'src/cli/parse.cpp',
  'src/cli/help.cpp',
  'src/theme/cli.cpp',
  'src/ipc/cli.cpp',
)

git_program = find_program('git', required: false)
if git_program.found()
  _git_revision_h = vcs_tag(
    input: 'src/app/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  _git_revision_config = configuration_data()
  _git_revision_config.set('VCS_TAG', 'unknown')
  _git_revision_h = configure_file(
    input: 'src/app/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    configuration: _git_revision_config,
  )
endif

# Headers clang-tidy needs before any compile has run (see `just lint`).
_generated_headers = noctalia_ui_protocol_headers + _protocol_headers
if git_program.found()
  _generated_headers += _git_revision_h
endif
alias_target('generated-headers', _generated_headers)

# ── Include directories ────────────────────────────────────────────────────────
_noctalia_inc = [
  include_directories('src'),
  # '.' adds both the source root and the corresponding build-dir root, so
  # generated protocol headers (written to the build root) are found by #include.
  include_directories('.'),
]

# ── Application library and executable ────────────────────────────────────────
_noctalia_system_dependencies = [
  sdbus_cpp_dep,
  wayland_client_dep,
  gles2_dep,
  cairo_dep,
  pango_dep,
  pangocairo_dep,
  xkbcommon_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  libsecret_dep,
  libsodium_dep,
  polkit_agent_dep,
  polkit_gobject_dep,
  pipewire_dep,
  wireplumber_dep,
  curl_dep,
  libqalculate_dep,
  pam_dep,
  nlohmann_dep,
  libxml2_dep,
  libical_dep,
  md4c_dep,
  tomlplusplus_dep,
  libwebp_dep,
  sndfile_dep,
] + jemalloc_deps + (dl_dep.found() ? [dl_dep] : [])

_noctalia_dependencies = _noctalia_system_dependencies + [
  fzy_dep,
  mcu_dep,
  luau_dep,
]

_noctalia_test_dependencies = _noctalia_system_dependencies + [
  _fzy_selective_dep,
  _mcu_selective_dep,
  _luau_selective_dep,
  noctalia_ui_dep,
]

# Production sources and generated protocols compile once. The shell links both
# archives whole; tests link them normally so only the objects they exercise
# enter each test executable.
noctalia_core_lib = static_library('noctalia_core',
  sources: _noctalia_sources + _protocol_sources + _protocol_headers + [_git_revision_h],
  dependencies: _noctalia_dependencies + [noctalia_ui_dep],
  include_directories: _noctalia_inc,
  cpp_args: _noctalia_cpp_args,
)

noctalia_core_dep = declare_dependency(
  link_with: noctalia_core_lib,
  dependencies: _noctalia_test_dependencies,
  include_directories: _noctalia_inc,
  compile_args: _noctalia_compile_definitions,
  sources: _protocol_headers,
)

noctalia_exe = executable('noctalia',
  sources: files('src/main.cpp') + _protocol_headers,
  dependencies: _noctalia_dependencies + [noctalia_ui_dep],
  include_directories: _noctalia_inc,
  cpp_args: _noctalia_cpp_args,
  objects: [
    noctalia_core_lib.extract_all_objects(recursive: false),
    noctalia_ui_lib.extract_all_objects(recursive: false),
  ],
  install: true,
)

noctalia_session_config = configuration_data()
noctalia_session_config.set(
  'NOCTALIA_EXECUTABLE',
  get_option('prefix') / get_option('bindir') / 'noctalia',
)
configure_file(
  input: 'assets/noctalia.service.in',
  output: 'noctalia.service',
  configuration: noctalia_session_config,
  install_dir: get_option('datadir') / 'noctalia' / 'assets',
)

install_subdir('assets',
  install_dir: get_option('datadir') / 'noctalia',
  exclude_files: ['dev.noctalia.Noctalia.desktop', 'dev.noctalia.Noctalia.Annotator.desktop', 'noctalia.service.in'],
)

install_data(
  'assets/dev.noctalia.Noctalia.desktop',
  'assets/dev.noctalia.Noctalia.Annotator.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'assets/noctalia.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

if build_tests
  wayland_server_dep = dependency('wayland-server')

  _wlr_screencopy_test_server_h = custom_target('wlr-screencopy-unstable-v1-test-server-header',
    input: 'protocols/wlr-screencopy-unstable-v1.xml',
    output: 'wlr-screencopy-unstable-v1-test-server-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )

  _idle_test_server_h = custom_target('ext-idle-notify-v1-test-server-header',
    input: wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
    output: 'ext-idle-notify-v1-test-server-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )

  # Tests of lib/ code link only the UI library.
  _ui_test_names = [
    'cairo_text_renderer',
    'grid_item_cursor',
    'i18n_language_tag',
    'ico_decoder',
    'image_file_loader_data_uri',
    'image_source_log',
    'inotify',
    'input_area',
    'input_bidi',
    'input_dispatcher',
    'input_password_mode',
    'input_readline_shortcuts',
    'jxl_decoder',
    'log',
    'process',
    'string_utils',
    'virtual_grid_view_gesture',
    'wayland_output_scale',
    'webp_decoder',
  ]

  _cpp_test_names = [
    'annotation_document',
    'annotation_raster',
    'annotation_view',
    'animation_manager',
    'app_identity',
    'audio_glyphs',
    'audio_media_class',
    'audio_route_selection',
    'battery_health',
    'battery_hook_state',
    'battery_warning_threshold',
    'bluetooth_uuid_names',
    'button_layout',
    'caldav_client',
    'calendar_cache_permissions',
    'calendar_credential_store',
    'calendar_discovery_state',
    'calendar_reminder_monitor',
    'calendar_reminders',
    'calendar_view_passed_event',
    'capsule_group_lane_override',
    'capsule_group_reconcile',
    'capsule_group_style_persist',
    'cli_help',
    'cli_parse',
    'cli_schema',
    'clipboard_service',
    'clipboard_storage_permissions',
    'config_migration',
    'config_override_mutation',
    'config_path_resolution',
    'config_schema_roundtrip',
    'config_wallpaper_precedence',
    'config_widget',
    'context_menu_popup',
    'cpu_freq',
    'cpu_stat',
    'cpu_temp_sensor',
    'desktop_entry_launch',
    'desktop_entry_localization',
    'desktop_entry_symlink',
    'desktop_entry_watch',
    'desktop_widget_output',
    'disk_mounts',
    'dock_edge_overlap',
    'dock_icon_shadow',
    'dock_pinned_apps',
    'enterprise_credentials',
    'event_link',
    'event_merge',
    'ext_toplevel_capture_target',
    'fcitx_status',
    'format_units',
    'freedesktop_key_file',
    'firefox_theme_manifest',
    'fixed_palette_json',
    'font_family_catalog',
    'google_client_calendar_list',
    'google_client_reminders',
    'greeter_appearance_sync',
    'gtk_extensions',
    'hook_manager',
    'hyprland_workspace_backend',
    'i18n_supported_languages',
    'ical_parser',
    'icon_resolver',
    'image_orientation',
    'ipc_service',
    'iwd_diagnostics',
    'kde_color_scheme',
    'keyboard_layout_label',
    'labwc_occupancy',
    'lane_selection_token',
    'launcher_category_state',
    'location_service',
    'lockscreen_session_actions',
    'math_provider',
    'monitor_selector',
    'network_display',
    'network_manager_security',
    'network_secret_keyring',
    'niri_workspace_backend',
    'notification_dnd',
    'notification_filter',
    'notification_history_dismiss',
    'notification_history_encryption',
    'notification_history_image_bounds',
    'notification_history_utf8',
    'notification_inline_reply',
    'notification_icon_source',
    'notification_internal_history',
    'path_browse',
    'pipewire_error',
    'umbriel_audio_provider',
    'privacy_capture_classification',
    'plugin_bindings',
    'plugin_catalog',
    'plugin_file_privacy',
    'plugin_git_export',
    'plugin_gpu_monitoring',
    'plugin_i18n',
    'plugin_lifecycle',
    'plugin_manifest',
    'plugin_process',
    'plugin_recommendations',
    'plugin_source_locks',
    'plugin_source_paths',
    'pixel_stream_client',
    'scheme',
    'screenshot_region_badge',
    'screenshot_ipc_request',
    'screenshot_backdrop',
    'scrollbar_geometry',
    'secret_store',
    'security_primitives',
    'settings_search',
    'state_store',
    'surface_blur_region',
    'sway_workspace_backend',
    'system_monitor_service',
    'taskbar_widget',
    'template_apply_notify',
    'template_apply_shutdown',
    'template_hook_async',
    'template_hook_runner',
    'theme_shell_mode',
    'time_format',
    'toplevel_thumbnail_capture',
    'tray_identifier',
    'tray_theme_path_index',
    'ui_tree_reconciler',
    'upower_charge_limit',
    'upower_device_catalog',
    'uri',
    'v4l2_camera',
    'vdir_reader',
    'vpn_auth_dialog',
    'wallpaper_scan_symlink',
    'wallpaper_shuffle_state',
    'wayland_toplevels_identity',
    'widget_action',
    'widget_definition',
    'widget_settings_schema',
    'window_minimize_state',
    'window_switcher_membership',
    'window_switcher_shortcut_state',
    'workspace_alert_service',
    'workspaces_widget',
  ]

  foreach _test_group : [[_ui_test_names, noctalia_ui_dep], [_cpp_test_names, noctalia_core_dep]]
    _test_dep = _test_group[1]
    foreach test_name : _test_group[0]
      if test_name == 'plugin_git_export' and not git_program.found()
        continue
      endif
      test_exe = executable(test_name + '_test',
        sources: files('tests' / (test_name + '_test.cpp')),
        dependencies: _test_dep,
        override_options: ['b_ndebug=false'],
      )
      test_args = []
      if test_name in ['desktop_entry_localization', 'config_wallpaper_precedence']
        test_args = [meson.project_source_root() / 'assets']
      elif test_name == 'i18n_supported_languages'
        test_args = [meson.project_source_root() / 'assets' / 'translations']
      elif test_name == 'settings_search'
        test_args = [meson.project_source_root() / 'assets']
      endif
      test(test_name, test_exe, args: test_args)
      if test_name == 'plugin_recommendations' and git_program.found()
        recommendations_python = find_program('python3', required: false)
        recommendations_openssl = find_program('openssl', required: false)
        if recommendations_python.found() and recommendations_openssl.found()
          test('plugin_recommendations_http', recommendations_python,
            args: [files('tests/plugin_recommendations_http_test.py'), test_exe],
            workdir: meson.project_source_root(),
            timeout: 90,
          )
        endif
      endif
    endforeach
  endforeach

  screencopy_connection_isolation_exe = executable('screencopy_connection_isolation_test',
    sources: files('tests/screencopy_connection_isolation_test.cpp') + [_wlr_screencopy_test_server_h],
    dependencies: [noctalia_core_dep, wayland_server_dep],
    override_options: ['b_ndebug=false'],
  )
  test('screencopy_connection_isolation', screencopy_connection_isolation_exe)

  idle_manager_exe = executable('idle_manager_test',
    sources: files('tests/idle_manager_test.cpp') + [_idle_test_server_h],
    dependencies: [noctalia_core_dep, wayland_server_dep],
    override_options: ['b_ndebug=false'],
  )
  test('idle_manager', idle_manager_exe)

  brightness_service_exe = executable('brightness_service_test',
    sources: files('tests/brightness_service_test.cpp'),
    dependencies: [noctalia_core_dep, wayland_server_dep],
    override_options: ['b_ndebug=false'],
  )
  test('brightness_service', brightness_service_exe, timeout: 60)

  upower_integration_exe = executable('upower_charge_limit_integration_test',
    sources: files('tests/upower_charge_limit_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  secret_collection_probe_integration_exe = executable('secret_collection_probe_integration_test',
    sources: files('tests/secret_collection_probe_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  tray_registration_integration_exe = executable('tray_registration_integration_test',
    sources: files('tests/tray_registration_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  tray_signal_sender_integration_exe = executable('tray_signal_sender_integration_test',
    sources: files('tests/tray_signal_sender_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  fingerprint_integration_exe = executable('fingerprint_authenticator_integration_test',
    sources: files('tests/fingerprint_authenticator_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  dbus_run_session = find_program('dbus-run-session', required: false)
  if dbus_run_session.found()
    url_open_test_exe = executable('url_open_test',
      sources: files('tests/url_open_test.cpp'),
      dependencies: noctalia_core_dep,
      override_options: ['b_ndebug=false'],
    )
    test('url_open', dbus_run_session, args: ['--', url_open_test_exe])
  endif
  if dbus_run_session.found()
    test('upower_charge_limit_integration',
      dbus_run_session,
      args: [
        '--', 'sh', '-c',
        'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"',
        'sh', upower_integration_exe.full_path(),
      ],
      depends: upower_integration_exe,
    )
    test('secret_collection_probe_integration',
      dbus_run_session,
      args: ['--', secret_collection_probe_integration_exe.full_path()],
      depends: secret_collection_probe_integration_exe,
    )
    foreach mode : ['registration', 'discovery', 'external-watcher']
      foreach scenario : ['default', 'ayatana', 'both']
        test('tray_' + mode + '_' + scenario + '_integration',
          dbus_run_session,
          args: ['--', tray_registration_integration_exe.full_path(), mode, scenario],
          depends: tray_registration_integration_exe,
        )
      endforeach
    endforeach
    test('tray_signal_sender_integration',
      dbus_run_session,
      args: ['--', tray_signal_sender_integration_exe.full_path()],
      depends: tray_signal_sender_integration_exe,
    )
    test('fingerprint_authenticator_integration',
      dbus_run_session,
      args: [
        '--', 'sh', '-c',
        'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"',
        'sh', fingerprint_integration_exe.full_path(),
      ],
      depends: fingerprint_integration_exe,
    )
  endif

  bluetooth_property_integration_exe = executable('bluetooth_property_integration_test',
    sources: files('tests/bluetooth_property_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('bluetooth_property_integration',
      dbus_run_session,
      args: [
        '--', 'sh', '-c',
        'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"',
        'sh', bluetooth_property_integration_exe.full_path(),
      ],
      depends: bluetooth_property_integration_exe,
    )
  endif

  bluetooth_connect_pass_integration_exe = executable('bluetooth_connect_pass_integration_test',
    sources: files('tests/bluetooth_connect_pass_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('bluetooth_connect_pass_integration',
      dbus_run_session,
      args: [
        '--', 'sh', '-c',
        'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"',
        'sh', bluetooth_connect_pass_integration_exe.full_path(),
      ],
      depends: bluetooth_connect_pass_integration_exe,
      timeout: 60,
    )
  endif

  notification_reply_integration_exe = executable('notification_reply_integration_test',
    sources: files('tests/notification_reply_integration_test.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  if dbus_run_session.found()
    test('notification_reply_integration',
      dbus_run_session,
      args: ['--', notification_reply_integration_exe.full_path()],
      depends: notification_reply_integration_exe,
    )
  endif

  config_validate_env = environment()
  config_validate_env.set('NOCTALIA_CONFIG_HOME', meson.current_source_dir() / 'tests/config_validate/config-home')
  config_validate_env.set('NOCTALIA_STATE_HOME', meson.current_source_dir() / 'tests/config_validate/state-home')
  config_validate_env.set('NOCTALIA_DATA_HOME', meson.current_source_dir() / 'tests/config_validate/data-home')

  sh_program = find_program('sh')
  test('config_validate_cli',
    sh_program,
    args: ['tests/config_validate_cli_test.sh', noctalia_exe.full_path()],
    depends: noctalia_exe,
    workdir: meson.current_source_dir(),
    env: config_validate_env,
  )

  bash_program = find_program('bash')
  test('template_undo_signal',
    bash_program,
    args: ['tests/template_undo_signal_test.sh', meson.current_source_dir() / 'assets/templates'],
    workdir: meson.current_source_dir(),
  )
  test('umbriel_apply_include_comment',
    bash_program,
    args: ['tests/umbriel_apply_include_comment_test.sh', meson.current_source_dir() / 'assets/templates/umbriel/apply.sh'],
    workdir: meson.current_source_dir(),
  )
  test('compositor_template_config',
    bash_program,
    args: ['tests/compositor_template_config_test.sh', meson.current_source_dir() / 'assets/templates'],
    workdir: meson.current_source_dir(),
  )
endif # build_tests
