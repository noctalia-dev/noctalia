#include "shell/desktop/editor/desktop_widgets_editor.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/backend/render_backend.h"
#include "render/core/color.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/texture_manager.h"
#include "render/core/wallpaper_types.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/wallpaper_node.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/widgets/desktop_login_box_widget.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/tooltip/tooltip_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>
#include <ranges>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kToolbarY = 68.0f;
  constexpr float kSelectionStroke = 2.0f;
  constexpr float kShadowExpand = 1.0f;
  const Color kShadowColor = rgba(0.0f, 0.0f, 0.0f, 0.45f);
  constexpr float kRotatePadding = 14.0f;
  constexpr float kHandleSize = 14.0f;
  constexpr float kDisabledWidgetOpacity = 0.25f;
  constexpr float kRotationSnap = std::numbers::pi_v<float> / 12.0f;
  constexpr float kSnapGuideThresholdMin = 6.0f;
  constexpr float kSnapGuideThresholdMax = 18.0f;
  constexpr float kCenterGuideThickness = 3.0f;
  constexpr std::size_t kScaleCornerCount = 4;

  struct CornerSigns {
    float x = 1.0f;
    float y = 1.0f;
  };

  float snapToGrid(float value, std::int32_t cellSize, float origin) {
    if (cellSize <= 0) {
      return value;
    }
    const auto cell = static_cast<float>(cellSize);
    return origin + std::round((value - origin) / cell) * cell;
  }

  float snapGuideThreshold(std::int32_t cellSize) {
    return std::clamp(static_cast<float>(cellSize) * 0.75f, kSnapGuideThresholdMin, kSnapGuideThresholdMax);
  }

  float snapLineToTargets(
      float value, std::int32_t cellSize, float origin, const std::vector<float>& guideLines, float guideThreshold
  ) {
    float bestGuide = value;
    float bestGuideDistance = std::numeric_limits<float>::max();
    for (const float line : guideLines) {
      const float distance = std::abs(line - value);
      if (distance <= guideThreshold && distance < bestGuideDistance) {
        bestGuide = line;
        bestGuideDistance = distance;
      }
    }
    if (bestGuideDistance < std::numeric_limits<float>::max()) {
      return bestGuide;
    }
    return snapToGrid(value, cellSize, origin);
  }

  float snapBoundsAxisToTargets(
      float center, float extent, std::int32_t cellSize, float origin, const std::vector<float>& guideLines,
      float guideThreshold
  ) {
    if (cellSize <= 0) {
      return center;
    }

    const float halfExtent = extent * 0.5f;
    const float left = center - halfExtent;
    const float right = center + halfExtent;
    const float leftOffset = snapLineToTargets(left, cellSize, origin, guideLines, guideThreshold) - left;
    const float centerOffset = snapLineToTargets(center, cellSize, origin, guideLines, guideThreshold) - center;
    const float rightOffset = snapLineToTargets(right, cellSize, origin, guideLines, guideThreshold) - right;

    float bestOffset = leftOffset;
    if (std::abs(centerOffset) < std::abs(bestOffset)) {
      bestOffset = centerOffset;
    }
    if (std::abs(rightOffset) < std::abs(bestOffset)) {
      bestOffset = rightOffset;
    }
    return center + bestOffset;
  }

  float normalizeAngle(float radians) {
    while (radians > std::numbers::pi_v<float>) {
      radians -= 2.0f * std::numbers::pi_v<float>;
    }
    while (radians < -std::numbers::pi_v<float>) {
      radians += 2.0f * std::numbers::pi_v<float>;
    }
    return radians;
  }

  bool parseWidgetCounter(std::string_view prefix, std::string_view id, std::uint64_t& value) {
    if (!id.starts_with(prefix)) {
      return false;
    }

    const std::string_view suffix = id.substr(prefix.size());
    if (suffix.empty()) {
      return false;
    }

    value = 0;
    const auto* begin = suffix.data();
    const auto* end = suffix.data() + suffix.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    return ec == std::errc{} && ptr == end;
  }

  CornerSigns cornerSigns(std::size_t cornerIndex) {
    switch (cornerIndex) {
    case 0:
      return {-1.0f, -1.0f};
    case 1:
      return {1.0f, -1.0f};
    case 2:
      return {-1.0f, 1.0f};
    case 3:
    default:
      return {1.0f, 1.0f};
    }
  }

  std::pair<float, float> rotatedCorner(float cx, float cy, float halfWidth, float halfHeight, float rotationRad) {
    const float cosTheta = std::cos(rotationRad);
    const float sinTheta = std::sin(rotationRad);
    return {cx + halfWidth * cosTheta - halfHeight * sinTheta, cy + halfWidth * sinTheta + halfHeight * cosTheta};
  }

  Color lockscreenWallpaperFillColor(const WallpaperConfig& config) {
    if (!config.fillColor) {
      return rgba(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return resolveColorSpec(*config.fillColor);
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  bool lockscreenWallpaperDiffersFromDesktop(
      const DesktopWidgetsEditorProfile& profile, ConfigService* config, std::string_view connectorName
  ) {
    if (!profile.showLockscreenLoginPreview || config == nullptr) {
      return false;
    }
    const std::string& custom = config->config().lockscreen.wallpaper;
    if (custom.empty()) {
      return false;
    }
    return custom != config->getWallpaperPath(std::string(connectorName));
  }

  const WaylandOutput* outputAtGlobalPoint(const WaylandConnection& wayland, double globalX, double globalY) {
    for (const auto& output : wayland.outputs()) {
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      const auto left = static_cast<double>(output.logicalX);
      const auto top = static_cast<double>(output.logicalY);
      const auto right = left + static_cast<double>(output.effectiveLogicalWidth());
      const auto bottom = top + static_cast<double>(output.effectiveLogicalHeight());
      if (globalX >= left && globalX < right && globalY >= top && globalY < bottom) {
        return &output;
      }
    }
    return nullptr;
  }

} // namespace

DesktopWidgetsEditor::DesktopWidgetsEditor(DesktopWidgetsEditorProfile profile) : m_profile(profile) {}

std::string DesktopWidgetsEditor::nextWidgetId() const {
  std::uint64_t maxCounter = 0;
  for (const auto& widget : m_snapshot.widgets) {
    std::uint64_t counter = 0;
    if (parseWidgetCounter(m_profile.widgetIdPrefix, widget.id, counter)) {
      maxCounter = std::max(maxCounter, counter);
    }
  }

  const std::uint64_t nextCounter =
      maxCounter == std::numeric_limits<std::uint64_t>::max() ? maxCounter : (maxCounter + 1);
  return std::format("{}{:016x}", m_profile.widgetIdPrefix, nextCounter);
}

void DesktopWidgetsEditor::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_config = services.config;
  m_renderContext = services.renderContext;
  m_textureCache = services.textureCache;
  m_factory = std::make_unique<DesktopWidgetFactory>(services.runtime);
}

void DesktopWidgetsEditor::setExitRequestedCallback(std::function<void()> callback) {
  m_exitRequestedCallback = std::move(callback);
}

void DesktopWidgetsEditor::open(const DesktopWidgetsEditorSnapshot& snapshot) {
  m_snapshot = snapshot;
  if (m_profile.showLockscreenLoginPreview && m_wayland != nullptr) {
    lockscreen_login_box::ensureWidgets(m_snapshot.widgets, *m_wayland);
  }
  m_open = true;
  clearSelection();
  m_widgetClipboard.clear();
  m_pasteCount = 0;
  m_drag = {};
  m_shiftHeld = false;
  m_leftShiftHeld = false;
  m_rightShiftHeld = false;
  m_ctrlHeld = false;
  m_leftCtrlHeld = false;
  m_rightCtrlHeld = false;
  m_altHeld = false;
  m_leftAltHeld = false;
  m_rightAltHeld = false;
  m_inspectorOpen = false;
  syncSurfaces();
  requestLayout();
}

DesktopWidgetsEditorSnapshot DesktopWidgetsEditor::close() {
  if (m_drag.mode != DragMode::None) {
    finishDrag();
  }
  for (auto& surface : m_surfaces) {
    releaseWallpaperPreview(*surface);
  }
  m_surfaces.clear();
  m_drag = {};
  clearSelection();
  m_widgetClipboard.clear();
  m_pasteCount = 0;
  m_shiftHeld = false;
  m_leftShiftHeld = false;
  m_rightShiftHeld = false;
  m_ctrlHeld = false;
  m_leftCtrlHeld = false;
  m_rightCtrlHeld = false;
  m_altHeld = false;
  m_leftAltHeld = false;
  m_rightAltHeld = false;
  m_open = false;
  return m_snapshot;
}

void DesktopWidgetsEditor::clearSelection() {
  m_selectedWidgetIds.clear();
  m_selectedWidgetId.clear();
}

void DesktopWidgetsEditor::setSingleSelection(const std::string& id) {
  m_selectedWidgetIds.clear();
  if (!id.empty()) {
    m_selectedWidgetIds.insert(id);
  }
  m_selectedWidgetId = id;
}

bool DesktopWidgetsEditor::isWidgetSelected(const std::string& id) const { return m_selectedWidgetIds.contains(id); }

void DesktopWidgetsEditor::handleWidgetPress(const std::string& id) {
  const bool ctrlHeld = m_ctrlHeld;
  const DesktopWidgetState* state = findWidgetState(id);
  const bool isLoginBox = state != nullptr && lockscreen_login_box::isLoginBoxWidget(*state);

  bool selectionChanged = false;

  if (isLoginBox) {
    if (!isWidgetSelected(id) || m_selectedWidgetIds.size() > 1) {
      setSingleSelection(id);
      selectionChanged = true;
    } else {
      m_selectedWidgetId = id;
    }
  } else if (ctrlHeld) {
    if (isWidgetSelected(id)) {
      m_selectedWidgetIds.erase(id);
      if (m_selectedWidgetId == id) {
        m_selectedWidgetId = m_selectedWidgetIds.empty() ? "" : *m_selectedWidgetIds.begin();
      }
    } else {
      m_selectedWidgetIds.insert(id);
      m_selectedWidgetId = id;
    }
    selectionChanged = true;
  } else if (!isWidgetSelected(id)) {
    setSingleSelection(id);
    selectionChanged = true;
  } else {
    m_selectedWidgetId = id;
  }

  if (selectionChanged) {
    DeferredCall::callLater([this]() { requestLayout(); });
  }
  startDrag(DragMode::Move, id, false);
}

bool DesktopWidgetsEditor::isOpen() const noexcept { return m_open; }

float DesktopWidgetsEditor::widgetContentScale() const {
  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0f;
  return desktop_widgets::widgetContentScale(baseUiScale);
}

void DesktopWidgetsEditor::syncSurfaces() {
  if (!m_open || m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }

  const auto& outputs = m_wayland->outputs();
  std::erase_if(m_surfaces, [&outputs](const auto& surface) {
    return std::none_of(outputs.begin(), outputs.end(), [&surface](const auto& output) {
      return output.done
          && output.output != nullptr
          && output.hasUsableGeometry()
          && desktop_widgets::outputKey(output) == surface->outputName;
    });
  });

  for (const auto& output : outputs) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    const std::string key = desktop_widgets::outputKey(output);
    const bool exists =
        std::ranges::any_of(m_surfaces, [&](const auto& surface) { return surface->outputName == key; });
    if (!exists) {
      createSurface(output);
    }
  }
}

