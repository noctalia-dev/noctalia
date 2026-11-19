#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CompositorPlatform;
class ConfigService;
class IconResolver;
class RenderContext;
struct DockConfig;
struct wl_output;
struct zwlr_layer_surface_v1;

namespace shell::dock {

  struct DockPopup;
  struct DockPopupAnchor;

  struct DockStackFile {
    std::filesystem::path path;
    bool directory = false;
  };

  struct DockStackCallbacks {
    std::function<void(const std::string& path)> openPath;
    std::function<void()> closeMenu;
  };

  // Newest entries of `folder` by modification time, skipping hidden files and partial downloads.
  [[nodiscard]] std::vector<DockStackFile> listStackFiles(const std::filesystem::path& folder, std::size_t limit);

  // Image to show for a stack file: the file itself for pictures, else its type icon.
  [[nodiscard]] std::string
  stackFileImagePath(IconResolver& iconResolver, const DockStackFile& file, int targetSize, bool& thumbnail);

  // Popup with a stack folder's newest files; clicking one opens it and dragging one drops it into other apps.
  [[nodiscard]] std::unique_ptr<DockPopup> createStackPopup(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext, IconResolver& iconResolver,
      zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, const DockConfig& dockConfig,
      const std::string& folder, const std::optional<DockPopupAnchor>& anchor, const DockStackCallbacks& callbacks
  );

  // Slides a stack popup back into the dock. `onDone` runs inside the popup's own animation, so it must defer
  // destroying the popup.
  void closeStackPopup(DockPopup& popup, std::function<void()> onDone);

} // namespace shell::dock
