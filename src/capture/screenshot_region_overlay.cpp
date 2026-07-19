#include "capture/screenshot_region_overlay.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>

namespace capture {
  namespace {

    constexpr Logger kLog("screenshot-region");
    constexpr float kDimensionFontSize = 14.0f;
    constexpr float kDimensionCursorOffsetX = 12.0f;
    constexpr float kDimensionCursorOffsetY = 14.0f;
    constexpr float kDimensionPaddingX = 6.0f;
    constexpr float kDimensionPaddingY = 4.0f;
    constexpr float kSelectionBorderWidth = 2.0f;
    constexpr float kDimOpacity = 0.65f;

    [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
      for (const auto& entry : wayland.outputs()) {
        if (entry.output == output) {
          return &entry;
        }
      }
      return nullptr;
    }

    [[nodiscard]] LayerShellKeyboard overlayKeyboardMode() { return LayerShellKeyboard::Exclusive; }

    [[nodiscard]] const ScreencopyImage*
    frozenImageForOutput(const std::vector<FrozenScreenshot>& screenshots, wl_output* output) {
      for (const auto& entry : screenshots) {
        if (entry.output == output) {
          return &entry.image;
        }
      }
      return nullptr;
    }

    [[nodiscard]] wl_output* outputAtGlobalPoint(const WaylandConnection& wayland, double globalX, double globalY) {
      for (const auto& out : wayland.outputs()) {
        if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
          continue;
        }
        if (globalX >= static_cast<double>(out.logicalX)
            && globalX < static_cast<double>(out.logicalX + out.logicalWidth)
            && globalY >= static_cast<double>(out.logicalY)
            && globalY < static_cast<double>(out.logicalY + out.logicalHeight)) {
          return out.output;
        }
      }
      return nullptr;
    }

    [[nodiscard]] std::string outputPickerLabel(const WaylandOutput& output) {
      if (!output.connectorName.empty()) {
        return output.connectorName;
      }
      if (!output.description.empty()) {
        return output.description;
      }
      return "Display";
    }

    std::unique_ptr<Flex>
    buildFullscreenPickerBar(const WaylandConnection& wayland, std::function<void(wl_output*)> onPick) {
      auto bar = std::make_unique<Flex>();
      bar->setDirection(FlexDirection::Horizontal);
      bar->setJustify(FlexJustify::Center);
      bar->setAlign(FlexAlign::Center);
      bar->setGap(Style::spaceSm);
      bar->setPadding(Style::spaceSm, Style::spaceMd, Style::spaceSm, Style::spaceMd);
      bar->setCardStyle(1.0f, 0.94f, true);

      auto hint = std::make_unique<Label>();
      hint->setText(i18n::tr("bar.screenshot.choose-display"));
      hint->setFontSize(Style::fontSizeCaption);
      hint->setColor(colorForRole(ColorRole::OnSurface));
      bar->addChild(std::move(hint));

      for (const auto& out : wayland.outputs()) {
        if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
          continue;
        }
        auto button = std::make_unique<Button>();
        button->setText(outputPickerLabel(out));
        button->setVariant(ButtonVariant::Outline);
        button->setOnClick([onPick, output = out.output]() { onPick(output); });
        bar->addChild(std::move(button));
      }