void DesktopWidgetsEditor::createSurface(const WaylandOutput& output) {
  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = std::string(m_profile.layerNamespace),
      .layer = LayerShellLayer::Bottom,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = 0,
      .exclusiveZone = -1,
      .keyboard = LayerShellKeyboard::OnDemand,
      .defaultWidth = static_cast<std::uint32_t>(output.effectiveLogicalWidth()),
      .defaultHeight = static_cast<std::uint32_t>(output.effectiveLogicalHeight()),
  };

  auto overlay = std::make_unique<OverlaySurface>();
  overlay->outputName = desktop_widgets::outputKey(output);
  overlay->output = output.output;
  overlay->sceneRebuildRequested = true;
  overlay->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
  overlay->surface->setRenderContext(m_renderContext);
  overlay->surface->setAnimationManager(&overlay->animations);
  overlay->inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });

  auto* rawOverlay = overlay.get();
  overlay->surface->setConfigureCallback([rawOverlay](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    rawOverlay->surface->requestLayout();
  });
  overlay->surface->setPrepareFrameCallback([this, rawOverlay](bool needsUpdate, bool needsLayout) {
    prepareFrame(*rawOverlay, needsUpdate, needsLayout);
  });
  overlay->inputDispatcher.setHoverChangeCallback([rawOverlay](InputArea* /*old*/, InputArea* next) {
    TooltipManager::instance().onHoverChange(next, rawOverlay->surface->layerSurface(), rawOverlay->output);
  });
  overlay->surface->setFrameTickCallback([this, rawOverlay](float deltaMs) {
    if (m_renderContext == nullptr || rawOverlay->surface == nullptr) {
      return;
    }
    m_renderContext->makeCurrent(rawOverlay->surface->renderTarget());
    for (auto& [id, view] : rawOverlay->views) {
      (void)id;
      if (view.widget != nullptr && view.widget->needsFrameTick()) {
        view.widget->onFrameTick(deltaMs, *m_renderContext);
      }
    }
  });

  if (!overlay->surface->initialize(output.output)) {
    Logger(m_profile.logSection.data())
        .warn("{} widgets editor: failed to initialize overlay on {}", m_profile.logSection, overlay->outputName);
    return;
  }

  m_surfaces.push_back(std::move(overlay));
}

std::optional<LayerPopupParentContext>
DesktopWidgetsEditor::overlayPopupParentContext(const OverlaySurface& surface) const {
  if (!m_open || surface.surface == nullptr) {
    return std::nullopt;
  }

  zwlr_layer_surface_v1* layerSurface = surface.surface->layerSurface();
  const std::uint32_t width = surface.surface->width();
  const std::uint32_t height = surface.surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  return LayerPopupParentContext{
      .surface = surface.surface->wlSurface(),
      .layerSurface = layerSurface,
      .output = surface.output,
      .width = width,
      .height = height,
  };
}

std::optional<LayerPopupParentContext> DesktopWidgetsEditor::popupParentContextForSurface(wl_surface* surface) const {
  if (surface == nullptr) {
    return std::nullopt;
  }
  const OverlaySurface* overlay = findSurface(surface);
  return overlay != nullptr ? overlayPopupParentContext(*overlay) : std::nullopt;
}

std::optional<LayerPopupParentContext> DesktopWidgetsEditor::fallbackPopupParentContext() const {
  if (!m_open || m_surfaces.empty()) {
    return std::nullopt;
  }

  if (m_wayland != nullptr) {
    if (const OverlaySurface* overlay = findSurface(m_wayland->lastPointerSurface()); overlay != nullptr) {
      if (auto context = overlayPopupParentContext(*overlay); context.has_value()) {
        return context;
      }
    }
    if (const OverlaySurface* overlay = findSurface(m_wayland->lastKeyboardSurface()); overlay != nullptr) {
      if (auto context = overlayPopupParentContext(*overlay); context.has_value()) {
        return context;
      }
    }
  }

  for (const auto& overlay : m_surfaces) {
    if (auto context = overlayPopupParentContext(*overlay); context.has_value()) {
      return context;
    }
  }
  return std::nullopt;
}

DesktopWidgetsEditor::OverlaySurface* DesktopWidgetsEditor::findSurface(wl_surface* surface) {
  return const_cast<OverlaySurface*>(std::as_const(*this).findSurface(surface));
}

const DesktopWidgetsEditor::OverlaySurface* DesktopWidgetsEditor::findSurface(wl_surface* surface) const {
  for (const auto& overlay : m_surfaces) {
    if (overlay->surface != nullptr && overlay->surface->wlSurface() == surface) {
      return overlay.get();
    }
  }
  return nullptr;
}

DesktopWidgetsEditor::OverlaySurface* DesktopWidgetsEditor::findSurface(const std::string& outputName) {
  for (auto& overlay : m_surfaces) {
    if (overlay->outputName == outputName) {
      return overlay.get();
    }
  }
  return nullptr;
}

DesktopWidgetsEditor::OverlaySurface* DesktopWidgetsEditor::findSurfaceForWidget(const std::string& widgetId) {
  for (auto& overlay : m_surfaces) {
    if (overlay->views.contains(widgetId)) {
      return overlay.get();
    }
  }
  return nullptr;
}

