#include "shell/lockscreen/lockscreen_transition_preview.h"

#include "capture/screencopy_util.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/lockscreen/lockscreen_transition_cover.h"
#include "shell/lockscreen/lockscreen_transition_util.h"
#include "shell/render/wallpaper_node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

  constexpr Logger kLog("lockscreen-preview");

  constexpr float kHoldMs = 400.0F;
  // With blur and tint both off the two sides would look identical, so fall back to the default blur.
  constexpr float kFallbackBlurIntensity = 0.5F;

  bool outputEligible(const WaylandOutput& output) noexcept {
    return output.done && output.output != nullptr && output.hasUsableGeometry();
  }

} // namespace

LockscreenTransitionPreview::~LockscreenTransitionPreview() { hide(); }

void LockscreenTransitionPreview::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService
) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_configService = configService;
}

void LockscreenTransitionPreview::play(LockscreenTransition transition) {
  hide();
  if (m_wayland == nullptr || m_renderContext == nullptr || m_configService == nullptr) {
    return;
  }

  const auto& lockscreen = m_configService->config().lockscreen;
  m_transition = lockscreen::renderTransitionKind(transition);
  m_durationMs = lockscreen.transitionDurationMs;
  const auto params = lockscreen::randomTransitionParams(transition, lockscreen.edgeSmoothness);

  std::vector<WaylandOutput> targets;
  std::ranges::copy_if(m_wayland->outputs(), std::back_inserter(targets), outputEligible);

  // Capture before any overlay maps so the preview never shows itself.
  std::vector<std::unique_ptr<Instance>> captured;
  lockscreen::captureOutputs(
      *m_wayland, targets, [] { return false; },
      [&](const WaylandOutput& target, ScreencopyImage&& image) {
        if (!screencopy::orientCaptureNative(image, *m_wayland, target.output)) {
          kLog.warn("preview capture orientation failed for {}", target.connectorName);
          return;
        }
        auto inst = std::make_unique<Instance>();
        inst->output = target.output;
        inst->capture = std::move(image);
        inst->params = params;
        captured.push_back(std::move(inst));
      }
  );
  if (captured.empty()) {
    kLog.warn("lockscreen transition preview needs a desktop capture, but none succeeded");
    return;
  }

  for (auto& inst : captured) {
    auto surfaceConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-lockscreen-preview",
        .layer = LayerShellLayer::Overlay,
        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
        .width = 0,
        .height = 0,
        .exclusiveZone = -1,
        .keyboard = LayerShellKeyboard::Exclusive,
        .defaultWidth = 1920,
        .defaultHeight = 1080,
    };
    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      instPtr->surface->requestLayout();
    });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool /*needsUpdate*/, bool /*needsLayout*/) {
      prepareFrame(*instPtr);
    });
    inst->surface->setAnimationManager(&inst->animations);
    inst->surface->setRenderContext(m_renderContext);
    if (!inst->surface->initialize(inst->output)) {
      kLog.warn("lockscreen transition preview: failed to initialize layer surface");
      continue;
    }
    m_instances.push_back(std::move(inst));
  }
}

void LockscreenTransitionPreview::hide() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
    releaseTextures(*inst);
  }
  m_instances.clear();
}

void LockscreenTransitionPreview::prepareForGraphicsReset() noexcept {
  for (auto& inst : m_instances) {
    inst->blurCache.abandon();
    inst->blurredTexture = {};
    inst->captureTexture = {};
  }
}

void LockscreenTransitionPreview::onOutputChange() {
  if (isActive()) {
    hide();
  }
}

bool LockscreenTransitionPreview::onPointerEvent(const PointerEvent& event) {
  if (!isActive()) {
    return false;
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    DeferredCall::callLater([this] { hide(); });
  }
  return true;
}

bool LockscreenTransitionPreview::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isActive()) {
    return false;
  }
  if (event.pressed && !event.repeat) {
    DeferredCall::callLater([this] { hide(); });
  }
  return true;
}

void LockscreenTransitionPreview::prepareFrame(Instance& inst) {
  if (m_renderContext == nullptr || inst.surface == nullptr || inst.surface->width() == 0) {
    return;
  }
  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const auto w = static_cast<float>(inst.surface->width());
  const auto h = static_cast<float>(inst.surface->height());
  if (inst.sceneRoot == nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (!buildScene(inst)) {
      DeferredCall::callLater([this] { hide(); });
      return;
    }
  }

  UiPhaseScope layoutPhase(UiPhase::Layout);
  inst.sceneRoot->setSize(w, h);
  inst.background->setPosition(0.0F, 0.0F);
  inst.background->setSize(w, h);
  inst.tint->setPosition(0.0F, 0.0F);
  inst.tint->setSize(w, h);
  inst.cover->setPosition(0.0F, 0.0F);
  inst.cover->setSize(w, h);
  inst.params.aspectRatio = h > 0.0F ? w / h : 1.0F;
  inst.cover->setTransition(m_transition, inst.progress, inst.params);

  if (!inst.started) {
    inst.started = true;
    start(inst);
  }
}