      return bar;
    }

    std::unique_ptr<Flex> buildConfirmHintBar(Label*& hintOut) {
      auto bar = std::make_unique<Flex>();
      bar->setDirection(FlexDirection::Horizontal);
      bar->setJustify(FlexJustify::Center);
      bar->setAlign(FlexAlign::Center);
      bar->setPadding(Style::spaceSm, Style::spaceMd, Style::spaceSm, Style::spaceMd);
      bar->setCardStyle(1.0f, 0.94f, true);
      bar->setVisible(false);

      auto hint = std::make_unique<Label>();
      hint->setFontSize(Style::fontSizeCaption);
      hint->setColor(colorForRole(ColorRole::OnSurface));
      hintOut = hint.get();
      bar->addChild(std::move(hint));
      return bar;
    }

  } // namespace

  struct ScreenshotRegionOverlay::Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputArea* input = nullptr;
    Image* backdrop = nullptr;
    Box* dimTop = nullptr;
    Box* dimBottom = nullptr;
    Box* dimLeft = nullptr;
    Box* dimRight = nullptr;
    Box* selection = nullptr;
    Box* dimensionsBadge = nullptr;
    Label* dimensionsLabel = nullptr;
    Flex* confirmHint = nullptr;
    Label* confirmHintLabel = nullptr;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
  };

  ScreenshotRegionOverlay::ScreenshotRegionOverlay() = default;

  ScreenshotRegionOverlay::~ScreenshotRegionOverlay() = default;

  void ScreenshotRegionOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void ScreenshotRegionOverlay::setCompleteCallback(CompleteCallback callback) { m_onComplete = std::move(callback); }

  void ScreenshotRegionOverlay::setFailureCallback(FailureCallback callback) { m_onFailure = std::move(callback); }

  void ScreenshotRegionOverlay::setFrozenScreenshots(std::vector<FrozenScreenshot> screenshots) {
    m_frozenScreenshots = std::move(screenshots);
  }

  std::vector<FrozenScreenshot> ScreenshotRegionOverlay::takeFrozenScreenshots() {
    std::vector<FrozenScreenshot> screenshots = std::move(m_frozenScreenshots);
    m_frozenScreenshots.clear();
    return screenshots;
  }

  void ScreenshotRegionOverlay::begin(bool freezeScreen, bool fullscreenPick, bool confirmRegion) {
    if (m_wayland == nullptr || m_renderContext == nullptr) {
      return;
    }
    destroySurfaces();
    m_freezeScreen = freezeScreen;
    m_fullscreenPick = fullscreenPick;
    m_confirmRegion = confirmRegion && !fullscreenPick;
    m_confirming = false;
    m_active = true;
    m_dragging = false;
    ensureSurfaces();
    for (auto& inst : m_instances) {
      if (inst->surface != nullptr) {
        inst->surface->requestLayout();
        inst->surface->requestRedraw();
      }
    }
  }

  void ScreenshotRegionOverlay::cancel() {
    m_active = false;
    m_dragging = false;
    m_confirming = false;
    m_freezeScreen = false;
    m_fullscreenPick = false;
    m_confirmRegion = false;
    m_frozenScreenshots.clear();
    destroySurfaces();
  }

  void ScreenshotRegionOverlay::cancelSelection() {
    if (!m_active) {
      return;
    }
    DeferredCall::callLater([this]() {
      if (!m_active) {
        return;
      }
      cancel();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
    });
  }

  void ScreenshotRegionOverlay::onOutputChange() {
    if (!m_active) {
      return;
    }
    if (!m_instances.empty() && !surfacesMatchOutputs()) {
      destroySurfaces();
      ensureSurfaces();
    }
  }

  bool ScreenshotRegionOverlay::surfacesMatchOutputs() const {
    if (m_wayland == nullptr) {
      return m_instances.empty();
    }
    const auto& outputs = m_wayland->outputs();
    if (m_instances.size() != outputs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (m_instances[i] == nullptr || m_instances[i]->output != outputs[i].output) {
        return false;
      }
    }
    return true;
  }

  void ScreenshotRegionOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr || !m_active) {
      return;
    }
    if (!m_instances.empty() && surfacesMatchOutputs()) {
      return;
    }
    destroySurfaces();

    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }

      auto inst = std::make_unique<Instance>();
      inst->output = output.output;

      auto config = LayerSurfaceConfig{
          .nameSpace = "noctalia-screenshot-region",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = 0,
          .exclusiveZone = -1,
          .keyboard = overlayKeyboardMode(),
          .defaultWidth = static_cast<std::uint32_t>(output.logicalWidth),
          .defaultHeight = static_cast<std::uint32_t>(output.logicalHeight),
      };

      inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
      auto* instPtr = inst.get();
      inst->surface->setRenderContext(m_renderContext);
      inst->surface->setAnimationManager(&inst->animations);
      inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
        instPtr->surface->requestLayout();
      });
      inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
        prepareFrame(*instPtr, needsUpdate, needsLayout);
      });

      if (!inst->surface->initialize(output.output)) {
        kLog.warn("failed to initialize screenshot region overlay on {}", output.connectorName);
        continue;
      }

      m_instances.push_back(std::move(inst));
    }
  }

  void ScreenshotRegionOverlay::destroySurfaces() {
    for (auto& inst : m_instances) {
      if (inst != nullptr) {
        if (inst->backdrop != nullptr && m_renderContext != nullptr) {
          inst->backdrop->clear(*m_renderContext);
        }
        inst->inputDispatcher.setSceneRoot(nullptr);
        inst->animations.cancelAll();
      }
    }
    m_instances.clear();
    m_dragging = false;
  }

  void ScreenshotRegionOverlay::prepareFrame(Instance& inst, bool /*needsUpdate*/, bool /*needsLayout*/) {
    if (!m_active || m_renderContext == nullptr || inst.surface == nullptr) {
      return;
    }

    const auto width = inst.surface->width();
    const auto height = inst.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    if (!m_renderContext->makeCurrent(inst.surface->renderTarget())) {
      abortWithError(i18n::tr("bar.screenshot.overlay-alloc-failed"));
      return;
    }

    const bool needsSceneBuild = inst.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
    if (!needsSceneBuild) {
      updateSelectionVisuals();
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);

    inst.sceneRoot = std::make_unique<Node>();
    inst.sceneRoot->setSize(w, h);

    auto input = std::make_unique<InputArea>();
    input->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    input->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);

    if (m_fullscreenPick) {
      input->setOnClick([this, surfaceOutput = inst.output](const InputArea::PointerData& data) {
        if (data.pressed || data.button != BTN_LEFT) {
          return;
        }
        const auto* surfaceOut = findOutput(*m_wayland, surfaceOutput);
        if (surfaceOut == nullptr) {
          return;
        }
        const double globalX = static_cast<double>(surfaceOut->logicalX) + static_cast<double>(data.localX);
        const double globalY = static_cast<double>(surfaceOut->logicalY) + static_cast<double>(data.localY);
        wl_output* picked = outputAtGlobalPoint(*m_wayland, globalX, globalY);
        if (picked == nullptr) {
          picked = surfaceOutput;
        }
        DeferredCall::callLater([this, picked]() { completeFullscreenPick(picked); });
      });
    } else {
      input->setOnPress([this, output = inst.output](const InputArea::PointerData& data) {
        if (data.button != BTN_LEFT) {
          return;
        }
        if (!data.pressed) {
          if (!m_dragging) {
            return;
          }
          m_dragging = false;
          DeferredCall::callLater([this]() { completeSelection(); });
          return;
        }
        if (m_confirming) {
          m_confirming = false;
        }
        const auto* out = findOutput(*m_wayland, output);
        if (out == nullptr) {
          return;
        }
        m_dragging = true;
        m_startGlobalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
        m_startGlobalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);
        m_currentGlobalX = m_startGlobalX;
        m_currentGlobalY = m_startGlobalY;
        updateSelectionVisuals();
        for (auto& instance : m_instances) {
          if (instance->surface != nullptr) {
            instance->surface->requestRedraw();
          }
        }
      });

      input->setOnMotion([this, output = inst.output](const InputArea::PointerData& data) {
        if (!m_dragging) {
          return;
        }
        const auto* out = findOutput(*m_wayland, output);
        if (out == nullptr) {
          return;
        }
        m_currentGlobalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
        m_currentGlobalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);
        updateSelectionVisuals();
        for (auto& instance : m_instances) {
          if (instance->surface != nullptr) {
            instance->surface->requestRedraw();
          }
        }
      });
    }

    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (!key.pressed) {
        return;
      }
      if (m_confirming) {
        if ((key.modifiers & KeyMod::Ctrl) != 0) {
          if (key.sym == XKB_KEY_c || key.sym == XKB_KEY_C) {
            DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::ForceClipboard); });
            return;
          }
          if (key.sym == XKB_KEY_s || key.sym == XKB_KEY_S) {
            DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::ForceSave); });
            return;
          }
        }
        if (KeySymbol::isEnterOrSpace(key.sym)) {
          DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::None); });
          return;
        }
      }
      if (KeybindMatcher::matches(KeybindAction::Cancel, key.sym, key.modifiers)) {
        cancelSelection();
      }
    });
    input->setFocusable(true);

    const auto* frozen = m_freezeScreen ? frozenImageForOutput(m_frozenScreenshots, inst.output) : nullptr;
    if (frozen != nullptr) {
      auto backdrop = std::make_unique<Image>();
      backdrop->setFit(ImageFit::Stretch);
      backdrop->setPosition(0.0f, 0.0f);
      backdrop->setSize(w, h);
      if (!backdrop->setSourceRaw(
              *m_renderContext, frozen->rgba.data(), frozen->rgba.size(), frozen->width, frozen->height,
              frozen->width * 4, PixmapFormat::RGBA, false
          )) {
        kLog.warn("failed to upload frozen screenshot backdrop");
      }
      inst.backdrop = static_cast<Image*>(input->addChild(std::move(backdrop)));
    }

    auto makeDimStrip = [&]() {
      auto strip = std::make_unique<Box>();
      strip->setFill(fixedColorSpec(rgba(0.0f, 0.0f, 0.0f, 1.0f)));
      strip->setOpacity(kDimOpacity);
      strip->setPosition(0.0f, 0.0f);
      strip->setSize(0.0f, 0.0f);
      return static_cast<Box*>(input->addChild(std::move(strip)));
    };
    inst.dimTop = makeDimStrip();
    inst.dimBottom = makeDimStrip();
    inst.dimLeft = makeDimStrip();
    inst.dimRight = makeDimStrip();

    Color border = colorForRole(ColorRole::Primary);
    border.a = 1.0f;

    auto selection = std::make_unique<Box>();
    selection->setBorder(fixedColorSpec(border), kSelectionBorderWidth);
    selection->setVisible(false);

    auto dimensionsBadge = std::make_unique<Box>();
    Color badgeFill = colorForRole(ColorRole::Surface);
    badgeFill.a = 0.94f;
    dimensionsBadge->setFill(fixedColorSpec(badgeFill));
    dimensionsBadge->setBorder(fixedColorSpec(border), 1.0f);
    dimensionsBadge->setRadius(Style::radiusSm);
    dimensionsBadge->setVisible(false);

    auto dimensionsLabel = std::make_unique<Label>();
    dimensionsLabel->setFontSize(kDimensionFontSize);
    dimensionsLabel->setFontWeight(FontWeight::Bold);
    dimensionsLabel->setColor(border);

    if (!m_fullscreenPick) {
      inst.dimensionsLabel = static_cast<Label*>(dimensionsBadge->addChild(std::move(dimensionsLabel)));
      inst.selection = static_cast<Box*>(input->addChild(std::move(selection)));
      inst.dimensionsBadge = static_cast<Box*>(input->addChild(std::move(dimensionsBadge)));
    }
    inst.input = input.get();
    inst.sceneRoot->addChild(std::move(input));

    if (m_fullscreenPick) {
      auto pickerBar = buildFullscreenPickerBar(*m_wayland, [this](wl_output* output) {
        DeferredCall::callLater([this, output]() { completeFullscreenPick(output); });
      });
      Flex* pickerBarPtr = pickerBar.get();
      inst.sceneRoot->addChild(std::move(pickerBar));
      pickerBarPtr->layout(*m_renderContext);
      pickerBarPtr->setPosition((w - pickerBarPtr->width()) * 0.5f, Style::spaceMd);
    } else if (m_confirmRegion) {
      auto hintBar = buildConfirmHintBar(inst.confirmHintLabel);
      inst.confirmHint = hintBar.get();
      inst.sceneRoot->addChild(std::move(hintBar));
    }

    inst.surface->setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      if (m_wayland != nullptr) {
        m_wayland->setCursorShape(serial, shape);
      }
    });
    if (inst.input != nullptr) {
      inst.inputDispatcher.setFocus(inst.input);
    }

    updateSelectionVisuals();
  }

  bool ScreenshotRegionOverlay::onPointerEvent(const PointerEvent& event) {
    if (!m_active) {
      return false;
    }

    Instance* target = nullptr;
    if (event.surface != nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == event.surface) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->pointerInside) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      return false;
    }

    const bool onTarget =
        event.surface != nullptr && target->surface != nullptr && event.surface == target->surface->wlSurface();

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onTarget) {
        target->pointerInside = true;
        target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      return onTarget;
    case PointerEvent::Type::Leave:
      if (onTarget || target->pointerInside) {
        target->pointerInside = false;
        target->inputDispatcher.pointerLeave();
      }
      return onTarget || target->pointerInside;
    case PointerEvent::Type::Motion:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (onTarget || target->pointerInside) {
        target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      return false;
    case PointerEvent::Type::Button: {
      if (onTarget) {
        target->pointerInside = true;
      }
      if (!onTarget && !target->pointerInside) {
        return false;
      }
      const bool pressed = event.pressed;
      return target->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
    }
    case PointerEvent::Type::Axis:
      if (onTarget || target->pointerInside) {
        return target->inputDispatcher.pointerAxis(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
            event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      return false;
    }

    return false;
  }

  bool ScreenshotRegionOverlay::onKeyboardEvent(const KeyboardEvent& event) {
    if (!m_active || !event.pressed || m_wayland == nullptr) {
      return false;
    }

    wl_surface* const kbSurface = m_wayland->lastKeyboardSurface();
    bool onOverlay = false;
    for (const auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == kbSurface) {
        onOverlay = true;
        break;
      }
    }
    if (!onOverlay) {
      return false;
    }

    if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      if (m_confirming) {
        if ((event.modifiers & KeyMod::Ctrl) != 0) {
          if (event.sym == XKB_KEY_c || event.sym == XKB_KEY_C) {
            confirmPendingSelection(ConfirmAction::ForceClipboard);
            return true;
          }
          if (event.sym == XKB_KEY_s || event.sym == XKB_KEY_S) {
            confirmPendingSelection(ConfirmAction::ForceSave);
            return true;
          }
        }
        if (KeySymbol::isEnterOrSpace(event.sym)) {
          confirmPendingSelection(ConfirmAction::None);
          return true;
        }
      }
      return false;
    }
    cancelSelection();
    return true;
  }

  void ScreenshotRegionOverlay::abortWithError(const std::string& message) {
    if (!m_active) {
      return;
    }
    m_active = false;
    kLog.warn("aborting screenshot region overlay: {}", message);
    FailureCallback onFailure = m_onFailure;
    DeferredCall::callLater([this, onFailure, message]() {
      cancel();
      if (onFailure) {
        onFailure(message);
      }
    });
  }

  void ScreenshotRegionOverlay::updateSelectionVisuals() {
    const auto layoutDimFrame = [](Instance& inst, float surfaceW, float surfaceH, float hx0, float hy0, float hx1,
                                   float hy1) {
      hx0 = std::clamp(hx0, 0.0f, surfaceW);
      hx1 = std::clamp(hx1, 0.0f, surfaceW);
      hy0 = std::clamp(hy0, 0.0f, surfaceH);
      hy1 = std::clamp(hy1, 0.0f, surfaceH);
      if (hx1 < hx0 || hy1 < hy0) {
        hx0 = hy0 = hx1 = hy1 = 0.0f;
      }
      if (inst.dimTop != nullptr) {
        inst.dimTop->setPosition(0.0f, 0.0f);
        inst.dimTop->setSize(surfaceW, hy0);
      }
      if (inst.dimBottom != nullptr) {
        inst.dimBottom->setPosition(0.0f, hy1);
        inst.dimBottom->setSize(surfaceW, surfaceH - hy1);
      }
      if (inst.dimLeft != nullptr) {
        inst.dimLeft->setPosition(0.0f, hy0);
        inst.dimLeft->setSize(hx0, hy1 - hy0);
      }
      if (inst.dimRight != nullptr) {
        inst.dimRight->setPosition(hx1, hy0);
        inst.dimRight->setSize(surfaceW - hx1, hy1 - hy0);
      }
    };

    if (!m_dragging && !m_confirming) {
      for (auto& inst : m_instances) {
        if (inst->surface != nullptr) {
          layoutDimFrame(
              *inst, static_cast<float>(inst->surface->width()), static_cast<float>(inst->surface->height()), 0.0f,
              0.0f, 0.0f, 0.0f
          );
        }
        if (inst->selection != nullptr) {
          inst->selection->setVisible(false);
        }
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        if (inst->confirmHint != nullptr) {
          inst->confirmHint->setVisible(false);
        }
      }
      return;
    }

    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int selectionWidth = globalX1 - globalX0;
    const int selectionHeight = globalY1 - globalY0;
    const int cursorGlobalX = static_cast<int>(std::lround(m_currentGlobalX));
    const int cursorGlobalY = static_cast<int>(std::lround(m_currentGlobalY));

    char dimensionText[32];
    std::snprintf(dimensionText, sizeof(dimensionText), "%dx%d", selectionWidth, selectionHeight);

    for (auto& inst : m_instances) {
      if (inst->selection == nullptr || inst->surface == nullptr) {
        continue;
      }
      const auto surfaceW = static_cast<float>(inst->surface->width());
      const auto surfaceH = static_cast<float>(inst->surface->height());
      const auto* out = findOutput(*m_wayland, inst->output);
      if (out == nullptr) {
        layoutDimFrame(*inst, surfaceW, surfaceH, 0.0f, 0.0f, 0.0f, 0.0f);
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      const int outLeft = out->logicalX;
      const int outTop = out->logicalY;
      const int outRight = out->logicalX + out->logicalWidth;
      const int outBottom = out->logicalY + out->logicalHeight;

      const int ix0 = std::max(globalX0, outLeft);
      const int iy0 = std::max(globalY0, outTop);
      const int ix1 = std::min(globalX1, outRight);
      const int iy1 = std::min(globalY1, outBottom);
      if (ix1 <= ix0 || iy1 <= iy0) {
        layoutDimFrame(*inst, surfaceW, surfaceH, 0.0f, 0.0f, 0.0f, 0.0f);
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      const auto holeX0 = static_cast<float>(ix0 - outLeft);
      const auto holeY0 = static_cast<float>(iy0 - outTop);
      const auto holeX1 = static_cast<float>(ix1 - outLeft);
      const auto holeY1 = static_cast<float>(iy1 - outTop);
      layoutDimFrame(*inst, surfaceW, surfaceH, holeX0, holeY0, holeX1, holeY1);

      inst->selection->setVisible(true);
      inst->selection->setPosition(holeX0 - kSelectionBorderWidth, holeY0 - kSelectionBorderWidth);
      inst->selection->setSize(
          (holeX1 - holeX0) + (kSelectionBorderWidth * 2.0f), (holeY1 - holeY0) + (kSelectionBorderWidth * 2.0f)
      );

      if (inst->dimensionsBadge != nullptr
          && inst->dimensionsLabel != nullptr
          && m_renderContext != nullptr
          && m_dragging) {
        const bool cursorOnOutput = cursorGlobalX >= outLeft
            && cursorGlobalX < outRight
            && cursorGlobalY >= outTop
            && cursorGlobalY < outBottom;
        if (cursorOnOutput) {
          inst->dimensionsLabel->setText(dimensionText);
          inst->dimensionsLabel->measure(*m_renderContext);
          const float badgeWidth = inst->dimensionsLabel->width() + (kDimensionPaddingX * 2.0f);
          const float badgeHeight = inst->dimensionsLabel->height() + (kDimensionPaddingY * 2.0f);
          inst->dimensionsBadge->setSize(badgeWidth, badgeHeight);

          float badgeX = static_cast<float>(cursorGlobalX - outLeft) + kDimensionCursorOffsetX;
          float badgeY = static_cast<float>(cursorGlobalY - outTop) + kDimensionCursorOffsetY;
          const float maxX = std::max(0.0f, surfaceW - badgeWidth);
          const float maxY = std::max(0.0f, surfaceH - badgeHeight);
          badgeX = std::clamp(badgeX, 0.0f, maxX);
          badgeY = std::clamp(badgeY, 0.0f, maxY);

          inst->dimensionsBadge->setPosition(badgeX, badgeY);
          inst->dimensionsLabel->setPosition(kDimensionPaddingX, kDimensionPaddingY);
          inst->dimensionsBadge->setVisible(true);
        } else {
          inst->dimensionsBadge->setVisible(false);
        }
      }
    }

    if (m_renderContext != nullptr) {
      for (auto& inst : m_instances) {
        if (inst->confirmHint == nullptr || inst->surface == nullptr) {
          continue;
        }
        inst->confirmHint->setVisible(m_confirming);
        if (!m_confirming) {
          continue;
        }
        if (inst->confirmHintLabel != nullptr) {
          inst->confirmHintLabel->setText(i18n::tr("bar.screenshot.confirm-region"));
        }
        const auto surfaceW = static_cast<float>(inst->surface->width());
        const auto surfaceH = static_cast<float>(inst->surface->height());
        inst->confirmHint->layout(*m_renderContext);
        const float y = std::max(Style::spaceMd, surfaceH - inst->confirmHint->height() - Style::spaceMd);
        inst->confirmHint->setPosition((surfaceW - inst->confirmHint->width()) * 0.5f, y);
      }
    }
  }

  void ScreenshotRegionOverlay::completeSelection() {
    m_dragging = false;
    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;

    if (width < 2 || height < 2) {
      m_active = false;
      destroySurfaces();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    if (m_confirmRegion) {
      m_confirming = true;
      m_startGlobalX = static_cast<double>(globalX0);
      m_startGlobalY = static_cast<double>(globalY0);
      m_currentGlobalX = static_cast<double>(globalX1);
      m_currentGlobalY = static_cast<double>(globalY1);
      updateSelectionVisuals();
      for (auto& instance : m_instances) {
        if (instance->surface != nullptr) {
          instance->surface->requestRedraw();
        }
      }
      return;
    }

    m_active = false;
    destroySurfaces();

    LogicalRect region{
        .x = globalX0,
        .y = globalY0,
        .width = width,
        .height = height,
    };
    if (m_onComplete) {
      m_onComplete(region, nullptr, ConfirmAction::None);
    }
  }

  void ScreenshotRegionOverlay::confirmPendingSelection(ConfirmAction action) {
    if (!m_active || !m_confirming) {
      return;
    }

    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;

    m_confirming = false;
    m_active = false;
    destroySurfaces();

    if (width < 2 || height < 2) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    LogicalRect region{
        .x = globalX0,
        .y = globalY0,
        .width = width,
        .height = height,
    };
    if (m_onComplete) {
      m_onComplete(region, nullptr, action);
    }
  }

  void ScreenshotRegionOverlay::completeFullscreenPick(wl_output* output) {
    if (!m_active || output == nullptr || m_wayland == nullptr) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    const auto* out = findOutput(*m_wayland, output);
    if (out == nullptr) {
      m_active = false;
      destroySurfaces();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    m_active = false;
    destroySurfaces();

    LogicalRect region{
        .x = 0,
        .y = 0,
        .width = out->logicalWidth,
        .height = out->logicalHeight,
    };
    if (m_onComplete) {
      m_onComplete(region, output, ConfirmAction::None);
    }
  }

} // namespace capture