DesktopWidgetsEditor::EditorWidgetView* DesktopWidgetsEditor::findView(const std::string& id) {
  for (auto& overlay : m_surfaces) {
    const auto it = overlay->views.find(id);
    if (it != overlay->views.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

DesktopWidgetState* DesktopWidgetsEditor::findWidgetState(const std::string& id) {
  for (auto& widget : m_snapshot.widgets) {
    if (widget.id == id) {
      return &widget;
    }
  }
  return nullptr;
}

const DesktopWidgetState* DesktopWidgetsEditor::findWidgetState(const std::string& id) const {
  for (const auto& widget : m_snapshot.widgets) {
    if (widget.id == id) {
      return &widget;
    }
  }
  return nullptr;
}

std::string DesktopWidgetsEditor::effectiveOutputName(const DesktopWidgetState& state) const {
  if (m_wayland == nullptr) {
    return state.outputName;
  }
  if (const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, state); output != nullptr) {
    return desktop_widgets::outputKey(*output);
  }
  return {};
}

bool DesktopWidgetsEditor::shouldSnap() const {
  return (m_snapshot.grid.visible != m_shiftHeld) && m_snapshot.grid.cellSize > 0;
}

void DesktopWidgetsEditor::prepareFrame(OverlaySurface& surface, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || surface.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(surface.surface->renderTarget());

  if (surface.sceneRoot == nullptr || surface.sceneRebuildRequested) {
    rebuildScene(surface);
    surface.sceneRebuildRequested = false;
  }

  if (needsUpdate) {
    for (auto& [id, view] : surface.views) {
      (void)id;
      if (view.widget != nullptr) {
        view.widget->update(*m_renderContext);
      }
    }
  }

  if (needsLayout && surface.sceneRoot != nullptr) {
    // Re-run each widget's own layout so async content (e.g. a plugin widget's
    // first render() arriving after view creation) reconciles and the view
    // tracks the widget's natural size.
    bool intrinsicsChanged = false;
    for (auto& [id, view] : surface.views) {
      if (view.widget == nullptr || view.transformNode == nullptr) {
        continue;
      }
      // During a scale drag, re-fit only the dragged widget; the others don't change, and skipping
      // them keeps the per-frame relayout cheap.
      if (m_drag.mode == DragMode::Scale && id != m_drag.widgetId) {
        continue;
      }
      view.widget->layout(*m_renderContext);
      const float w = std::max(1.0f, view.widget->intrinsicWidth());
      const float h = std::max(1.0f, view.widget->intrinsicHeight());
      if (w == view.intrinsicWidth && h == view.intrinsicHeight) {
        continue;
      }
      view.intrinsicWidth = w;
      view.intrinsicHeight = h;
      view.transformNode->setFrameSize(w, h);
      if (const DesktopWidgetState* state = findWidgetState(id); state != nullptr) {
        view.transformNode->setPosition(state->cx - w * 0.5f, state->cy - h * 0.5f);
      }
      intrinsicsChanged = true;
    }
    if (intrinsicsChanged) {
      updateSelectionVisuals(surface);
    }
    surface.sceneRoot->layout(*m_renderContext);
  }

  if (surface.wallpaperPreviewActive) {
    updateWallpaperPreview(surface);
  }

  const bool needsFrameTick = std::ranges::any_of(surface.views, [](const auto& entry) {
    return entry.second.widget != nullptr && entry.second.widget->needsFrameTick();
  });
  if (needsFrameTick) {
    surface.surface->requestFrameTick();
  }
}

void DesktopWidgetsEditor::rebuildScene(OverlaySurface& surface) {
  surface.views.clear();
  surface.secondarySelections.clear();
  surface.selectionFrameTransform = nullptr;
  surface.selectionBorder = nullptr;
  surface.rotationRing = nullptr;
  surface.rotateArea = nullptr;
  surface.scaleHandles.fill(nullptr);
  surface.scaleAreas.fill(nullptr);
  surface.toolbar = nullptr;

  auto root = std::make_unique<InputArea>();
  root->setEnabled(false);
  root->setAnimationManager(&surface.animations);
  if (m_renderContext != nullptr && m_wayland != nullptr) {
    surface.selectPopup = std::make_unique<SelectDropdownPopup>(*m_wayland, *m_renderContext);
    if (m_config != nullptr) {
      surface.selectPopup->setShadowConfig(m_config->config().shell.shadow);
    }
    surface.selectPopup->setParent(surface.surface->layerSurface(), surface.surface->wlSurface(), surface.output);
    root->setPopupContext(surface.selectPopup.get());
  }
  root->setFrameSize(static_cast<float>(surface.surface->width()), static_cast<float>(surface.surface->height()));

  releaseWallpaperPreview(surface);
  surface.wallpaperPreview = nullptr;
  surface.wallpaperPreviewActive = false;
  surface.wallpaperPreviewPath.clear();

  if (lockscreenWallpaperDiffersFromDesktop(m_profile, m_config, surface.outputName)) {
    surface.wallpaperPreviewActive = true;
    surface.wallpaperPreviewPath = m_config->config().lockscreen.wallpaper;

    auto wallpaper = std::make_unique<WallpaperNode>();
    surface.wallpaperPreview = wallpaper.get();
    wallpaper->setZIndex(-1);
    const auto& wpConfig = m_config->config().wallpaper;
    wallpaper->setFillMode(wpConfig.fillMode);
    wallpaper->setFillColor(lockscreenWallpaperFillColor(wpConfig));
    wallpaper->setPosition(0.0f, 0.0f);
    wallpaper->setFrameSize(root->width(), root->height());
    root->addChild(std::move(wallpaper));
  }

  auto dim = ui::box({
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.14f),
  });
  dim->setPosition(0.0f, 0.0f);
  dim->setFrameSize(root->width(), root->height());
  dim->setZIndex(0);
  root->addChild(std::move(dim));

  auto backgroundArea = std::make_unique<InputArea>();
  backgroundArea->setPosition(0.0f, 0.0f);
  backgroundArea->setFrameSize(root->width(), root->height());
  backgroundArea->setZIndex(1);
  backgroundArea->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT || m_drag.mode != DragMode::None) {
      return;
    }
    if (!m_selectedWidgetIds.empty()) {
      clearSelection();
      requestLayout();
    }
  });
  root->addChild(std::move(backgroundArea));

  if (m_snapshot.grid.visible && m_snapshot.grid.cellSize > 0) {
    const float width = root->width();
    const float height = root->height();
    const auto cell = static_cast<float>(m_snapshot.grid.cellSize);
    const std::int32_t majorInterval = std::max(1, m_snapshot.grid.majorInterval);
    const float centerX = width * 0.5f;
    const float centerY = height * 0.5f;
    const float firstX = centerX - std::floor(centerX / cell) * cell;
    const float firstY = centerY - std::floor(centerY / cell) * cell;

    for (float x = firstX; x <= width; x += cell) {
      const int idx = std::abs(static_cast<int>(std::lround((x - centerX) / cell)));
      const bool major = (idx % majorInterval) == 0;
      auto line = ui::box({
          .fill = colorSpecFromRole(major ? ColorRole::Primary : ColorRole::Outline, major ? 0.18f : 0.08f),
      });
      line->setPosition(x, 0.0f);
      line->setFrameSize(1.0f, height);
      line->setZIndex(2);
      root->addChild(std::move(line));
    }

    for (float y = firstY; y <= height; y += cell) {
      const int idx = std::abs(static_cast<int>(std::lround((y - centerY) / cell)));
      const bool major = (idx % majorInterval) == 0;
      auto line = ui::box({
          .fill = colorSpecFromRole(major ? ColorRole::Primary : ColorRole::Outline, major ? 0.18f : 0.08f),
      });
      line->setPosition(0.0f, y);
      line->setFrameSize(width, 1.0f);
      line->setZIndex(2);
      root->addChild(std::move(line));
    }
  }

  const float width = root->width();
  const float height = root->height();
  const float centerX = width * 0.5f;
  const float centerY = height * 0.5f;
  const float centerGuideOffset = kCenterGuideThickness * 0.5f;

  auto verticalGuide = ui::box({
      .fill = colorSpecFromRole(ColorRole::Primary, 0.35f),
  });
  verticalGuide->setPosition(centerX - centerGuideOffset, 0.0f);
  verticalGuide->setFrameSize(kCenterGuideThickness, height);
  verticalGuide->setZIndex(3);
  root->addChild(std::move(verticalGuide));

  auto horizontalGuide = ui::box({
      .fill = colorSpecFromRole(ColorRole::Primary, 0.35f),
  });
  horizontalGuide->setPosition(0.0f, centerY - centerGuideOffset);
  horizontalGuide->setFrameSize(width, kCenterGuideThickness);
  horizontalGuide->setZIndex(3);
  root->addChild(std::move(horizontalGuide));

  for (const auto& widgetState : m_snapshot.widgets) {
    if (effectiveOutputName(widgetState) != surface.outputName || m_factory == nullptr) {
      continue;
    }

    auto widget = m_factory->create(widgetState.type, widgetState.settings, widgetContentScale());
    if (widget == nullptr) {
      continue;
    }

    if (lockscreen_login_box::isLoginBoxWidget(widgetState)) {
      if (auto* loginWidget = dynamic_cast<DesktopLoginBoxWidget*>(widget.get())) {
        loginWidget->setScreenWidth(root->width());
      }
    }

    widget->create();
    if (widgetState.type == "audio_visualizer"
        || widgetState.type == "fancy_audio_visualizer"
        || widgetState.type == "button") {
      widget->setEditorPreview(true);
    }
    widget->setAnimationManager(&surface.animations);
    auto* surfacePtr = &surface;
    widget->setUpdateCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestUpdateOnly();
      }
    });
    widget->setLayoutCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestUpdate();
      }
    });
    widget->setRedrawCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestRedraw();
      }
    });
    widget->setFrameTickRequestCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestFrameTick();
      }
    });
    widget->setBox(widgetState.boxWidth, widgetState.boxHeight);
    widget->update(*m_renderContext);
    widget->layout(*m_renderContext);
    if ((widgetState.type == "audio_visualizer" || widgetState.type == "fancy_audio_visualizer")
        && surface.surface != nullptr) {
      surface.surface->requestFrameTick();
    }

    EditorWidgetView view;
    view.intrinsicWidth = std::max(1.0f, widget->intrinsicWidth());
    view.intrinsicHeight = std::max(1.0f, widget->intrinsicHeight());

    auto bodyArea = std::make_unique<InputArea>();
    view.bodyArea = bodyArea.get();
    view.transformNode = view.bodyArea;
    view.transformNode->setFrameSize(view.intrinsicWidth, view.intrinsicHeight);
    view.transformNode->setPosition(
        widgetState.cx - view.intrinsicWidth * 0.5f, widgetState.cy - view.intrinsicHeight * 0.5f
    );
    view.transformNode->setRotation(widgetState.rotationRad);
    {
      float flipScaleX = 1.0f;
      float flipScaleY = 1.0f;
      desktop_widgets::widgetNodeScale(widgetState, flipScaleX, flipScaleY);
      view.transformNode->setScale(flipScaleX, flipScaleY);
    }
    view.transformNode->setOpacity(widgetState.enabled ? 1.0f : kDisabledWidgetOpacity);
    view.transformNode->setZIndex(lockscreen_login_box::isLoginBoxWidget(widgetState) ? 3 : 4);
    if (isWidgetSelected(widgetState.id) && widgetState.id != m_selectedWidgetId) {
      view.transformNode->setZIndex(100);
    }
    view.bodyArea->setOnPress([this, id = widgetState.id](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      if (data.pressed) {
        handleWidgetPress(id);
      } else if (m_drag.mode == DragMode::Move && m_drag.widgetId == id) {
        finishDrag();
      }
    });
    view.bodyArea->setOnMotion([this, id = widgetState.id](const InputArea::PointerData& /*data*/) {
      if (m_drag.mode == DragMode::Move && m_drag.widgetId == id) {
        updateDrag();
      }
    });
    view.transformNode->addChild(widget->releaseRoot());

    root->addChild(std::move(bodyArea));
    view.widget = std::move(widget);
    surface.views.emplace(widgetState.id, std::move(view));
  }

  for (const std::string& selectedId : m_selectedWidgetIds) {
    if (selectedId == m_selectedWidgetId) {
      continue;
    }
    const auto secondaryIt = surface.views.find(selectedId);
    const DesktopWidgetState* secondaryState = findWidgetState(selectedId);
    if (secondaryIt == surface.views.end()
        || secondaryState == nullptr
        || lockscreen_login_box::isLoginBoxWidget(*secondaryState)) {
      continue;
    }

    SecondarySelectionVisual visual;
    visual.widgetId = selectedId;

    auto borderTransform = std::make_unique<Node>();
    borderTransform->setZIndex(102);
    borderTransform->setHitTestVisible(false);
    visual.transform = borderTransform.get();

    auto borderShadow = ui::box({
        .fill = clearColorSpec(),
        .radius = Style::scaledRadiusMd() + kShadowExpand,
        .configure = [](Box& box) { box.setBorder(kShadowColor, kSelectionStroke + kShadowExpand * 2.0f); },
    });
    borderShadow->setZIndex(0);
    visual.borderShadow = borderShadow.get();
    borderTransform->addChild(std::move(borderShadow));

    auto border = ui::box({
        .fill = clearColorSpec(),
        .radius = Style::scaledRadiusMd(),
        .configure = [](Box& box) { box.setBorder(colorSpecFromRole(ColorRole::Primary), kSelectionStroke); },
    });
    border->setZIndex(1);
    visual.border = border.get();
    borderTransform->addChild(std::move(border));
    root->addChild(std::move(borderTransform));
    surface.secondarySelections.push_back(visual);
  }

  const auto selectedIt = surface.views.find(m_selectedWidgetId);
  const DesktopWidgetState* selectedState = findWidgetState(m_selectedWidgetId);
  const bool selectedIsLoginBox = selectedState != nullptr && lockscreen_login_box::isLoginBoxWidget(*selectedState);
  if (selectedIt != surface.views.end()) {
    selectedIt->second.bodyArea->setZIndex(101);

    auto selectionFrameTransform = std::make_unique<Node>();
    selectionFrameTransform->setZIndex(100);
    surface.selectionFrameTransform = selectionFrameTransform.get();

    if (!selectedIsLoginBox) {
      auto ringShadow = ui::box({
          .fill = clearColorSpec(),
          .radius = Style::scaledRadiusMd() + kRotatePadding + kShadowExpand,
          .configure = [](Box& box) { box.setBorder(kShadowColor, 1.0f + kShadowExpand * 2.0f); },
      });
      surface.rotationRingShadow = ringShadow.get();
      surface.selectionFrameTransform->addChild(std::move(ringShadow));

      auto ring = ui::box({
          .fill = clearColorSpec(),
          .radius = Style::scaledRadiusMd() + kRotatePadding,
          .configure = [](Box& box) { box.setBorder(colorSpecFromRole(ColorRole::Primary), 1.0f); },
      });
      ring->setZIndex(1);
      surface.rotationRing = ring.get();
      surface.selectionFrameTransform->addChild(std::move(ring));

      auto rotateArea = std::make_unique<InputArea>();
      rotateArea->setZIndex(1);
      rotateArea->setOnPress([this, id = m_selectedWidgetId](const InputArea::PointerData& data) {
        if (data.button != BTN_LEFT) {
          return;
        }
        if (data.pressed) {
          startDrag(DragMode::Rotate, id, false);
        } else if (m_drag.mode == DragMode::Rotate && m_drag.widgetId == id) {
          finishDrag();
        }
      });
      rotateArea->setOnMotion([this, id = m_selectedWidgetId](const InputArea::PointerData& /*data*/) {
        if (m_drag.mode == DragMode::Rotate && m_drag.widgetId == id) {
          updateDrag();
        }
      });
      surface.rotateArea = rotateArea.get();
      surface.selectionFrameTransform->addChild(std::move(rotateArea));
    }

    root->addChild(std::move(selectionFrameTransform));

    auto selectionBorderTransform = std::make_unique<Node>();
    selectionBorderTransform->setZIndex(102);
    selectionBorderTransform->setHitTestVisible(false);
    surface.selectionBorderTransform = selectionBorderTransform.get();

    auto selectionBorderShadow = ui::box({
        .fill = clearColorSpec(),
        .radius = Style::scaledRadiusMd() + kShadowExpand,
        .configure = [](Box& box) { box.setBorder(kShadowColor, kSelectionStroke + kShadowExpand * 2.0f); },
    });
    surface.selectionBorderShadow = selectionBorderShadow.get();
    selectionBorderTransform->addChild(std::move(selectionBorderShadow));

    auto selectionBorder = ui::box({
        .fill = clearColorSpec(),
        .radius = Style::scaledRadiusMd(),
        .configure = [](Box& box) { box.setBorder(colorSpecFromRole(ColorRole::Primary), kSelectionStroke); },
    });
    selectionBorder->setZIndex(1);
    surface.selectionBorder = selectionBorder.get();
    selectionBorderTransform->addChild(std::move(selectionBorder));
    root->addChild(std::move(selectionBorderTransform));

    for (std::size_t i = 0; i < kScaleCornerCount; ++i) {
      const auto corner = static_cast<ScaleCorner>(i);

      auto scaleHandleShadow = ui::box({
          .fill = clearColorSpec(),
          .radius = Style::scaledRadiusSm() + kShadowExpand,
          .configure = [](Box& box) { box.setBorder(kShadowColor, kShadowExpand); },
      });
      scaleHandleShadow->setZIndex(103);
      surface.scaleHandleShadows[i] = scaleHandleShadow.get();
      root->addChild(std::move(scaleHandleShadow));

      auto scaleHandle = ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = Style::scaledRadiusSm(),
      });
      scaleHandle->setZIndex(104);
      surface.scaleHandles[i] = scaleHandle.get();
      root->addChild(std::move(scaleHandle));

      auto scaleArea = std::make_unique<InputArea>();
      scaleArea->setZIndex(105);
      scaleArea->setOnPress([this, id = m_selectedWidgetId, corner](const InputArea::PointerData& data) {
        if (data.button != BTN_LEFT) {
          return;
        }
        if (data.pressed) {
          startDrag(DragMode::Scale, id, false, corner);
        } else if (m_drag.mode == DragMode::Scale && m_drag.widgetId == id) {
          finishDrag();
        }
      });
      scaleArea->setOnMotion([this, id = m_selectedWidgetId](const InputArea::PointerData& /*data*/) {
        if (m_drag.mode == DragMode::Scale && m_drag.widgetId == id) {
          updateDrag();
        }
      });
      surface.scaleAreas[i] = scaleArea.get();
      root->addChild(std::move(scaleArea));
    }

    updateSelectionVisuals(surface);
  } else if (!surface.secondarySelections.empty()) {
    updateSelectionVisuals(surface);
  }

  auto toolbarHandleArea = std::make_unique<InputArea>();
  toolbarHandleArea->setParticipatesInLayout(false);
  toolbarHandleArea->setZIndex(1);
  toolbarHandleArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
  toolbarHandleArea->setOnPress([this, outputName = surface.outputName](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      startToolbarDrag(outputName);
    } else if (m_drag.mode == DragMode::ToolbarMove && m_drag.surfaceOutputName == outputName) {
      finishDrag();
    }
  });
  toolbarHandleArea->setOnMotion([this, outputName = surface.outputName](const InputArea::PointerData& /*data*/) {
    if (m_drag.mode == DragMode::ToolbarMove && m_drag.surfaceOutputName == outputName) {
      updateDrag();
    }
  });
  auto* toolbarHandleAreaPtr = toolbarHandleArea.get();

  const auto selectedWidgetIt = std::ranges::find(m_snapshot.widgets, m_selectedWidgetId, &DesktopWidgetState::id);
  const bool hasSelectedWidget = selectedWidgetIt != m_snapshot.widgets.end();
  const bool selectedWidgetEnabled = hasSelectedWidget ? selectedWidgetIt->enabled : false;
  const bool canSendSelectedToBack =
      hasSelectedWidget && !selectedIsLoginBox && selectedWidgetIt != m_snapshot.widgets.begin();
  const bool canBringSelectedToFront =
      hasSelectedWidget && !selectedIsLoginBox && std::next(selectedWidgetIt) != m_snapshot.widgets.end();

  const bool selectedFlipX = hasSelectedWidget && selectedWidgetIt->flipX;
  const bool selectedFlipY = hasSelectedWidget && selectedWidgetIt->flipY;

  const bool canCloneSelected = hasSelectedWidget
      && !selectedIsLoginBox
      && std::ranges::any_of(m_selectedWidgetIds, [this](const std::string& id) {
                                  const DesktopWidgetState* state = findWidgetState(id);
                                  return state != nullptr && !lockscreen_login_box::isLoginBoxWidget(*state);
                                });

  const auto typeOptions = desktop_settings::desktopWidgetTypeOptions();
  std::vector<std::string> typeLabels;
  typeLabels.reserve(typeOptions.size());
  std::size_t selectedTypeIndex = 0;
  for (std::size_t i = 0; i < typeOptions.size(); ++i) {
    typeLabels.push_back(typeOptions[i].label);
    if (typeOptions[i].value == m_addWidgetType) {
      selectedTypeIndex = i;
    }
  }

  const std::array<std::int32_t, 5> gridSizes{8, 16, 24, 32, 64};
  std::size_t selectedGridIndex = 1;
  for (std::size_t i = 0; i < gridSizes.size(); ++i) {
    if (gridSizes[i] == m_snapshot.grid.cellSize) {
      selectedGridIndex = i;
      break;
    }
  }

  Flex* toolbarPtr = nullptr;
  Flex* toolbarHandlePtr = nullptr;
  auto
      toolbar =
          ui::
              row(
                  {
                      .out = &toolbarPtr,
                      .align = FlexAlign::Center,
                      .gap = Style::spaceSm,
                      .configure =
                          [](Flex& flex) {
                            flex.setPadding(Style::spaceSm, Style::spaceMd);
                            flex.setFill(colorSpecFromRole(ColorRole::Surface, 0.94f));
                            flex.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                            flex.setRadius(Style::scaledRadiusXl());
                            flex.setZIndex(200);
                          },
                  },
                  ui::row(
                      {
                          .out = &toolbarHandlePtr,
                          .align = FlexAlign::Center,
                          .gap = Style::spaceXs,
                          .paddingV = Style::spaceXs,
                          .paddingH = Style::spaceSm,
                          .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
                          .radius = Style::scaledRadiusLg(),
                          .minHeight = Style::controlHeightSm,
                      },
                      ui::glyph({
                          .glyph = "menu-2",
                          .glyphSize = 14.0f,
                      }),
                      ui::label({
                          .text = i18n::tr("desktop-widgets.editor.title"),
                          .fontSize = Style::fontSizeBody,
                          .fontWeight = FontWeight::Bold,
                      }),
                      std::move(toolbarHandleArea)
                  ),
                  ui::select({
                      .options = std::move(typeLabels),
                      .selectedIndex = selectedTypeIndex,
                      .controlHeight = Style::controlHeightSm,
                      .onSelectionChanged =
                          [this](std::size_t index, std::string_view) {
                            const auto options = desktop_settings::desktopWidgetTypeOptions();
                            if (index < options.size()) {
                              m_addWidgetType = options[index].value;
                            }
                          },
                      .configure = [](Select& select) { select.setMinWidth(200.0f); },
                  }),
                  ui::button({
                      .glyph = "plus",
                      .variant = ButtonVariant::Primary,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.add"),
                      .onClick =
                          [this, outputName = surface.outputName]() {
                            deferEditorMutation([this, outputName]() { addWidget(outputName, m_addWidgetType); });
                          },
                  }),
                  ui::button({
                      .glyph = "copy-plus",
                      .enabled = canCloneSelected,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.clone"),
                      .onClick = [this]() { deferEditorMutation([this]() { cloneSelectedWidgets(); }); },
                  }),
                  ui::button({
                      .glyph = "stack-back",
                      .enabled = canSendSelectedToBack,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.stack-back"),
                      .onClick = [this]() { deferEditorMutation([this]() { sendSelectedWidgetToBack(); }); },
                  }),
                  ui::button({
                      .glyph = "stack-front",
                      .enabled = canBringSelectedToFront,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.stack-front"),
                      .onClick = [this]() { deferEditorMutation([this]() { bringSelectedWidgetToFront(); }); },
                  }),
                  ui::button({
                      .glyph = "flip-vertical",
                      .enabled = hasSelectedWidget && !selectedIsLoginBox,
                      .selected = selectedFlipX,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.flip-horizontal"),
                      .onClick = [this]() { deferEditorMutation([this]() { flipSelectedWidgetHorizontal(); }); },
                  }),
                  ui::button({
                      .glyph = "flip-horizontal",
                      .enabled = hasSelectedWidget && !selectedIsLoginBox,
                      .selected = selectedFlipY,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.flip-vertical"),
                      .onClick = [this]() { deferEditorMutation([this]() { flipSelectedWidgetVertical(); }); },
                  }),
                  ui::button({
                      .glyph = "settings",
                      .enabled = hasSelectedWidget,
                      .selected = m_inspectorOpen,
                      .variant = ButtonVariant::Default,
                      .tooltip = i18n::tr("desktop-widgets.editor.actions.settings"),
                      .onClick =
                          [this]() {
                            deferEditorMutation([this]() {
                              m_inspectorOpen = !m_inspectorOpen;
                              requestLayout();
                            });
                          },
                  }),
                  [&]() -> std::unique_ptr<Node> {
                    bool canToggleVisibility = hasSelectedWidget;
                    if (hasSelectedWidget
                        && lockscreen_login_box::isLoginBoxWidget(*selectedWidgetIt)
                        && selectedWidgetIt->enabled) {
                      int enabledCount = 0;
                      for (const auto& w : m_snapshot.widgets) {
                        if (lockscreen_login_box::isLoginBoxWidget(w) && w.enabled) {
                          enabledCount++;
                        }
                      }
                      if (enabledCount <= 1) {
                        canToggleVisibility = false;
                      }
                    }
                    return ui::button({
                        .glyph = selectedWidgetEnabled ? "eye" : "eye-off",
                        .enabled = canToggleVisibility,
                        .selected = selectedWidgetEnabled,
                        .variant = ButtonVariant::Default,
                        .tooltip = selectedWidgetEnabled ? i18n::tr("desktop-widgets.editor.actions.hide")
                                                         : i18n::tr("desktop-widgets.editor.actions.show"),
                        .onClick = [this]() { deferEditorMutation([this]() { toggleSelectedWidgetEnabled(); }); },
                    });
                  }(),
                  ui::button(
                      {
                          .glyph = "trash",
                          .enabled = hasSelectedWidget && !selectedIsLoginBox,
                          .variant = ButtonVariant::Destructive,
                          .tooltip = i18n::tr("desktop-widgets.editor.actions.trash"),
                          .onClick = [this]() { deferEditorMutation([this]() { removeSelectedWidget(); }); },
                      }
                  ),
                  ui::separator(
                      {
                          .orientation = SeparatorOrientation::VerticalRule,
                          .width = Style::borderWidth,
                          .height = Style::controlHeight,
                      }
                  ),
                  ui::button(
                      {
                          .text = i18n::tr("desktop-widgets.editor.state.grid"),
                          .controlHeight = Style::controlHeightSm,
                          .selected = m_snapshot.grid.visible,
                          .variant = ButtonVariant::Default,
                          .onClick =
                              [this]() {
                                deferEditorMutation([this]() {
                                  m_snapshot.grid.visible = !m_snapshot.grid.visible;
                                  requestLayout();
                                });
                              },
                      }
                  ),
                  ui::select(
                      {
                          .options = std::vector<std::string>{"8", "16", "24", "32", "64"},
                          .selectedIndex = selectedGridIndex,
                          .controlHeight = Style::controlHeightSm,
                          .onSelectionChanged =
                              [this](std::size_t /*index*/, std::string_view text) {
                                deferEditorMutation([this, value = std::string(text)]() {
                                  try {
                                    m_snapshot.grid.cellSize = std::stoi(value);
                                    requestLayout();
                                  } catch (...) {
                                  }
                                });
                              },
                      }
                  ),
                  ui::button({
                      .text = i18n::tr("desktop-widgets.editor.actions.done"),
                      .controlHeight = Style::controlHeightSm,
                      .variant = ButtonVariant::Secondary,
                      .onClick = [this]() { requestExit(); },
                  })
              );

  surface.toolbar = toolbarPtr;
  root->addChild(std::move(toolbar));
  toolbarPtr->layout(*m_renderContext);
  toolbarHandleAreaPtr->setPosition(0.0f, 0.0f);
  toolbarHandleAreaPtr->setFrameSize(toolbarHandlePtr->width(), toolbarHandlePtr->height());

  if (!surface.toolbarPositionInitialized) {
    surface.toolbarX = std::round((root->width() - toolbarPtr->width()) * 0.5f);
    surface.toolbarY = kToolbarY;
    surface.toolbarPositionInitialized = true;
  }
  clampToolbarPosition(surface, toolbarPtr->width(), toolbarPtr->height());
  toolbarPtr->setPosition(surface.toolbarX, surface.toolbarY);

  if (hasSelectedWidget && m_inspectorOpen) {
    buildInspector(surface, *root, *selectedWidgetIt);
  } else {
    surface.inspector = nullptr;
    if (!hasSelectedWidget) {
      surface.inspectorPositionInitialized = false;
      m_inspectorOpen = false;
    }
  }

  surface.sceneRoot = std::move(root);
  surface.surface->setSceneRoot(surface.sceneRoot.get());
  surface.inputDispatcher.setTextInputContext(surface.surface->wlSurface(), m_wayland->textInputService());
  surface.inputDispatcher.setSceneRoot(surface.sceneRoot.get());
}

