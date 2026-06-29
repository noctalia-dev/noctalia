#include "idle/idle_grace_overlay.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr Logger kLog("idle-grace");

  bool outputEligible(const WaylandOutput& output) noexcept {
    return output.done && output.output != nullptr && output.hasUsableGeometry();
  }

} // namespace

void IdleGraceOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
}

void IdleGraceOverlay::onOutputChange() {
  if (!m_instances.empty() && !surfacesMatchOutputs()) {
    destroySurfaces();
  }
}

void IdleGraceOverlay::show(std::chrono::milliseconds fadeIn, std::function<void()> onFadeComplete) {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    if (onFadeComplete) {
      DeferredCall::callLater(std::move(onFadeComplete));
    }
    return;
  }
  m_showRequested = true;
  m_onAllFadesComplete = std::move(onFadeComplete);
  m_fadeCompletionsReceived = 0;
  m_fadeCompletionTarget = 0;
  ensureSurfaces();
  const auto fade = std::max(std::chrono::milliseconds(1), fadeIn);
  if (m_instances.empty()) {
    auto cb = std::move(m_onAllFadesComplete);
    m_onAllFadesComplete = {};
    if (cb) {
      DeferredCall::callLater(std::move(cb));
    }
    return;
  }
  std::uint32_t fadeTargets = 0;
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    fadeTargets++;
    inst->visible = true;
    inst->pendingFadeIn = fade;
    inst->surface->requestLayout();
    inst->surface->requestUpdate();
  }
  if (fadeTargets == 0) {
    auto cb = std::move(m_onAllFadesComplete);
    m_onAllFadesComplete = {};
    if (cb) {
      DeferredCall::callLater(std::move(cb));
    }
    return;
  }
  m_fadeCompletionTarget = fadeTargets;
}

void IdleGraceOverlay::hide() {
  m_showRequested = false;
  destroySurfaces();
}

void IdleGraceOverlay::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  if (!m_showRequested) {
    return;
  }

  if (!m_instances.empty() && !surfacesMatchOutputs()) {
    destroySurfaces();
  }
  if (!m_instances.empty()) {
    return;
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!outputEligible(output)) {
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = output.scale;

    auto surfaceConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-idle-grace",
        .layer = LayerShellLayer::Overlay,
        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
        .width = 0,
        .height = 0,
        // -1: extend over exclusive zones (e.g. bar) so the dim reads as a true fullscreen overlay.
        .exclusiveZone = -1,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = 1920,
        .defaultHeight = 1080,
    };

    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      instPtr->surface->requestLayout();
    });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
      prepareFrame(*instPtr, needsUpdate, needsLayout);
    });
    inst->surface->setAnimationManager(&inst->animations);
    inst->surface->setRenderContext(m_renderContext);

    if (!inst->surface->initialize(output.output)) {
      kLog.warn("idle grace: failed to initialize layer surface on {}", output.connectorName);
      continue;
    }

    inst->surface->setClickThrough(true);
    inst->surface->setInputRegion({});

    m_instances.push_back(std::move(inst));
  }
}

bool IdleGraceOverlay::surfacesMatchOutputs() const {
  if (m_wayland == nullptr) {
    return m_instances.empty();
  }
  const auto& outputs = m_wayland->outputs();
  const auto eligibleCount = static_cast<std::size_t>(std::ranges::count_if(outputs, outputEligible));
  if (m_instances.size() != eligibleCount) {
    return false;
  }
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      return false;
    }
    const auto it = std::ranges::find_if(outputs, [&](const WaylandOutput& output) {
      return outputEligible(output) && output.output == instance->output;
    });
    if (it == outputs.end()) {
      return false;
    }
  }
  return true;
}

void IdleGraceOverlay::destroySurfaces() {
  m_onAllFadesComplete = {};
  m_fadeCompletionTarget = 0;
  m_fadeCompletionsReceived = 0;
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
}

void IdleGraceOverlay::prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
  if (needsSceneBuild) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(inst, width, height);
  }

  if ((needsUpdate || needsLayout || needsSceneBuild) && inst.sceneRoot != nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (inst.dim != nullptr) {
      inst.dim->setPosition(0.0f, 0.0f);
      inst.dim->setSize(static_cast<float>(width), static_cast<float>(height));
    }
    if (inst.pendingFadeIn.has_value()) {
      const auto ms = *inst.pendingFadeIn;
      inst.pendingFadeIn.reset();
      startFadeIn(inst, ms);
    }
  }
}

void IdleGraceOverlay::buildScene(Instance& inst, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("IdleGraceOverlay::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  inst.sceneRoot = std::make_unique<Node>();
  inst.sceneRoot->setSize(w, h);
  inst.sceneRoot->setOpacity(1.0f);
  inst.surface->setSceneRoot(inst.sceneRoot.get());

  // Fullscreen tint: fade this layer only so the Wayland buffer stays a proper overlay (transparent
  // clear + premultiplied tint).
  auto dim = std::make_unique<Box>();
  dim->setFill(fixedColorSpec(rgba(0.0f, 0.0f, 0.0f, 1.0f)));
  dim->setOpacity(0.0f);
  dim->setPosition(0.0f, 0.0f);
  dim->setSize(w, h);
  inst.dim = static_cast<Box*>(inst.sceneRoot->addChild(std::move(dim)));
}

void IdleGraceOverlay::startFadeIn(Instance& inst, std::chrono::milliseconds fadeIn) {
  if (inst.dim == nullptr) {
    return;
  }
  if (inst.fadeAnimId != 0) {
    inst.animations.cancel(inst.fadeAnimId);
    inst.fadeAnimId = 0;
  }

  const float start = inst.dim->opacity();
  const auto durationMs = static_cast<float>(fadeIn.count());
  Instance* instPtr = &inst;
  Box* dimPtr = inst.dim;
  inst.fadeAnimId = inst.animations.animate(
      start, 1.0f, durationMs, Easing::EaseInOutCubic, [dimPtr](float v) { dimPtr->setOpacity(v); },
      [this, instPtr]() {
        instPtr->fadeAnimId = 0;
        onFadeInstanceComplete();
      },
      dimPtr
  );
}

void IdleGraceOverlay::onFadeInstanceComplete() {
  if (m_fadeCompletionTarget == 0) {
    return;
  }
  m_fadeCompletionsReceived++;
  if (m_fadeCompletionsReceived < m_fadeCompletionTarget) {
    return;
  }
  m_fadeCompletionTarget = 0;
  m_fadeCompletionsReceived = 0;
  auto cb = std::move(m_onAllFadesComplete);
  m_onAllFadesComplete = {};
  if (cb) {
    cb();
  }
}
