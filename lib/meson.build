# noctalia-ui: UI controls, renderer, Wayland transport, and core utilities
# shared by the shell. lib/ is its include root; it never sees src/.

# ── Wayland protocols bound by the library ────────────────────────────────────
# The shell generates the protocols only its own extensions bind.
_noctalia_ui_protocol_sources = []
noctalia_ui_protocol_headers = []

foreach _p : [
  ['xdg-output-unstable-v1',
    wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell',
    wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['xdg-activation-v1',
    wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['fractional-scale-v1',
    wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter',
    wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['text-input-unstable-v3',
    '../protocols/text-input-unstable-v3.xml'],
  ['cursor-shape-v1',
    '../protocols/cursor-shape-v1.xml'],
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
  _noctalia_ui_protocol_sources += _c
  noctalia_ui_protocol_headers += _h
endforeach

# ── Sources ───────────────────────────────────────────────────────────────────
_noctalia_ui_sources = files(
  'core/deferred_call.cpp',
  'core/files/file_watcher.cpp',
  'core/files/resource_paths.cpp',
  'core/inotify/inotify.cpp',
  'core/input/key_chord.cpp',
  'core/input/keybind_matcher.cpp',
  'core/log.cpp',
  'core/process/process.cpp',
  'core/process/process_fds.cpp',
  'core/timer_manager.cpp',
  'core/ui_phase.cpp',
  'i18n/i18n.cpp',
  'i18n/i18n_service.cpp',
  'i18n/language_tag.cpp',
  'render/animation/animation.cpp',
  'render/animation/animation_manager.cpp',
  'render/animation/motion_service.cpp',
  'render/backend/gles_framebuffer.cpp',
  'render/backend/gles_render_backend.cpp',
  'render/backend/gles_texture_manager.cpp',
  'render/backend/render_backend.cpp',
  'render/core/async_texture_cache.cpp',
  'render/core/blur_cache.cpp',
  'render/core/cached_layer.cpp',
  'render/core/color.cpp',
  'render/core/heif_library.cpp',
  'render/core/image_decoder.cpp',
  'render/core/image_encoder.cpp',
  'render/core/image_file_loader.cpp',
  'render/core/image_orientation.cpp',
  'render/core/image_resize.cpp',
  'render/core/shader_program.cpp',
  'render/core/shared_texture_cache.cpp',
  'render/core/stb_image_resize_impl.cpp',
  'render/core/thumbnail_service.cpp',
  'render/gl_shared_context.cpp',
  'render/programs/blur_program.cpp',
  'render/programs/glyph_program.cpp',
  'render/programs/image_program.cpp',
  'render/programs/rect_program.cpp',
  'render/programs/spinner_program.cpp',
  'render/render_context.cpp',
  'render/render_target.cpp',
  'render/scene/input_area.cpp',
  'render/scene/input_dispatcher.cpp',
  'render/scene/node.cpp',
  'render/text/cairo_glyph_renderer.cpp',
  'render/text/cairo_text_renderer.cpp',
  'render/text/font_registry.cpp',
  'render/text/font_weight_catalog.cpp',
  'render/text/glyph_registry.cpp',
  'ui/app_icon_colorization.cpp',
  'ui/builders.cpp',
  'ui/controls/box.cpp',
  'ui/controls/button.cpp',
  'ui/controls/checkbox.cpp',
  'ui/controls/collapsible.cpp',
  'ui/controls/color_swatch_preview.cpp',
  'ui/controls/context_menu.cpp',
  'ui/controls/context_menu_popup.cpp',
  'ui/controls/drag_source.cpp',
  'ui/controls/drop_zone.cpp',
  'ui/controls/flex.cpp',
  'ui/controls/glyph.cpp',
  'ui/controls/grid_tile.cpp',
  'ui/controls/grid_view.cpp',
  'ui/controls/image.cpp',
  'ui/controls/input.cpp',
  'ui/controls/keybind_recorder.cpp',
  'ui/controls/label.cpp',
  'ui/controls/list_editor.cpp',
  'ui/controls/progress_bar.cpp',
  'ui/controls/radio_button.cpp',
  'ui/controls/range_slider.cpp',
  'ui/controls/roving_list_nav.cpp',
  'ui/controls/scroll_view.cpp',
  'ui/controls/scrollbar.cpp',
  'ui/controls/segmented.cpp',
  'ui/controls/select.cpp',
  'ui/controls/select_dropdown_popup.cpp',
  'ui/controls/separator.cpp',
  'ui/controls/slider.cpp',
  'ui/controls/spacer.cpp',
  'ui/controls/spinner.cpp',
  'ui/controls/stepper.cpp',
  'ui/controls/toggle.cpp',
  'ui/controls/virtual_grid_view.cpp',
  'ui/controls/virtual_list_view.cpp',
  'ui/drag_drop_controller.cpp',
  'ui/palette.cpp',
  'ui/popup_chrome.cpp',
  'ui/scroll_into_view.cpp',
  'ui/split_pane_focus.cpp',
  'ui/style.cpp',
  'ui/surface_shadow.cpp',
  'util/checksum.cpp',
  'util/clamp.cpp',
  'util/string_utils.cpp',
  'wayland/popup_surface.cpp',
  'wayland/subsurface.cpp',
  'wayland/surface.cpp',
  'wayland/text_input_service.cpp',
  'wayland/toplevel_surface.cpp',
  'wayland/wayland_connection.cpp',
  'wayland/wayland_seat.cpp',
)

if sanitize
  _noctalia_ui_sources += files('core/lsan_suppressions.cpp')
endif

# '.' adds both lib/ and its build directory, where the protocol headers above
# are generated.
_noctalia_ui_inc = include_directories('.')

_noctalia_ui_system_dependencies = [
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
  nlohmann_dep,
  tomlplusplus_dep,
  libwebp_dep,
  libwebpdemux_dep,
  libjxl_dep,
  libjxl_threads_dep,
] + (dl_dep.found() ? [dl_dep] : [])

noctalia_ui_lib = static_library('noctalia-ui',
  sources: _noctalia_ui_sources + _noctalia_ui_protocol_sources + noctalia_ui_protocol_headers,
  dependencies: _noctalia_ui_system_dependencies,
  include_directories: [_noctalia_ui_inc] + _wuffs_inc,
  cpp_args: _noctalia_cpp_args,
)

noctalia_ui_dep = declare_dependency(
  link_with: noctalia_ui_lib,
  dependencies: _noctalia_ui_system_dependencies,
  include_directories: _noctalia_ui_inc,
  compile_args: _noctalia_compile_definitions,
  sources: noctalia_ui_protocol_headers,
)