void DesktopWidgetsEditor::updateSelectionVisuals(OverlaySurface& surface) {
  for (const SecondarySelectionVisual& secondary : surface.secondarySelections) {
    if (secondary.transform == nullptr || secondary.border == nullptr) {
      continue;
    }
    const auto viewIt = surface.views.find(secondary.widgetId);
    const DesktopWidgetState* state = findWidgetState(secondary.widgetId);
    if (viewIt == surface.views.end() || state == nullptr) {
      continue;
    }

    const float width = viewIt->second.intrinsicWidth;
    const float height = viewIt->second.intrinsicHeight;
    const float left = state->cx - width * 0.5f;
    const float top = state->cy - height * 0.5f;

    secondary.transform->setFrameSize(width, height);
    secondary.transform->setPosition(left, top);
    secondary.transform->setRotation(state->rotationRad);

    if (secondary.borderShadow != nullptr) {
      secondary.borderShadow->setPosition(-kShadowExpand, -kShadowExpand);
      secondary.borderShadow->setFrameSize(width + kShadowExpand * 2.0f, height + kShadowExpand * 2.0f);
    }
    secondary.border->setPosition(0.0f, 0.0f);
    secondary.border->setFrameSize(width, height);
  }

  const auto selectedIt = surface.views.find(m_selectedWidgetId);
  const DesktopWidgetState* state = findWidgetState(m_selectedWidgetId);
  if (selectedIt == surface.views.end()
      || state == nullptr
      || surface.selectionFrameTransform == nullptr
      || surface.selectionBorderTransform == nullptr
      || surface.selectionBorder == nullptr) {
    return;
  }
  const bool selectedIsLoginBox = lockscreen_login_box::isLoginBoxWidget(*state);
  for (std::size_t i = 0; i < kScaleCornerCount; ++i) {
    if (surface.scaleHandles[i] == nullptr || surface.scaleAreas[i] == nullptr) {
      return;
    }
  }
  if (!selectedIsLoginBox) {
    if (surface.rotationRing == nullptr || surface.rotateArea == nullptr) {
      return;
    }
  }

  const float width = selectedIt->second.intrinsicWidth;
  const float height = selectedIt->second.intrinsicHeight;
  const float left = state->cx - width * 0.5f;
  const float top = state->cy - height * 0.5f;

  surface.selectionFrameTransform->setFrameSize(width, height);
  surface.selectionFrameTransform->setPosition(left, top);
  surface.selectionFrameTransform->setRotation(state->rotationRad);

  surface.selectionBorderTransform->setFrameSize(width, height);
  surface.selectionBorderTransform->setPosition(left, top);
  surface.selectionBorderTransform->setRotation(state->rotationRad);

  if (!selectedIsLoginBox) {
    const float ringPadExp = kRotatePadding + kShadowExpand;
    if (surface.rotationRingShadow != nullptr) {
      surface.rotationRingShadow->setPosition(-ringPadExp, -ringPadExp);
      surface.rotationRingShadow->setFrameSize(width + ringPadExp * 2.0f, height + ringPadExp * 2.0f);
    }

    surface.rotationRing->setPosition(-kRotatePadding, -kRotatePadding);
    surface.rotationRing->setFrameSize(width + kRotatePadding * 2.0f, height + kRotatePadding * 2.0f);

    surface.rotateArea->setPosition(-kRotatePadding, -kRotatePadding);
    surface.rotateArea->setFrameSize(width + kRotatePadding * 2.0f, height + kRotatePadding * 2.0f);
  }

  if (surface.selectionBorderShadow != nullptr) {
    surface.selectionBorderShadow->setPosition(-kShadowExpand, -kShadowExpand);
    surface.selectionBorderShadow->setFrameSize(width + kShadowExpand * 2.0f, height + kShadowExpand * 2.0f);
  }

  surface.selectionBorder->setPosition(0.0f, 0.0f);
  surface.selectionBorder->setFrameSize(width, height);

  for (std::size_t i = 0; i < kScaleCornerCount; ++i) {
    const CornerSigns signs = cornerSigns(i);
    const auto [cornerX, cornerY] =
        rotatedCorner(state->cx, state->cy, width * 0.5f * signs.x, height * 0.5f * signs.y, state->rotationRad);

    const float shadowSize = kHandleSize + kShadowExpand * 2.0f;
    if (surface.scaleHandleShadows[i] != nullptr) {
      surface.scaleHandleShadows[i]->setPosition(cornerX - shadowSize * 0.5f, cornerY - shadowSize * 0.5f);
      surface.scaleHandleShadows[i]->setFrameSize(shadowSize, shadowSize);
    }

    surface.scaleHandles[i]->setPosition(cornerX - kHandleSize * 0.5f, cornerY - kHandleSize * 0.5f);
    surface.scaleHandles[i]->setFrameSize(kHandleSize, kHandleSize);

    surface.scaleAreas[i]->setPosition(cornerX - kHandleSize, cornerY - kHandleSize);
    surface.scaleAreas[i]->setFrameSize(kHandleSize * 1.5f, kHandleSize * 1.5f);
  }
}

