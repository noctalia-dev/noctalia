# Shared desktop-widget controls. Configure the consumer's lib/ first (C++23).
# Input: noctalia_ui_dep. Outputs: noctalia_desktop_widgets_dep, noctalia_desktop_widgets_lib.
if not is_variable('noctalia_ui_dep')
  error('noctalia-desktop-widgets requires noctalia_ui_dep; configure lib/ first')
endif

_noctalia_desktop_widgets_inc = include_directories('.')
_noctalia_desktop_widgets_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
  '-fno-strict-aliasing', '-fno-strict-overflow',
]
noctalia_desktop_widgets_lib = static_library(
  'noctalia-desktop-widgets',
  sources: files('label_widget.cpp', 'digital_clock_widget.cpp'),
  include_directories: _noctalia_desktop_widgets_inc,
  dependencies: noctalia_ui_dep,
  cpp_args: _noctalia_desktop_widgets_cpp_args,
  install: false,
)
noctalia_desktop_widgets_dep = declare_dependency(
  link_with: noctalia_desktop_widgets_lib,
  include_directories: _noctalia_desktop_widgets_inc,
  dependencies: noctalia_ui_dep,
)
