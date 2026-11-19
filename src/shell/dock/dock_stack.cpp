#include "shell/dock/dock_stack.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_context_menu.h"
#include "shell/wayland/file_drag.h"
#include "shell/wayland/layer_shell.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <linux/input-event-codes.h>
#include <string_view>
#include <system_error>
#include <utility>

namespace shell::dock {
  namespace {

    constexpr Logger kLog("dock");

    constexpr std::size_t kStackMaxFiles = 12;
    constexpr std::size_t kStackMaxColumns = 4;
    constexpr float kTileWidth = 96.0F;
    constexpr float kTileHeight = 112.0F;
    constexpr float kTileIconSize = 56.0F;
    constexpr float kListWidth = 320.0F;
    constexpr float kRowHeight = 36.0F;
    constexpr float kRowIconSize = 24.0F;
    constexpr float kFooterHeight = 32.0F;
    // Pointer travel, in logical px, that turns a press on a file into a drag.
    constexpr float kDragThreshold = 6.0F;
    constexpr int kDragIconTarget = 96;

    struct ExtensionIcon {
      std::string_view extension;
      std::string_view icon;
    };

    constexpr std::array<std::string_view, 6> kImageExtensions{".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp"};

    constexpr std::array<ExtensionIcon, 22> kExtensionIcons{{
        {".pdf", "application-pdf"},        {".zip", "package-x-generic"},
        {".tar", "package-x-generic"},      {".gz", "package-x-generic"},
        {".xz", "package-x-generic"},       {".zst", "package-x-generic"},
        {".7z", "package-x-generic"},       {".rar", "package-x-generic"},
        {".svg", "image-x-generic"},        {".mp4", "video-x-generic"},
        {".mkv", "video-x-generic"},        {".webm", "video-x-generic"},
        {".mp3", "audio-x-generic"},        {".flac", "audio-x-generic"},
        {".ogg", "audio-x-generic"},        {".txt", "text-x-generic"},
        {".md", "text-x-generic"},          {".doc", "x-office-document"},
        {".docx", "x-office-document"},     {".xlsx", "x-office-spreadsheet"},
        {".pptx", "x-office-presentation"}, {".appimage", "application-x-executable"},
    }};

    bool isPartialDownload(std::string_view name) {
      return name.ends_with(".part") || name.ends_with(".crdownload") || name.ends_with(".download");
    }

    bool isImageFile(const std::filesystem::path& path) {
      const std::string ext = StringUtils::toLower(path.extension().string());
      return std::ranges::contains(kImageExtensions, std::string_view(ext));
    }

    std::string fileIconName(const DockStackFile& file) {
      if (file.directory) {
        return "folder";
      }
      const std::string ext = StringUtils::toLower(file.path.extension().string());
      const auto it = std::ranges::find(kExtensionIcons, std::string_view(ext), &ExtensionIcon::extension);
      return it != kExtensionIcons.end() ? std::string(it->icon) : std::string("text-x-generic");
    }

    // Press-and-drag state for one file in the popup.
    struct FileDragGesture {
      bool pressed = false;
      bool dragged = false;
      float startX = 0.0F;
      float startY = 0.0F;
      std::uint32_t serial = 0;
    };

  } // namespace

  std::vector<DockStackFile> listStackFiles(const std::filesystem::path& folder, std::size_t limit) {
    std::vector<std::pair<std::filesystem::file_time_type, DockStackFile>> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = it->path().filename().string();
      if (name.empty() || name.front() == '.' || isPartialDownload(name)) {
        continue;
      }
      std::error_code entryEc;
      const auto mtime = it->last_write_time(entryEc);
      if (entryEc) {
        continue;
      }
      found.emplace_back(mtime, DockStackFile{.path = it->path(), .directory = it->is_directory(entryEc)});
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<DockStackFile> files;
    files.reserve(std::min(limit, found.size()));
    for (auto& [mtime, file] : found) {
      if (files.size() >= limit) {
        break;
      }
      files.push_back(std::move(file));
    }
    return files;
  }