void DesktopWidgetsEditor::applyViewState(
    EditorWidgetView& view, const DesktopWidgetState& state, bool refreshContent
) {
  if (view.widget == nullptr || view.transformNode == nullptr || m_renderContext == nullptr) {
    return;
  }

  if (refreshContent) {
    if (lockscreen_login_box::isLoginBoxWidget(state)) {
      if (auto* loginWidget = dynamic_cast<DesktopLoginBoxWidget*>(view.widget.get())) {
        if (OverlaySurface* surface = findSurfaceForWidget(state.id);
            surface != nullptr && surface->surface != nullptr) {
          loginWidget->setScreenWidth(static_cast<float>(surface->surface->width()));
        }
      }
    }
    view.widget->setContentScale(widgetContentScale());
    view.widget->setBox(state.boxWidth, state.boxHeight);
    view.widget->update(*m_renderContext);
    view.widget->layout(*m_renderContext);
    view.intrinsicWidth = std::max(1.0f, view.widget->intrinsicWidth());
    view.intrinsicHeight = std::max(1.0f, view.widget->intrinsicHeight());
  }

  view.transformNode->setFrameSize(view.intrinsicWidth, view.intrinsicHeight);
  view.transformNode->setPosition(state.cx - view.intrinsicWidth * 0.5f, state.cy - view.intrinsicHeight * 0.5f);
  view.transformNode->setRotation(state.rotationRad);
  {
    float flipScaleX = 1.0f;
    float flipScaleY = 1.0f;
    desktop_widgets::widgetNodeScale(state, flipScaleX, flipScaleY);
    view.transformNode->setScale(flipScaleX, flipScaleY);
  }
  view.transformNode->setOpacity(state.enabled ? 1.0f : kDisabledWidgetOpacity);
}

void DesktopWidgetsEditor::updateViewTransforms(const std::string* relayoutWidgetId) {
  for (auto& surface : m_surfaces) {
    for (auto& [id, view] : surface->views) {
      const DesktopWidgetState* state = findWidgetState(id);
      if (state == nullptr) {
        continue;
      }
      applyViewState(view, *state, relayoutWidgetId != nullptr && *relayoutWidgetId == id);
    }
    updateSelectionVisuals(*surface);
  }
}

void DesktopWidgetsEditor::applyScaleDragPreview(const DesktopWidgetState& state) {
  EditorWidgetView* view = findView(m_drag.widgetId);
  if (view == nullptr || view->widget == nullptr || view->transformNode == nullptr) {
    return;
  }

  view->intrinsicWidth = std::max(1.0f, state.boxWidth);
  view->intrinsicHeight = std::max(1.0f, state.boxHeight);

  // Handles and the tile follow the cursor immediately; the content re-fits via a real layout in
  // prepareFrame, coalesced to one per frame from the pointer-move stream.
  view->widget->setBox(state.boxWidth, state.boxHeight);

  float flipScaleX = 1.0f;
  float flipScaleY = 1.0f;
  desktop_widgets::widgetNodeScale(state, flipScaleX, flipScaleY);

  view->transformNode->setFrameSize(view->intrinsicWidth, view->intrinsicHeight);
  view->transformNode->setPosition(state.cx - view->intrinsicWidth * 0.5f, state.cy - view->intrinsicHeight * 0.5f);
  view->transformNode->setRotation(state.rotationRad);
  view->transformNode->setScale(flipScaleX, flipScaleY);
  view->transformNode->setOpacity(state.enabled ? 1.0f : kDisabledWidgetOpacity);

  OverlaySurface* dragSurface = findSurfaceForWidget(m_drag.widgetId);
  for (auto& surface : m_surfaces) {
    updateSelectionVisuals(*surface);
  }

  // Request a plain relayout of the dragged surface (re-fit content) — NOT the editor's
  // requestLayout(), which rebuilds the whole scene every frame and would destroy the captured
  // resize-handle node, dropping the pointer grab mid-drag.
  if (dragSurface != nullptr && dragSurface->surface != nullptr) {
    dragSurface->surface->requestLayout();
  }
}

void DesktopWidgetsEditor::addWidget(const std::string& outputName, const std::string& type) {
  if (!m_open || m_wayland == nullptr) {
    return;
  }

  float centerX = 320.0f;
  float centerY = 240.0f;
  if (const WaylandOutput* output = desktop_widgets::resolveEffectiveOutput(*m_wayland, outputName);
      output != nullptr) {
    const int logicalWidth = output->effectiveLogicalWidth();
    const int logicalHeight = output->effectiveLogicalHeight();
    centerX = static_cast<float>(std::max(1, logicalWidth)) * 0.5f;
    centerY = static_cast<float>(std::max(1, logicalHeight)) * 0.5f;
  }

  DesktopWidgetState widget;
  widget.id = nextWidgetId();
  widget.type = type.empty() ? "clock" : type;
  widget.outputName = outputName;
  widget.cx = centerX;
  widget.cy = centerY;
  // box 0 = auto-fit content's natural size; the user resizes from there.
  widget.boxWidth = 0.0f;
  widget.boxHeight = 0.0f;
  widget.rotationRad = 0.0f;
  if (widget.type == "audio_visualizer") {
    widget.settings.emplace("bands", static_cast<std::int64_t>(32));
    widget.settings.emplace("show_when_idle", true);
  }
  if (widget.type == "fancy_audio_visualizer") {
    widget.settings.emplace("background", false);
  }
  if (widget.type == "button") {
    widget.settings.emplace("background", true);
    widget.settings.emplace("glyph", std::string("heart"));
    widget.settings.emplace("variant", std::string("default"));
  }
  if (widget.type == "sysmon") {
    widget.settings.emplace("stat", std::string("cpu_usage"));
    widget.settings.emplace("stat2", std::string("cpu_temp"));
  }

  if (widget.type == "sticker") {
    widget.settings.emplace("opacity", 1.0);
    auto widgetId = widget.id;
    m_snapshot.widgets.push_back(std::move(widget));

    FileDialogOptions options;
    options.mode = FileDialogMode::Open;
    options.title = i18n::tr("desktop-widgets.editor.dialogs.select-sticker-image");
    options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".gif"};
    if (!FileDialog::open(std::move(options), [this, widgetId](std::optional<std::filesystem::path> result) {
          deferEditorMutation([this, widgetId, result = std::move(result)]() {
            auto* state = findWidgetState(widgetId);
            if (state == nullptr) {
              return;
            }
            if (result) {
              state->settings["image_path"] = result->string();
            } else {
              std::erase_if(m_snapshot.widgets, [&](const auto& w) { return w.id == widgetId; });
              m_selectedWidgetIds.erase(widgetId);
              if (m_selectedWidgetId == widgetId) {
                m_selectedWidgetId = m_selectedWidgetIds.empty() ? "" : *m_selectedWidgetIds.begin();
              }
            }
            requestLayout();
          });
        })) {
      std::erase_if(m_snapshot.widgets, [&](const auto& w) { return w.id == widgetId; });
      requestLayout();
      return;
    }

    setSingleSelection(m_snapshot.widgets.back().id);
    requestLayout();
    return;
  }

  m_snapshot.widgets.push_back(std::move(widget));
  setSingleSelection(m_snapshot.widgets.back().id);
  requestLayout();
}

void DesktopWidgetsEditor::removeSelectedWidget() {
  if (m_selectedWidgetIds.empty()) {
    return;
  }
  std::erase_if(m_snapshot.widgets, [this](const auto& widget) {
    if (!m_selectedWidgetIds.contains(widget.id)) {
      return false;
    }
    return !lockscreen_login_box::isLoginBoxWidget(widget);
  });
  clearSelection();
  requestLayout();
}

