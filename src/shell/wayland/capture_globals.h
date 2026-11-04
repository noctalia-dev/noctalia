#pragma once

#include "wayland/wayland_extension.h"

class WaylandConnection;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct zwlr_screencopy_manager_v1;

// wlr-screencopy output capture. Separate from ImageCaptureGlobals so a sibling capture connection binds only this.
class ScreencopyGlobals final : public WaylandExtension {
public:
  explicit ScreencopyGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  zwlr_screencopy_manager_v1* manager() const noexcept { return m_manager; }

private:
  zwlr_screencopy_manager_v1* m_manager = nullptr;
};

// ext-image-copy-capture with its output and foreign-toplevel capture sources.
class ImageCaptureGlobals final : public WaylandExtension {
public:
  explicit ImageCaptureGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  ext_image_copy_capture_manager_v1* copyCaptureManager() const noexcept { return m_copyCaptureManager; }
  ext_output_image_capture_source_manager_v1* outputSourceManager() const noexcept { return m_outputSourceManager; }
  ext_foreign_toplevel_image_capture_source_manager_v1* foreignToplevelSourceManager() const noexcept {
    return m_foreignToplevelSourceManager;
  }

private:
  ext_image_copy_capture_manager_v1* m_copyCaptureManager = nullptr;
  ext_output_image_capture_source_manager_v1* m_outputSourceManager = nullptr;
  ext_foreign_toplevel_image_capture_source_manager_v1* m_foreignToplevelSourceManager = nullptr;
};