  std::string
  stackFileImagePath(IconResolver& iconResolver, const DockStackFile& file, int targetSize, bool& thumbnail) {
    thumbnail = !file.directory && isImageFile(file.path);
    if (thumbnail) {
      return file.path.string();
    }
    std::string path = iconResolver.resolve(fileIconName(file), targetSize);
    if (path.empty()) {
      path = iconResolver.resolve("text-x-generic", targetSize);
    }
    return path;
  }

  std::unique_ptr<DockPopup> createStackPopup(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext, IconResolver& iconResolver,
      zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, const DockConfig& dockConfig,
      const std::string& folder, const std::optional<DockPopupAnchor>& anchor, const DockStackCallbacks& callbacks
  ) {
    auto popup = std::make_unique<DockPopup>();
    std::vector<DockStackFile> files = listStackFiles(folder, kStackMaxFiles);
    const bool list = dockConfig.stackView == DockStackView::List;

    const std::size_t columns = list ? 1 : std::clamp<std::size_t>(files.size(), 1, kStackMaxColumns);
    const std::size_t rows = files.empty() ? 1 : (files.size() + columns - 1) / columns;
    const float cellWidth = list ? kListWidth : kTileWidth;
    const float cellHeight = list ? kRowHeight : kTileHeight;
    const float gridWidth = static_cast<float>(columns) * cellWidth;
    const float gridHeight = files.empty() ? kRowHeight : static_cast<float>(rows) * cellHeight;
    const float contentWidth = std::max(gridWidth, 2.0F * kTileWidth) + 2.0F * Style::spaceSm;
    const float contentHeight = gridHeight + kFooterHeight + 3.0F * Style::spaceSm;

    const auto popupChrome = popup_chrome::computeGeometry(
        contentWidth, contentHeight, config.config().shell.shadow, Style::popupShadowsEnabled()
    );
    PopupSurfaceConfig popupCfg = dockPopupSurfaceConfig(platform, dockConfig, popupChrome, anchor);
    // niri only accepts a drag started under the popup's own grab.
    const std::uint32_t grabSerial = compositors::isNiri() ? popupCfg.serial : 0;

    popup->surface = std::make_unique<PopupSurface>(platform.wayland());
    popup->surface->setRenderContext(&renderContext);
    popup->chrome = popupChrome;

    auto* popupPtr = popup.get();
    const auto activate = [callbacks](std::string path) {
      DeferredCall::callLater([callbacks, path = std::move(path)] {
        if (callbacks.openPath) {
          callbacks.openPath(path);
        }
        if (callbacks.closeMenu) {
          callbacks.closeMenu();
        }
      });
    };

    popup->surface->setConfigureCallback([popupPtr](std::uint32_t /*w*/, std::uint32_t /*h*/) {
      popupPtr->surface->requestLayout();
    });
    popup->surface->setPrepareFrameCallback([&platform, &config, &renderContext, &iconResolver, popupPtr, files, folder,
                                             list, columns, cellWidth, cellHeight, gridWidth, gridHeight, grabSerial,
                                             activate, callbacks](bool /*needsUpdate*/, bool needsLayout) {
      if (popupPtr->surface == nullptr) {
        return;
      }
      const auto width = popupPtr->surface->width();
      const auto height = popupPtr->surface->height();
      if (width == 0 || height == 0) {
        return;
      }

      renderContext.makeCurrent(popupPtr->surface->renderTarget());
      const bool needsSceneBuild = popupPtr->sceneRoot == nullptr
          || static_cast<std::uint32_t>(std::round(popupPtr->sceneRoot->width())) != width
          || static_cast<std::uint32_t>(std::round(popupPtr->sceneRoot->height())) != height;
      if (!needsSceneBuild && !needsLayout) {
        return;
      }

      UiPhaseScope layoutPhase(UiPhase::Layout);
      Renderer& renderer = popupPtr->surface->renderTarget().renderer();
      const auto& chrome = popupPtr->chrome;

      popupPtr->sceneRoot = ui::node({});
      popupPtr->sceneRoot->setSize(static_cast<float>(width), static_cast<float>(height));
      if (Style::popupShadowsEnabled()) {
        popup_chrome::addShadow(*popupPtr->sceneRoot, chrome, config.config().shell.shadow, Style::scaledRadiusLg());
      }
      popup_chrome::addCardBackground(*popupPtr->sceneRoot, chrome, 1.0F);

      const float gridX = chrome.contentX() + (chrome.contentWidth - gridWidth) * 0.5F;
      const float gridY = chrome.contentY() + Style::spaceSm;
      const float iconSize = list ? kRowIconSize : kTileIconSize;
      const int iconTarget = static_cast<int>(std::ceil(iconSize * 2.0F));

      if (files.empty()) {
        popupPtr->sceneRoot->addChild(
            ui::label({
                .text = i18n::tr("dock.stack.empty"),
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .width = chrome.contentWidth,
                .textAlign = TextAlign::Center,
                .configure = [&chrome, gridY](Label& label) {
                  label.setPosition(chrome.contentX(), gridY + Style::spaceSm);
                },
            })
        );
      }

      for (std::size_t index = 0; index < files.size(); ++index) {
        const DockStackFile& file = files[index];
        const std::string path = file.path.string();
        const std::string name = file.path.filename().string();
        const float cellX = gridX + static_cast<float>(index % columns) * cellWidth;
        const float cellY = gridY + static_cast<float>(index / columns) * cellHeight;

        auto gesture = std::make_shared<FileDragGesture>();
        auto cell = ui::inputArea({
            .acceptedButtons = InputArea::buttonMask({BTN_LEFT}),
            .x = cellX,
            .y = cellY,
            .width = cellWidth,
            .height = cellHeight,
            .onPress =
                [gesture](const InputArea::PointerData& d) {
                  gesture->pressed = d.pressed;
                  if (d.pressed) {
                    gesture->dragged = false;
                    gesture->startX = d.sceneX;
                    gesture->startY = d.sceneY;
                    gesture->serial = d.serial;
                  }
                },
            .onClick =
                [activate, gesture, path](const InputArea::PointerData& /*d*/) {
                  if (!gesture->dragged) {
                    activate(path);
                  }
                },
        });
        bool dragThumbnail = false;
        std::string dragIcon = stackFileImagePath(iconResolver, file, kDragIconTarget, dragThumbnail);
        cell->setOnMotion([&platform, popupPtr, gesture, path, dragIcon = std::move(dragIcon), grabSerial,
                           callbacks](const InputArea::PointerData& d) {
          if (!gesture->pressed || gesture->dragged) {
            return;
          }
          if (std::hypot(d.sceneX - gesture->startX, d.sceneY - gesture->startY) < kDragThreshold) {
            return;
          }
          gesture->dragged = true;
          const std::uint32_t serial = grabSerial != 0 ? grabSerial : gesture->serial;
          const auto closeMenu = [callbacks] {
            DeferredCall::callLater([callbacks] {
              if (callbacks.closeMenu) {
                callbacks.closeMenu();
              }
            });
          };
          if (!platform.wayland().extension<FileDrag>().start(
                  popupPtr->wlSurface, serial, {path}, dragIcon, closeMenu
              )) {
            kLog.warn("dock: could not start dragging {}", path);
            return;
          }
          // The drag takes over the popup's grab, so hide it now and close it when the drag ends.
          if (popupPtr->sceneRoot != nullptr) {
            popupPtr->sceneRoot->setOpacity(0.0F);
            popupPtr->surface->requestRedraw();
          }
        });

        Box* hover = nullptr;
        cell->addChild(
            ui::box({
                .out = &hover,
                .fill = colorSpecFromRole(ColorRole::SurfaceContainerHighest),
                .radius = Style::scaledRadiusLg(),
                .width = cellWidth,
                .height = cellHeight,
                .visible = false,
            })
        );
        cell->setOnEnter([hover](const InputArea::PointerData& /*d*/) { hover->setVisible(true); });
        cell->setOnLeave([hover, gesture] {
          hover->setVisible(false);
          gesture->pressed = false;
        });

        bool thumbnail = false;
        const std::string imagePath = stackFileImagePath(iconResolver, file, iconTarget, thumbnail);
        const float iconX = list ? Style::spaceSm : (cellWidth - iconSize) * 0.5F;
        const float iconY = list ? (cellHeight - iconSize) * 0.5F : Style::spaceSm;
        cell->addChild(
            ui::image({
                .fit = ImageFit::Contain,
                .radius = thumbnail ? Style::radiusSm : 0.0F,
                .width = iconSize,
                .height = iconSize,
                .configure = [&renderer, imagePath, iconTarget, iconX, iconY](Image& image) {
                  image.setPosition(iconX, iconY);
                  if (!imagePath.empty()) {
                    image.setSourceFile(renderer, imagePath, iconTarget, true);
                  }
                },
            })
        );

        const float textX = list ? iconX + iconSize + Style::spaceSm : Style::spaceXs;
        const float textWidth = list ? cellWidth - textX - Style::spaceSm : cellWidth - 2.0F * Style::spaceXs;
        cell->addChild(
            ui::label({
                .text = name,
                .fontSize = list ? Style::fontSizeCaption : Style::fontSizeMini,
                .width = textWidth,
                .maxWidth = textWidth,
                .maxLines = list ? 1 : 2,
                .textAlign = list ? TextAlign::Start : TextAlign::Center,
                .ellipsize = TextEllipsize::Middle,
                .configure = [list, textX, iconY, iconSize, cellHeight](Label& label) {
                  const float lineHeight = Style::fontSizeCaption * 1.4F;
                  label.setPosition(textX, list ? (cellHeight - lineHeight) * 0.5F : iconY + iconSize + Style::spaceXs);
                },
            })
        );
        popupPtr->sceneRoot->addChild(std::move(cell));
      }

      popupPtr->sceneRoot->addChild(
          ui::button({
              .text = i18n::tr("dock.stack.open-folder"),
              .fontSize = Style::fontSizeCaption,
              .variant = ButtonVariant::Ghost,
              .width = chrome.contentWidth - 2.0F * Style::spaceSm,
              .height = kFooterHeight,
              .onClick = [activate, folder] { activate(folder); },
              .configure =
                  [&chrome, gridY, gridHeight](Button& button) {
                    button.setPosition(chrome.contentX() + Style::spaceSm, gridY + gridHeight + Style::spaceSm);
                  },
          })
      );

      popupPtr->sceneRoot->layout(renderer);
      popupPtr->inputDispatcher.setSceneRoot(popupPtr->sceneRoot.get());
      popupPtr->inputDispatcher.setCursorShapeCallback([&platform](std::uint32_t serial, std::uint32_t shape) {
        platform.setCursorShape(serial, shape);
      });
      popupPtr->surface->setSceneRoot(popupPtr->sceneRoot.get());
    });

    popup->surface->setDismissedCallback([callbacks] {
      DeferredCall::callLater([callbacks] {
        if (callbacks.closeMenu) {
          callbacks.closeMenu();
        }
      });
    });

    if (parentLayerSurface == nullptr
        || !popup->surface->initialize(LayerShell::popupAttach(parentLayerSurface), output, popupCfg)) {
      kLog.warn("dock: failed to create stack popup");
      return nullptr;
    }

    popup_chrome::setContentInputRegion(*popup->surface, popup->chrome);
    popup->wlSurface = popup->surface->wlSurface();
    return popup;
  }

} // namespace shell::dock