void DesktopWidgetsEditor::toggleSelectedWidgetEnabled() {
  if (m_selectedWidgetId.empty()) {
    return;
  }
  DesktopWidgetState* state = findWidgetState(m_selectedWidgetId);
  if (state == nullptr) {
    return;
  }

  if (lockscreen_login_box::isLoginBoxWidget(*state) && state->enabled) {
    int enabledCount = 0;
    for (const auto& w : m_snapshot.widgets) {
      if (lockscreen_login_box::isLoginBoxWidget(w) && w.enabled) {
        enabledCount++;
      }
    }
    if (enabledCount <= 1) {
      return;
    }
  }

  state->enabled = !state->enabled;
  requestLayout();
}

void DesktopWidgetsEditor::sendSelectedWidgetToBack() {
  if (m_selectedWidgetId.empty()) {
    return;
  }
  const DesktopWidgetState* state = findWidgetState(m_selectedWidgetId);
  if (state != nullptr && lockscreen_login_box::isLoginBoxWidget(*state)) {
    return;
  }

  auto it = std::ranges::find(m_snapshot.widgets, m_selectedWidgetId, &DesktopWidgetState::id);
  if (it == m_snapshot.widgets.end() || it == m_snapshot.widgets.begin()) {
    return;
  }

  std::rotate(m_snapshot.widgets.begin(), it, std::next(it));
  requestLayout();
}

void DesktopWidgetsEditor::bringSelectedWidgetToFront() {
  if (m_selectedWidgetId.empty()) {
    return;
  }
  const DesktopWidgetState* state = findWidgetState(m_selectedWidgetId);
  if (state != nullptr && lockscreen_login_box::isLoginBoxWidget(*state)) {
    return;
  }

  auto it = std::ranges::find(m_snapshot.widgets, m_selectedWidgetId, &DesktopWidgetState::id);
  if (it == m_snapshot.widgets.end() || std::next(it) == m_snapshot.widgets.end()) {
    return;
  }

  std::rotate(it, std::next(it), m_snapshot.widgets.end());
  requestLayout();
}

void DesktopWidgetsEditor::flipSelectedWidgetHorizontal() {
  if (m_selectedWidgetIds.empty()) {
    return;
  }
  for (const std::string& id : m_selectedWidgetIds) {
    DesktopWidgetState* state = findWidgetState(id);
    if (state == nullptr || lockscreen_login_box::isLoginBoxWidget(*state)) {
      continue;
    }
    state->flipX = !state->flipX;
  }
  updateViewTransforms();
  requestLayout();
}

void DesktopWidgetsEditor::flipSelectedWidgetVertical() {
  if (m_selectedWidgetIds.empty()) {
    return;
  }
  for (const std::string& id : m_selectedWidgetIds) {
    DesktopWidgetState* state = findWidgetState(id);
    if (state == nullptr || lockscreen_login_box::isLoginBoxWidget(*state)) {
      continue;
    }
    state->flipY = !state->flipY;
  }
  updateViewTransforms();
  requestLayout();
}

float DesktopWidgetsEditor::duplicateOffset() const {
  if (shouldSnap() && m_snapshot.grid.cellSize > 0) {
    return static_cast<float>(m_snapshot.grid.cellSize);
  }
  return 24.0f;
}

std::vector<DesktopWidgetState> DesktopWidgetsEditor::selectedWidgetTemplates() const {
  std::vector<DesktopWidgetState> templates;
  for (const auto& widget : m_snapshot.widgets) {
    if (!m_selectedWidgetIds.contains(widget.id) || lockscreen_login_box::isLoginBoxWidget(widget)) {
      continue;
    }
    templates.push_back(widget);
  }
  return templates;
}

std::vector<std::string> DesktopWidgetsEditor::insertWidgetCopies(
    const std::vector<DesktopWidgetState>& templates, float offsetX, float offsetY, bool selectInserted,
    const std::string& targetOutputName
) {
  if (templates.empty() || m_wayland == nullptr) {
    return {};
  }

  std::vector<std::string> insertedIds;
  insertedIds.reserve(templates.size());
  for (const DesktopWidgetState& templateState : templates) {
    if (lockscreen_login_box::isLoginBoxWidget(templateState)) {
      continue;
    }

    DesktopWidgetState copy = templateState;
    copy.id = nextWidgetId();
    if (!targetOutputName.empty()) {
      copy.outputName = targetOutputName;
    }
    copy.cx += offsetX;
    copy.cy += offsetY;

    const float intrinsicWidth = copy.boxWidth > 0.0f ? copy.boxWidth : 96.0f;
    const float intrinsicHeight = copy.boxHeight > 0.0f ? copy.boxHeight : 96.0f;
    desktop_widgets::clampStateToOutput(*m_wayland, copy, intrinsicWidth, intrinsicHeight);

    insertedIds.push_back(copy.id);
    m_snapshot.widgets.push_back(std::move(copy));
  }

  if (insertedIds.empty()) {
    return insertedIds;
  }

  if (selectInserted) {
    clearSelection();
    for (const std::string& id : insertedIds) {
      m_selectedWidgetIds.insert(id);
    }
    m_selectedWidgetId = insertedIds.back();
  }

  requestLayout();
  return insertedIds;
}

void DesktopWidgetsEditor::cloneSelectedWidgets() {
  const std::vector<DesktopWidgetState> templates = selectedWidgetTemplates();
  if (templates.empty()) {
    return;
  }
  const float step = duplicateOffset();
  insertWidgetCopies(templates, step, step, true);
}

void DesktopWidgetsEditor::copySelectedWidgets() {
  m_widgetClipboard = selectedWidgetTemplates();
  m_pasteCount = 0;
}

void DesktopWidgetsEditor::pasteWidgets() {
  if (m_widgetClipboard.empty()) {
    return;
  }
  const std::string targetOutputName = currentPointerOutputName();
  ++m_pasteCount;
  const float step = duplicateOffset();
  const float offset = step * static_cast<float>(m_pasteCount);
  insertWidgetCopies(m_widgetClipboard, offset, offset, true, targetOutputName);
}

std::string DesktopWidgetsEditor::currentPointerOutputName() const {
  for (const auto& surface : m_surfaces) {
    if (surface->pointerInside) {
      return surface->outputName;
    }
  }
  if (m_wayland != nullptr) {
    if (const OverlaySurface* surface = findSurface(m_wayland->lastPointerSurface()); surface != nullptr) {
      return surface->outputName;
    }
  }
  return {};
}

void DesktopWidgetsEditor::startToolbarDrag(const std::string& outputName) {
  OverlaySurface* surface = findSurface(outputName);
  if (surface == nullptr || surface->toolbar == nullptr) {
    return;
  }

  m_drag = {};
  m_drag.mode = DragMode::ToolbarMove;
  m_drag.startSceneX = m_currentEventSceneX;
  m_drag.startSceneY = m_currentEventSceneY;
  m_drag.surfaceOutputName = outputName;
  m_drag.initialToolbarX = surface->toolbarX;
  m_drag.initialToolbarY = surface->toolbarY;
}

void DesktopWidgetsEditor::clampToolbarPosition(OverlaySurface& surface, float toolbarWidth, float toolbarHeight) {
  if (surface.surface == nullptr) {
    return;
  }

  const float maxX = std::max(0.0f, static_cast<float>(surface.surface->width()) - toolbarWidth);
  const float maxY = std::max(0.0f, static_cast<float>(surface.surface->height()) - toolbarHeight);
  surface.toolbarX = std::clamp(surface.toolbarX, 0.0f, maxX);
  surface.toolbarY = std::clamp(surface.toolbarY, 0.0f, maxY);
}

void DesktopWidgetsEditor::startInspectorDrag(const std::string& outputName) {
  OverlaySurface* surface = findSurface(outputName);
  if (surface == nullptr || surface->inspector == nullptr) {
    return;
  }

  m_drag = {};
  m_drag.mode = DragMode::InspectorMove;
  m_drag.startSceneX = m_currentEventSceneX;
  m_drag.startSceneY = m_currentEventSceneY;
  m_drag.surfaceOutputName = outputName;
  m_drag.initialInspectorX = surface->inspectorX;
  m_drag.initialInspectorY = surface->inspectorY;
}

void DesktopWidgetsEditor::clampInspectorPosition(
    OverlaySurface& surface, float inspectorWidth, float inspectorHeight
) {
  if (surface.surface == nullptr) {
    return;
  }

  const float maxX = std::max(0.0f, static_cast<float>(surface.surface->width()) - inspectorWidth);
  const float maxY = std::max(0.0f, static_cast<float>(surface.surface->height()) - inspectorHeight);
  surface.inspectorX = std::clamp(surface.inspectorX, 0.0f, maxX);
  surface.inspectorY = std::clamp(surface.inspectorY, 0.0f, maxY);
}

// buildInspector and applySettingChange are in desktop_widgets_editor_settings.cpp

void DesktopWidgetsEditor::deferEditorMutation(std::function<void()> action) {
  DeferredCall::callLater([this, action = std::move(action)]() mutable {
    if (m_open) {
      action();
    }
  });
}

void DesktopWidgetsEditor::requestExit() {
  if (!m_exitRequestedCallback) {
    return;
  }
  DeferredCall::callLater([this]() {
    if (m_open && m_exitRequestedCallback) {
      m_exitRequestedCallback();
    }
  });
}

void DesktopWidgetsEditor::startDrag(
    DragMode mode, const std::string& widgetId, bool rebuildOnFinish, ScaleCorner scaleCorner
) {
  DesktopWidgetState* state = findWidgetState(widgetId);
  if (state == nullptr) {
    return;
  }
  if (lockscreen_login_box::isLoginBoxWidget(*state) && mode != DragMode::Move && mode != DragMode::Scale) {
    return;
  }

  EditorWidgetView* view = findView(widgetId);
  if (view == nullptr) {
    return;
  }

  m_drag.mode = mode;
  m_drag.widgetId = widgetId;
  m_drag.startSceneX = m_currentEventSceneX;
  m_drag.startSceneY = m_currentEventSceneY;
  m_drag.initialState = *state;
  m_drag.intrinsicWidth = view->intrinsicWidth;
  m_drag.intrinsicHeight = view->intrinsicHeight;
  m_drag.scaleCorner = scaleCorner;
  m_drag.rebuildOnFinish = rebuildOnFinish;
  if (OverlaySurface* surface = findSurfaceForWidget(widgetId); surface != nullptr) {
    m_drag.surfaceOutputName = surface->outputName;
  }
  if (mode == DragMode::Move) {
    m_drag.moveSourceOutputName = m_drag.surfaceOutputName;
    m_drag.movePointerOffsetX = state->cx - m_drag.startSceneX;
    m_drag.movePointerOffsetY = state->cy - m_drag.startSceneY;
  }

  m_drag.groupInitialStates.clear();
  if (mode == DragMode::Move && m_selectedWidgetIds.size() > 1 && isWidgetSelected(widgetId)) {
    const std::string anchorOutput = effectiveOutputName(*state);
    for (const std::string& selectedId : m_selectedWidgetIds) {
      if (selectedId == widgetId) {
        continue;
      }
      const DesktopWidgetState* otherState = findWidgetState(selectedId);
      if (otherState == nullptr || lockscreen_login_box::isLoginBoxWidget(*otherState)) {
        continue;
      }
      if (effectiveOutputName(*otherState) != anchorOutput) {
        continue;
      }
      m_drag.groupInitialStates.emplace(selectedId, *otherState);
    }
  }

  if (mode == DragMode::Scale && view->widget != nullptr && m_renderContext != nullptr) {
    if (OverlaySurface* surface = findSurfaceForWidget(widgetId); surface != nullptr && surface->surface != nullptr) {
      m_renderContext->makeCurrent(surface->surface->renderTarget());
    }
    applyViewState(*view, *state, true);
    m_drag.intrinsicWidth = view->intrinsicWidth;
    m_drag.intrinsicHeight = view->intrinsicHeight;
  }
}