bool LockscreenTransitionPreview::buildScene(Instance& inst) {
  uiAssertNotRendering("LockscreenTransitionPreview::buildScene");
  const auto& lockscreen = m_configService->config().lockscreen;
  auto& tm = m_renderContext->textureManager();

  inst.captureTexture = tm.loadFromRgba(inst.capture.rgba.data(), inst.capture.width, inst.capture.height, false);
  if (inst.captureTexture.id == 0) {
    kLog.warn("lockscreen transition preview: failed to upload desktop capture");
    return false;
  }

  const float blurIntensity = lockscreen.blurIntensity > 0.0F || lockscreen.tintIntensity > 0.0F
      ? lockscreen.blurIntensity
      : kFallbackBlurIntensity;
  if (blurIntensity > 0.0F) {
    static constexpr int kBlurRounds = 3;
    inst.blurredTexture = inst.blurCache.get(
        m_renderContext->backend(), inst.captureTexture, inst.surface->renderTarget().bufferWidth(),
        inst.surface->renderTarget().bufferHeight(), blurIntensity * 40.0F, kBlurRounds
    );
  }
  const TextureHandle backgroundTexture = inst.blurredTexture.id != 0 ? inst.blurredTexture : inst.captureTexture;

  inst.sceneRoot = ui::node({});
  inst.surface->setSceneRoot(inst.sceneRoot.get());

  auto background = std::make_unique<WallpaperNode>();
  inst.background = static_cast<WallpaperNode*>(inst.sceneRoot->addChild(std::move(background)));
  inst.background->setZIndex(0);
  inst.background->setTextures(
      backgroundTexture.id, {}, static_cast<float>(backgroundTexture.width),
      static_cast<float>(backgroundTexture.height), 0.0F, 0.0F
  );
  inst.background->setTransition(WallpaperTransition::Fade, 0.0F, TransitionParams{});
  inst.background->setFillMode(m_configService->config().wallpaper.fillMode);

  auto tint = ui::box({});
  tint->setZIndex(1);
  tint->setVisible(lockscreen.tintIntensity > 0.0F);
  tint->setStyle(
      RoundedRectStyle{
          .fill = colorForRole(ColorRole::Surface, lockscreen.tintIntensity),
          .fillMode = FillMode::Solid,
      }
  );
  inst.tint = static_cast<Box*>(inst.sceneRoot->addChild(std::move(tint)));

  auto cover = std::make_unique<LockscreenTransitionCover>();
  cover->setZIndex(2);
  cover->setTexture(inst.captureTexture.id);
  inst.cover = static_cast<LockscreenTransitionCover*>(inst.sceneRoot->addChild(std::move(cover)));
  return true;
}

void LockscreenTransitionPreview::start(Instance& inst) {
  Instance* instPtr = &inst;
  const auto setProgress = [this, instPtr](float progress) {
    instPtr->progress = progress;
    instPtr->cover->setTransition(m_transition, progress, instPtr->params);
    instPtr->surface->requestUpdate();
  };
  inst.animations.animateTimer(
      0.0F, 1.0F, m_durationMs, Easing::EaseInOutCubic, setProgress, [this, instPtr, setProgress] {
        instPtr->animations.animateTimer(
            1.0F, 1.0F, kHoldMs, Easing::Linear, setProgress, [this, instPtr, setProgress] {
              instPtr->animations.animateTimer(1.0F, 0.0F, m_durationMs, Easing::EaseInOutCubic, setProgress, [this] {
                DeferredCall::callLater([this] { hide(); });
              });
            }
        );
      }
  );
}

void LockscreenTransitionPreview::releaseTextures(Instance& inst) {
  if (m_renderContext != nullptr) {
    auto& tm = m_renderContext->textureManager();
    m_renderContext->backend().makeCurrentNoSurface();
    if (inst.blurredTexture.id != 0) {
      tm.unload(inst.blurredTexture);
    }
    if (inst.captureTexture.id != 0) {
      tm.unload(inst.captureTexture);
    }
  }
  inst.blurredTexture = {};
  inst.captureTexture = {};
  inst.blurCache.destroy();
}
