#include "shell/wayland/capture_globals.h"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kScreencopyManagerVersion = 3;
  constexpr std::uint32_t kImageCopyCaptureManagerVersion = 1;
  constexpr std::uint32_t kOutputImageCaptureSourceManagerVersion = 1;
  constexpr std::uint32_t kForeignToplevelImageCaptureSourceManagerVersion = 1;

} // namespace

ScreencopyGlobals::ScreencopyGlobals(WaylandConnection& /*connection*/) {}

void ScreencopyGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == zwlr_screencopy_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kScreencopyManagerVersion);
    m_manager = static_cast<zwlr_screencopy_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, bindVersion)
    );
  }
}

void ScreencopyGlobals::releaseGlobals() {
  if (m_manager != nullptr) {
    zwlr_screencopy_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

ImageCaptureGlobals::ImageCaptureGlobals(WaylandConnection& /*connection*/) {}

void ImageCaptureGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == ext_image_copy_capture_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kImageCopyCaptureManagerVersion);
    m_copyCaptureManager = static_cast<ext_image_copy_capture_manager_v1*>(
        wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interface == ext_output_image_capture_source_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kOutputImageCaptureSourceManagerVersion);
    m_outputSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(
        wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interface == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kForeignToplevelImageCaptureSourceManagerVersion);
    m_foreignToplevelSourceManager = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
        wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, bindVersion)
    );
  }
}

void ImageCaptureGlobals::releaseGlobals() {
  if (m_copyCaptureManager != nullptr) {
    ext_image_copy_capture_manager_v1_destroy(m_copyCaptureManager);
    m_copyCaptureManager = nullptr;
  }
  if (m_outputSourceManager != nullptr) {
    ext_output_image_capture_source_manager_v1_destroy(m_outputSourceManager);
    m_outputSourceManager = nullptr;
  }
  if (m_foreignToplevelSourceManager != nullptr) {
    ext_foreign_toplevel_image_capture_source_manager_v1_destroy(m_foreignToplevelSourceManager);
    m_foreignToplevelSourceManager = nullptr;
  }
}