void DesktopWidgetsEditor::updateDrag() {
  if (m_drag.mode == DragMode::None) {
    return;
  }

  if (m_drag.mode == DragMode::ToolbarMove) {
    OverlaySurface* surface = findSurface(m_drag.surfaceOutputName);
    if (surface == nullptr || surface->toolbar == nullptr) {
      return;
    }

    surface->toolbarX = m_drag.initialToolbarX + (m_currentEventSceneX - m_drag.startSceneX);
    surface->toolbarY = m_drag.initialToolbarY + (m_currentEventSceneY - m_drag.startSceneY);
    clampToolbarPosition(*surface, surface->toolbar->width(), surface->toolbar->height());
    surface->toolbar->setPosition(surface->toolbarX, surface->toolbarY);
    surface->surface->requestRedraw();
    return;
  }

  if (m_drag.mode == DragMode::InspectorMove) {
    OverlaySurface* surface = findSurface(m_drag.surfaceOutputName);
    if (surface == nullptr || surface->inspector == nullptr) {
      return;
    }

    surface->inspectorX = m_drag.initialInspectorX + (m_currentEventSceneX - m_drag.startSceneX);
    surface->inspectorY = m_drag.initialInspectorY + (m_currentEventSceneY - m_drag.startSceneY);
    clampInspectorPosition(*surface, surface->inspector->width(), surface->inspector->height());
    surface->inspector->setPosition(surface->inspectorX, surface->inspectorY);
    surface->surface->requestRedraw();
    return;
  }

  DesktopWidgetState* state = findWidgetState(m_drag.widgetId);
  if (state == nullptr) {
    return;
  }

  float gridOriginX = 0.0f;
  float gridOriginY = 0.0f;
  float outputWidth = 0.0f;
  float outputHeight = 0.0f;
  std::vector<float> snapLinesX;
  std::vector<float> snapLinesY;

  float dragSceneX = m_currentEventSceneX;
  float dragSceneY = m_currentEventSceneY;
  if (m_drag.mode == DragMode::Move && m_wayland != nullptr) {
    const WaylandOutput* sourceOutput = desktop_widgets::findOutputByKey(*m_wayland, m_drag.moveSourceOutputName);
    if (sourceOutput == nullptr) {
      sourceOutput = desktop_widgets::resolveStateOutput(*m_wayland, *state);
    }
    if (sourceOutput != nullptr) {
      const double globalX = static_cast<double>(sourceOutput->logicalX) + static_cast<double>(m_currentEventSceneX);
      const double globalY = static_cast<double>(sourceOutput->logicalY) + static_cast<double>(m_currentEventSceneY);
      if (const WaylandOutput* targetOutput = outputAtGlobalPoint(*m_wayland, globalX, globalY);
          targetOutput != nullptr) {
        m_drag.surfaceOutputName = desktop_widgets::outputKey(*targetOutput);
        dragSceneX = static_cast<float>(globalX - static_cast<double>(targetOutput->logicalX));
        dragSceneY = static_cast<float>(globalY - static_cast<double>(targetOutput->logicalY));
      }
    }
  }

  OverlaySurface* dragSurface = findSurface(m_drag.surfaceOutputName);
  if (dragSurface == nullptr) {
    dragSurface = findSurfaceForWidget(m_drag.widgetId);
  }
  if (dragSurface != nullptr && dragSurface->surface != nullptr) {
    outputWidth = static_cast<float>(dragSurface->surface->width());
    outputHeight = static_cast<float>(dragSurface->surface->height());
    gridOriginX = outputWidth * 0.5f;
    gridOriginY = outputHeight * 0.5f;
    snapLinesX = {0.0f, gridOriginX, outputWidth};
    snapLinesY = {0.0f, gridOriginY, outputHeight};

    for (const auto& [id, view] : dragSurface->views) {
      if (id == m_drag.widgetId || m_drag.groupInitialStates.contains(id) || m_selectedWidgetIds.contains(id)) {
        continue;
      }
      const DesktopWidgetState* otherState = findWidgetState(id);
      if (otherState == nullptr) {
        continue;
      }
      const WidgetTransformBounds bounds = computeWidgetTransformBounds(
          otherState->cx, otherState->cy, view.intrinsicWidth, view.intrinsicHeight, 1.0f, otherState->rotationRad
      );
      snapLinesX.push_back(bounds.left);
      snapLinesX.push_back(otherState->cx);
      snapLinesX.push_back(bounds.left + bounds.aabbWidth);
      snapLinesY.push_back(bounds.top);
      snapLinesY.push_back(otherState->cy);
      snapLinesY.push_back(bounds.top + bounds.aabbHeight);
    }
  }
  const float guideThreshold = snapGuideThreshold(m_snapshot.grid.cellSize);
  bool outputAssignmentChanged = false;

  if (m_drag.mode == DragMode::Move) {
    state->cx = dragSceneX + m_drag.movePointerOffsetX;
    state->cy = dragSceneY + m_drag.movePointerOffsetY;
    if (!m_drag.surfaceOutputName.empty()) {
      outputAssignmentChanged = outputAssignmentChanged || (state->outputName != m_drag.surfaceOutputName);
      state->outputName = m_drag.surfaceOutputName;
    }
    if (shouldSnap()) {
      const WidgetTransformBounds bounds = computeWidgetTransformBounds(
          state->cx, state->cy, m_drag.intrinsicWidth, m_drag.intrinsicHeight, 1.0f, state->rotationRad
      );
      state->cx = snapBoundsAxisToTargets(
          state->cx, bounds.aabbWidth, m_snapshot.grid.cellSize, gridOriginX, snapLinesX, guideThreshold
      );
      state->cy = snapBoundsAxisToTargets(
          state->cy, bounds.aabbHeight, m_snapshot.grid.cellSize, gridOriginY, snapLinesY, guideThreshold
      );
    }

    if (std::abs(state->cx - gridOriginX) <= guideThreshold) {
      state->cx = gridOriginX;
    }
    if (std::abs(state->cy - gridOriginY) <= guideThreshold) {
      state->cy = gridOriginY;
    }

    const float deltaX = state->cx - m_drag.initialState.cx;
    const float deltaY = state->cy - m_drag.initialState.cy;
    for (const auto& [groupId, initialState] : m_drag.groupInitialStates) {
      DesktopWidgetState* groupState = findWidgetState(groupId);
      if (groupState == nullptr) {
        continue;
      }
      groupState->cx = initialState.cx + deltaX;
      groupState->cy = initialState.cy + deltaY;
      if (!m_drag.surfaceOutputName.empty()) {
        outputAssignmentChanged = outputAssignmentChanged || (groupState->outputName != m_drag.surfaceOutputName);
        groupState->outputName = m_drag.surfaceOutputName;
      }
      if (m_wayland != nullptr) {
        if (EditorWidgetView* groupView = findView(groupId); groupView != nullptr) {
          desktop_widgets::clampStateToOutput(
              *m_wayland, *groupState, groupView->intrinsicWidth, groupView->intrinsicHeight
          );
        }
      }
    }
  } else if (m_drag.mode == DragMode::Rotate) {
    const float startAngle =
        std::atan2(m_drag.startSceneY - m_drag.initialState.cy, m_drag.startSceneX - m_drag.initialState.cx);
    const float currentAngle =
        std::atan2(m_currentEventSceneY - m_drag.initialState.cy, m_currentEventSceneX - m_drag.initialState.cx);
    float rotation = normalizeAngle(m_drag.initialState.rotationRad + (currentAngle - startAngle));
    if (shouldSnap()) {
      rotation = std::round(rotation / kRotationSnap) * kRotationSnap;
    }
    state->rotationRad = rotation;
  } else if (m_drag.mode == DragMode::Scale) {
    // Resize the widget's box tile. The opposite corner stays fixed (Alt anchors the center).
    // Snapping quantizes the box width/height to whole grid cells; content re-fits the box.
    const CornerSigns signs = cornerSigns(static_cast<std::size_t>(m_drag.scaleCorner));
    const float rot = m_drag.initialState.rotationRad;
    const float cosR = std::cos(rot);
    const float sinR = std::sin(rot);
    const float halfW0 = std::max(0.5f, m_drag.intrinsicWidth * 0.5f);
    const float halfH0 = std::max(0.5f, m_drag.intrinsicHeight * 0.5f);

    float anchorX = m_drag.initialState.cx;
    float anchorY = m_drag.initialState.cy;
    if (!m_altHeld) {
      const float anchorLocalX = -signs.x * halfW0;
      const float anchorLocalY = -signs.y * halfH0;
      anchorX = m_drag.initialState.cx + cosR * anchorLocalX - sinR * anchorLocalY;
      anchorY = m_drag.initialState.cy + sinR * anchorLocalX + cosR * anchorLocalY;
    }

    // Dragged corner relative to the anchor, projected into the widget's un-rotated frame.
    const float wx = m_currentEventSceneX - anchorX;
    const float wy = m_currentEventSceneY - anchorY;
    const float localX = wx * std::cos(-rot) - wy * std::sin(-rot);
    const float localY = wx * std::sin(-rot) + wy * std::cos(-rot);

    // Corner-anchored resize uses the signed projection along the corner's direction so dragging
    // past the fixed anchor clamps to the minimum size instead of flipping sign and growing again.
    // Center-anchored (Alt) resize grows symmetrically in either direction, so it uses the magnitude.
    float boxW = m_altHeld ? std::abs(localX) * 2.0f : signs.x * localX;
    float boxH = m_altHeld ? std::abs(localY) * 2.0f : signs.y * localY;

    const float cell = static_cast<float>(std::max(1, m_snapshot.grid.cellSize));
    boxW = std::max(cell, boxW);
    boxH = std::max(cell, boxH);
    if (shouldSnap()) {
      boxW = std::max(cell, std::round(boxW / cell) * cell);
      boxH = std::max(cell, std::round(boxH / cell) * cell);
    }

    if (lockscreen_login_box::isLoginBoxWidget(*state)) {
      float screenWidth = 1920.0f;
      if (OverlaySurface* surface = findSurfaceForWidget(m_drag.widgetId);
          surface != nullptr && surface->surface != nullptr) {
        screenWidth = static_cast<float>(surface->surface->width());
      }
      lockscreen_login_box::clampPanelSize(screenWidth, boxW, boxH);
    }

    if (!m_altHeld) {
      const float centerLocalX = signs.x * boxW * 0.5f;
      const float centerLocalY = signs.y * boxH * 0.5f;
      state->cx = anchorX + cosR * centerLocalX - sinR * centerLocalY;
      state->cy = anchorY + sinR * centerLocalX + cosR * centerLocalY;
    }
    state->boxWidth = boxW;
    state->boxHeight = boxH;
  }

  float clampWidth = m_drag.intrinsicWidth;
  float clampHeight = m_drag.intrinsicHeight;
  if (m_drag.mode == DragMode::Scale) {
    clampWidth = std::max(1.0f, state->boxWidth);
    clampHeight = std::max(1.0f, state->boxHeight);
  }

  if (m_wayland != nullptr) {
    desktop_widgets::clampStateToOutput(*m_wayland, *state, clampWidth, clampHeight);
  }

  if (outputAssignmentChanged) {
    requestLayout();
    return;
  }

  // For a resize, preview the new box as a cheap GPU scale of the dragged widget (its content is
  // re-fitted crisply once, on release, in finishDrag()); otherwise just reposition the views.
  if (m_drag.mode == DragMode::Scale) {
    applyScaleDragPreview(*state);
  } else {
    updateViewTransforms();
  }

  OverlaySurface* dragSurfaceForRedraw = findSurfaceForWidget(m_drag.widgetId);
  if (dragSurfaceForRedraw != nullptr && dragSurfaceForRedraw->surface != nullptr) {
    dragSurfaceForRedraw->surface->requestRedraw();
  }
  for (const auto& [groupId, initialState] : m_drag.groupInitialStates) {
    (void)initialState;
    if (OverlaySurface* groupSurface = findSurfaceForWidget(groupId);
        groupSurface != nullptr && groupSurface->surface != nullptr && groupSurface != dragSurfaceForRedraw) {
      groupSurface->surface->requestRedraw();
    }
  }
}

void DesktopWidgetsEditor::finishDrag() {
  const DragMode mode = m_drag.mode;
  const std::string widgetId = m_drag.widgetId;
  const bool rebuildOnFinish = m_drag.rebuildOnFinish;
  m_drag = {};

  if (mode == DragMode::Scale && !widgetId.empty()) {
    if (m_renderContext != nullptr) {
      if (OverlaySurface* surface = findSurfaceForWidget(widgetId); surface != nullptr && surface->surface != nullptr) {
        m_renderContext->makeCurrent(surface->surface->renderTarget());
      }
    }
    if (DesktopWidgetState* state = findWidgetState(widgetId); state != nullptr) {
      if (EditorWidgetView* view = findView(widgetId); view != nullptr) {
        applyViewState(*view, *state, true);
      }
    }
    updateViewTransforms();
    if (OverlaySurface* surface = findSurfaceForWidget(widgetId); surface != nullptr && surface->surface != nullptr) {
      surface->surface->requestRedraw();
    }
    return;
  }

  if (rebuildOnFinish) {
    requestLayout();
  }
}

bool DesktopWidgetsEditor::onPointerEvent(const PointerEvent& event) {
  if (!m_open) {
    return false;
  }

  wl_surface* eventSurface = event.surface;
  if (eventSurface == nullptr && m_wayland != nullptr) {
    // wl_pointer.motion does not carry the surface; reuse the seat's current
    // pointer focus so drags continue after the initial press.
    eventSurface = m_wayland->lastPointerSurface();
  }

  for (auto& s : m_surfaces) {
    if (s->selectPopup != nullptr && s->selectPopup->isSelectDropdownOpen()) {
      if (s->selectPopup->onPointerEvent(event)) {
        return true;
      }
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        s->selectPopup->closeSelectDropdown();
        return true;
      }
    }
  }

  OverlaySurface* surface = findSurface(eventSurface);
  if (surface == nullptr) {
    return false;
  }

  m_currentEventOutputName = surface->outputName;
  m_currentEventSceneX = static_cast<float>(event.sx);
  m_currentEventSceneY = static_cast<float>(event.sy);

  switch (event.type) {
  case PointerEvent::Type::Enter:
    surface->pointerInside = true;
    surface->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    surface->pointerInside = false;
    surface->inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    surface->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // An active drag tracks the pointer everywhere, not just while it stays over the handle's input
    // area — otherwise dragging a scale handle outward (growing the widget) stops updating once the
    // pointer leaves the handle.
    if (m_drag.mode != DragMode::None) {
      updateDrag();
    }
    break;
  case PointerEvent::Type::Button:
    surface->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed
    );
    if (!event.pressed && m_drag.mode != DragMode::None && event.button == BTN_LEFT) {
      finishDrag();
    }
    break;
  case PointerEvent::Type::Axis:
    surface->inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (surface->sceneRoot != nullptr && (surface->sceneRoot->layoutDirty() || surface->sceneRoot->paintDirty())) {
    if (surface->sceneRoot->layoutDirty()) {
      surface->surface->requestLayout();
    } else {
      surface->surface->requestRedraw();
    }
  }

  return true;
}

void DesktopWidgetsEditor::onKeyboardEvent(const KeyboardEvent& event) {
  if (!m_open) {
    return;
  }

  if (!event.preedit) {
    const bool shiftFromMask = (event.modifiers & KeyMod::Shift) != 0;
    if (event.sym == XKB_KEY_Shift_L) {
      m_leftShiftHeld = event.pressed;
    } else if (event.sym == XKB_KEY_Shift_R) {
      m_rightShiftHeld = event.pressed;
    }

    // wl_keyboard.modifiers is delivered separately from key up/down, so the
    // modifier bit on a Shift key event can lag by one transition. Track the
    // physical Shift keys directly and only fall back to the mask when we have
    // no concrete Shift key state yet.
    m_shiftHeld = m_leftShiftHeld || m_rightShiftHeld;
    if (!m_shiftHeld && event.sym != XKB_KEY_Shift_L && event.sym != XKB_KEY_Shift_R) {
      m_shiftHeld = shiftFromMask;
    }

    const bool ctrlFromMask = (event.modifiers & KeyMod::Ctrl) != 0;
    if (event.sym == XKB_KEY_Control_L) {
      m_leftCtrlHeld = event.pressed;
    } else if (event.sym == XKB_KEY_Control_R) {
      m_rightCtrlHeld = event.pressed;
    }
    m_ctrlHeld = m_leftCtrlHeld || m_rightCtrlHeld;
    if (!m_ctrlHeld && event.sym != XKB_KEY_Control_L && event.sym != XKB_KEY_Control_R) {
      m_ctrlHeld = ctrlFromMask;
    }

    const bool altFromMask = (event.modifiers & KeyMod::Alt) != 0;
    if (event.sym == XKB_KEY_Alt_L) {
      m_leftAltHeld = event.pressed;
    } else if (event.sym == XKB_KEY_Alt_R) {
      m_rightAltHeld = event.pressed;
    }
    m_altHeld = m_leftAltHeld || m_rightAltHeld;
    if (!m_altHeld && event.sym != XKB_KEY_Alt_L && event.sym != XKB_KEY_Alt_R) {
      m_altHeld = altFromMask;
    }
  }

  for (auto& surface : m_surfaces) {
    if (surface->selectPopup != nullptr && surface->selectPopup->isSelectDropdownOpen()) {
      surface->selectPopup->onKeyboardEvent(event);
      return;
    }
  }

  InputArea* focused = nullptr;
  for (auto& surface : m_surfaces) {
    if (surface->pointerInside) {
      surface->inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
      focused = surface->inputDispatcher.focusedArea();
      break;
    }
  }

  if (!event.pressed || event.preedit) {
    return;
  }

  if (focused != nullptr) {
    if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      for (auto& surface : m_surfaces) {
        surface->inputDispatcher.setFocus(nullptr);
      }
    }
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    requestExit();
    return;
  }

  if (KeySymbol::isBackspaceOrDelete(event.sym)) {
    removeSelectedWidget();
    return;
  }

  if ((event.modifiers & KeyMod::Ctrl) != 0) {
    if (event.sym == XKB_KEY_c
        || event.sym == XKB_KEY_C
        || event.utf32 == static_cast<std::uint32_t>('c')
        || event.utf32 == static_cast<std::uint32_t>('C')) {
      copySelectedWidgets();
      return;
    }
    if (event.sym == XKB_KEY_v
        || event.sym == XKB_KEY_V
        || event.utf32 == static_cast<std::uint32_t>('v')
        || event.utf32 == static_cast<std::uint32_t>('V')) {
      pasteWidgets();
      return;
    }
  }

  if (event.sym == XKB_KEY_g
      || event.sym == XKB_KEY_G
      || event.utf32 == static_cast<std::uint32_t>('g')
      || event.utf32 == static_cast<std::uint32_t>('G')) {
    m_snapshot.grid.visible = !m_snapshot.grid.visible;
    requestLayout();
  }
}

void DesktopWidgetsEditor::onOutputChange() {
  if (!m_open) {
    return;
  }
  syncSurfaces();
  requestLayout();
}

void DesktopWidgetsEditor::onSecondTick() {
  if (!m_open || m_drag.mode != DragMode::None) {
    return;
  }

  const bool minuteBoundary = formatLocalTime("{:%S}") == "00";

  for (auto& surface : m_surfaces) {
    if (surface->surface == nullptr) {
      continue;
    }
    const bool wantsSecondTicks = std::any_of(surface->views.begin(), surface->views.end(), [](const auto& entry) {
      return entry.second.widget != nullptr && entry.second.widget->wantsSecondTicks();
    });
    if (minuteBoundary) {
      surface->surface->requestUpdate();
    } else if (wantsSecondTicks) {
      surface->surface->requestUpdateOnly();
    }
  }
}

void DesktopWidgetsEditor::releaseWallpaperPreview(OverlaySurface& surface) {
  if (surface.wallpaperPreviewTexture.id == 0) {
    surface.wallpaperPreviewLoadedPath.clear();
    return;
  }

  const std::string& releasePath = surface.wallpaperPreviewLoadedPath;
  if (m_textureCache != nullptr && m_textureCache->shared()) {
    if (!releasePath.empty()) {
      m_textureCache->release(surface.wallpaperPreviewTexture, releasePath);
    }
  } else if (m_renderContext != nullptr) {
    m_renderContext->backend().makeCurrentNoSurface();
    m_renderContext->textureManager().unload(surface.wallpaperPreviewTexture);
  }
  surface.wallpaperPreviewTexture = {};
  surface.wallpaperPreviewLoadedPath.clear();
}

void DesktopWidgetsEditor::updateWallpaperPreview(OverlaySurface& surface) {
  if (!surface.wallpaperPreviewActive
      || surface.wallpaperPreview == nullptr
      || m_config == nullptr
      || m_renderContext == nullptr
      || surface.surface == nullptr) {
    return;
  }

  const auto width = static_cast<float>(surface.surface->width());
  const auto height = static_cast<float>(surface.surface->height());
  surface.wallpaperPreview->setPosition(0.0f, 0.0f);
  surface.wallpaperPreview->setSize(width, height);

  const std::string& path = surface.wallpaperPreviewPath;
  if (path.empty()) {
    return;
  }

  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  if (parseColorWallpaperPath(path, color)) {
    if (surface.wallpaperPreviewTexture.id != 0) {
      releaseWallpaperPreview(surface);
    }
    surface.wallpaperPreview->setSources(
        WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.0f,
        0.0f, 0.0f
    );
    surface.wallpaperPreview->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    return;
  }

  const bool needsReload = surface.wallpaperPreviewTexture.id == 0 || surface.wallpaperPreviewLoadedPath != path;
  TextureHandle texture = surface.wallpaperPreviewTexture;
  if (needsReload) {
    if (m_textureCache != nullptr) {
      texture = m_textureCache->acquire(path);
      if (texture.id == 0 && !m_textureCache->shared()) {
        m_renderContext->backend().makeCurrentNoSurface();
        texture = m_renderContext->textureManager().loadFromFile(path, 0, true);
      }
    } else {
      m_renderContext->backend().makeCurrentNoSurface();
      texture = m_renderContext->textureManager().loadFromFile(path, 0, true);
    }
  }

  if (texture.id == 0) {
    return;
  }

  if (needsReload && surface.wallpaperPreviewTexture.id != 0 && surface.wallpaperPreviewLoadedPath != path) {
    releaseWallpaperPreview(surface);
  }
  surface.wallpaperPreviewTexture = texture;
  surface.wallpaperPreviewLoadedPath = path;
  surface.wallpaperPreview->setTextures(
      texture.id, {}, static_cast<float>(texture.width), static_cast<float>(texture.height), 0.0f, 0.0f
  );
  surface.wallpaperPreview->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
}

void DesktopWidgetsEditor::requestLayout() {
  for (auto& surface : m_surfaces) {
    if (surface->surface != nullptr) {
      surface->sceneRebuildRequested = true;
      surface->surface->requestLayout();
    }
  }
}

void DesktopWidgetsEditor::requestUpdate() {
  for (auto& surface : m_surfaces) {
    if (surface->surface != nullptr) {
      surface->surface->requestUpdateOnly();
    }
  }
}

void DesktopWidgetsEditor::requestRedraw() {
  for (auto& surface : m_surfaces) {
    if (surface->surface != nullptr) {
      surface->surface->requestRedraw();
    }
  }
}
