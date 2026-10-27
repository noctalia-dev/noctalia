#include "shell/plugin_layer/plugin_layer.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "scripting/plugin_runtime_context.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-layer");
  constexpr int kDefaultUpdateIntervalMs = 1000;

  using scripting::PluginLayerAnchor::Bottom;
  using scripting::PluginLayerAnchor::Left;
  using scripting::PluginLayerAnchor::Right;
  using scripting::PluginLayerAnchor::Top;

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
      return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  LayerShellLayer toLayerShellLayer(scripting::PluginLayerLevel level) {
    switch (level) {
    case scripting::PluginLayerLevel::Background:
      return LayerShellLayer::Background;
    case scripting::PluginLayerLevel::Bottom:
      return LayerShellLayer::Bottom;
    case scripting::PluginLayerLevel::Top:
      return LayerShellLayer::Top;
    case scripting::PluginLayerLevel::Overlay:
      return LayerShellLayer::Overlay;
    }
    return LayerShellLayer::Top;
  }

  LayerShellKeyboard toLayerShellKeyboard(scripting::PluginLayerKeyboard keyboard) {
    switch (keyboard) {
    case scripting::PluginLayerKeyboard::None:
      return LayerShellKeyboard::None;
    case scripting::PluginLayerKeyboard::OnDemand:
      return LayerShellKeyboard::OnDemand;
    case scripting::PluginLayerKeyboard::Exclusive:
      return LayerShellKeyboard::Exclusive;
    }
    return LayerShellKeyboard::None;
  }

  bool stretchesWidth(const scripting::PluginLayerConfig& config) {
    return config.width == 0 && (config.anchors & Left) != 0 && (config.anchors & Right) != 0;
  }

  bool stretchesHeight(const scripting::PluginLayerConfig& config) {
    return config.height == 0 && (config.anchors & Top) != 0 && (config.anchors & Bottom) != 0;
  }

  // Extent reserved by exclusive_zone = "auto": the surface size perpendicular to its single
  // anchored edge. Corner or centered anchoring has no edge to reserve against.
  std::int32_t autoExclusiveZone(std::uint32_t anchors, std::uint32_t width, std::uint32_t height) {
    const bool top = (anchors & Top) != 0;
    const bool bottom = (anchors & Bottom) != 0;
    const bool left = (anchors & Left) != 0;
    const bool right = (anchors & Right) != 0;
    if (top != bottom && (left == right)) {
      return static_cast<std::int32_t>(height);
    }
    if (left != right && (top == bottom)) {
      return static_cast<std::int32_t>(width);
    }
    return 0;
  }

} // namespace

PluginLayer::PluginLayer(
    PluginLayerServices services, scripting::PluginRuntimeContext context, const WaylandOutput& output
)
    : m_services(services), m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(std::move(context.pluginDir)), m_scriptApi(context.scriptApi),
      m_settings(std::move(context.settings)), m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient),
      m_clipboard(context.clipboard), m_output(output.output), m_outputName(output.connectorName) {
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

PluginLayer::~PluginLayer() {
  scripting::PluginIpcRouter::instance().unregisterEndpoint(this);
  *m_alive = false;
  m_scriptWatcher.stop();
  m_updateTimer.stop();
  if (m_runtime != nullptr) {
    if (m_runtimeSubscription != 0) {
      m_runtime->unsubscribe(m_runtimeSubscription);
    }
    m_runtime->stop();
  }
  const bool hadKeyboardFocus =
      m_surface != nullptr && m_services.platform.lastKeyboardSurface() == m_surface->wlSurface();
  m_animations.cancelAll();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_reconciler.reset();
  m_flex = nullptr;
  m_sceneRoot.reset();
  m_surface.reset();
  if (hadKeyboardFocus) {
    m_services.platform.stopKeyRepeat();
  }
}

bool PluginLayer::initialize(const scripting::PluginLayerConfig& config) {
  m_config = config;
  const WaylandOutput* output = m_services.platform.findOutputByWl(m_output);
  const auto outputWidth = output != nullptr ? static_cast<std::uint32_t>(std::max(1, output->logicalWidth)) : 1U;
  const auto outputHeight = output != nullptr ? static_cast<std::uint32_t>(std::max(1, output->logicalHeight)) : 1U;

  const std::uint32_t width = requestedExtent(config.width, stretchesWidth(config), 0.0F);
  const std::uint32_t height = requestedExtent(config.height, stretchesHeight(config), 0.0F);
  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-plugin:" + m_entryId,
      .layer = toLayerShellLayer(config.layer),
      .anchor = config.anchors,
      .width = width,
      .height = height,
      .exclusiveZone = config.exclusiveZoneAuto ? 0 : config.exclusiveZone,
      .marginTop = config.marginTop,
      .marginRight = config.marginRight,
      .marginBottom = config.marginBottom,
      .marginLeft = config.marginLeft,
      .keyboard = toLayerShellKeyboard(config.keyboard),
      .defaultWidth = width != 0 ? width : outputWidth,
      .defaultHeight = height != 0 ? height : outputHeight,
  };

  m_surface = std::make_unique<LayerSurface>(m_services.platform.wayland(), std::move(surfaceConfig));
  m_surface->setRenderContext(m_services.renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    m_surface->requestLayout();
  });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  m_surface->setFrameTickCallback([this](float deltaMs) {
    if (m_runtime == nullptr || !m_needsFrameTick) {
      return;
    }
    // Coalesced like the desktop-widget path: a slow script only ever sees the latest frame.
    (void)m_runtime->enqueueCallStrings(
        "onFrameTick", std::format("{:.3F}", deltaMs), {}, makeScriptSnapshot(), /*coalesce=*/true
    );
    m_surface->requestRedraw();
  });

  if (!m_surface->initialize(m_output)) {
    kLog.warn("failed to initialize layer '{}' on {}", m_entryId, m_outputName);
    m_surface.reset();
    return false;
  }
  m_surface->setClickThrough(config.clickThrough);

  startScript();
  m_scriptWatcher.start(m_fileWatcher, m_sourcePath, [this] { reloadScript(); });
  return true;
}

void PluginLayer::applyConfig(const scripting::PluginLayerConfig& config) {
  m_config = config;
  if (m_surface == nullptr) {
    return;
  }
  m_surface->setLayer(toLayerShellLayer(config.layer));
  m_surface->setAnchorAndSize(
      config.anchors, requestedExtent(config.width, stretchesWidth(config), m_contentWidth),
      requestedExtent(config.height, stretchesHeight(config), m_contentHeight)
  );
  m_surface->setMargins(config.marginTop, config.marginRight, config.marginBottom, config.marginLeft);
  if (!config.exclusiveZoneAuto) {
    m_surface->setExclusiveZone(config.exclusiveZone);
  }
  m_surface->setKeyboardInteractivity(toLayerShellKeyboard(config.keyboard));
  m_surface->setClickThrough(config.clickThrough);
  m_surface->requestLayout();
}

bool PluginLayer::ownsSurface(wl_surface* surface) const noexcept {
  return surface != nullptr && m_surface != nullptr && m_surface->wlSurface() == surface;
}

void PluginLayer::requestLayout() {
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

std::uint32_t PluginLayer::requestedExtent(std::uint32_t configured, bool stretched, float content) const {
  if (stretched) {
    return 0;
  }
  if (configured != 0) {
    return configured;
  }
  return static_cast<std::uint32_t>(std::max(1.0F, std::ceil(content)));
}

float PluginLayer::contentScale() const {
  return m_services.config != nullptr ? std::max(0.01F, m_services.config->config().accessibility.uiScale) : 1.0F;
}

void PluginLayer::buildScene() {
  uiAssertNotRendering("PluginLayer::buildScene");
  if (m_sceneRoot != nullptr || m_surface == nullptr) {
    return;
  }

  m_sceneRoot = ui::node({});
  m_sceneRoot->setAnimationManager(&m_animations);
  m_sceneRoot->addChild(ui::column({.out = &m_flex, .align = FlexAlign::Stretch}));

  m_reconciler.reset();
  m_treeDirty = m_tree.has_value();
  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, makeScriptSnapshot(), callback.coalesce, callback.coalesceKey
      );
    }
  });
  m_reconciler.setPathResolver([this](const std::string& path) { return resolvePluginPath(path); });
  m_reconciler.setFocusRequestSink([this](InputArea* area) { m_pendingFocusArea = area; });

  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_inputDispatcher.setTextInputContext(m_surface->wlSurface(), m_services.platform.wayland().textInputService());
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.platform.setCursorShape(serial, shape);
  });
  m_surface->setSceneRoot(m_sceneRoot.get());
}

void PluginLayer::prepareFrame(bool needsUpdate, bool needsLayout) {
  (void)needsUpdate;
  if (m_surface == nullptr || m_services.renderContext == nullptr) {
    return;
  }
  m_services.renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();

  buildScene();
  if (needsLayout || m_treeDirty) {
    layoutScene(renderer);
  }
  if (auto* area = std::exchange(m_pendingFocusArea, nullptr); area != nullptr) {
    m_inputDispatcher.setFocus(area);
  }
  if (m_needsFrameTick) {
    m_surface->requestFrameTick();
  }
}

void PluginLayer::layoutScene(Renderer& renderer) {
  if (m_flex == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  UiPhaseScope layoutPhase(UiPhase::Layout);
  if (m_tree.has_value() && m_treeDirty) {
    m_reconciler.setScale(contentScale());
    (void)m_reconciler.reconcile(*m_flex, *m_tree, renderer);
  }
  m_treeDirty = false;

  const auto surfaceWidth = static_cast<float>(m_surface->width());
  const auto surfaceHeight = static_cast<float>(m_surface->height());
  const bool fixedWidth = stretchesWidth(m_config) || m_config.width != 0;
  const bool fixedHeight = stretchesHeight(m_config) || m_config.height != 0;

  LayoutConstraints constraints = LayoutConstraints::unconstrained();
  if (fixedWidth) {
    constraints.setExactWidth(surfaceWidth);
  }
  if (fixedHeight) {
    constraints.setExactHeight(surfaceHeight);
  }
  const LayoutSize content = m_flex->measure(renderer, constraints);
  m_contentWidth = content.width;
  m_contentHeight = content.height;
  m_flex->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = content.width, .height = content.height});
  m_sceneRoot->setFrameSize(surfaceWidth, surfaceHeight);

  const std::uint32_t width = requestedExtent(m_config.width, stretchesWidth(m_config), content.width);
  const std::uint32_t height = requestedExtent(m_config.height, stretchesHeight(m_config), content.height);
  if ((!fixedWidth && width != m_surface->width()) || (!fixedHeight && height != m_surface->height())) {
    m_surface->requestSize(width, height);
  }
  if (m_config.exclusiveZoneAuto) {
    m_surface->setExclusiveZone(autoExclusiveZone(m_config.anchors, m_surface->width(), m_surface->height()));
  }
  if (m_pointerInside) {
    m_inputDispatcher.syncPointerHover();
  }
}

void PluginLayer::syncAfterInput() {
  if (m_sceneRoot == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else if (m_sceneRoot->paintDirty()) {
    m_surface->requestRedraw();
  }
}

void PluginLayer::onPointerEvent(const PointerEvent& event) {
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    m_inputDispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    m_inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_inputDispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    m_inputDispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    m_inputDispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  syncAfterInput();
}

void PluginLayer::onKeyboardEvent(const KeyboardEvent& event) {
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  syncAfterInput();
}

PluginLayer::DispatchResult
PluginLayer::dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) {
  (void)snapshot;
  if (m_runtime == nullptr) {
    return DispatchResult::MissingHost;
  }
  if (m_hasOnIpcKnown && !m_hasOnIpc) {
    return DispatchResult::MissingCallback;
  }
  if (!m_runtime->enqueueCallStrings("onIpc", std::string(event), std::string(payload), makeScriptSnapshot())) {
    return DispatchResult::Failed;
  }
  return DispatchResult::Handled;
}

void PluginLayer::startScript() {
  if (m_sourcePath.empty()) {
    kLog.warn("layer '{}': no source path", m_entryId);
    return;
  }
  std::string source = readFile(m_sourcePath);
  if (source.empty()) {
    kLog.warn("layer '{}': failed to read '{}'", m_entryId, m_sourcePath.string());
    return;
  }

  m_runtime = std::make_shared<scripting::ScriptRuntime>(
      m_entryId, m_settings, m_scriptApi, m_pluginDir, m_httpClient, m_clipboard
  );
  auto alive = std::weak_ptr<bool>(m_alive);
  m_runtimeSubscription = m_runtime->subscribe([this, alive](scripting::ScriptResult result) {
    auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    if (result.modulePathsKnown) {
      m_scriptWatcher.setModulePaths(result.modulePaths);
    }
    handleScriptResult(std::move(result));
  });
  m_runtime->start(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
}

void PluginLayer::reloadScript() {
  std::string source = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (source.empty() || m_runtime == nullptr) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }
  // Tick opt-ins and interval reset to defaults; the reloaded script re-declares them.
  m_needsFrameTick = false;
  m_updateIntervalMs = kDefaultUpdateIntervalMs;
  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;
  m_runtime->reload(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}

void PluginLayer::startUpdateTimer() {
  m_updateTimer.startRepeating(std::chrono::milliseconds(m_updateIntervalMs), [this] {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
    }
  });
}

void PluginLayer::handleScriptResult(scripting::ScriptResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }
  if (result.unhealthy) {
    m_updateTimer.stop();
    m_needsFrameTick = false;
    kLog.warn("layer '{}' disabled after repeated script failures", m_entryId);
  }

  const auto& patch = result.patch;
  if (patch.needsFrameTick.has_value()) {
    const bool was = m_needsFrameTick;
    m_needsFrameTick = *patch.needsFrameTick;
    if (m_needsFrameTick && !was && m_surface != nullptr) {
      m_surface->requestFrameTick();
    }
  }
  if (patch.updateIntervalMs.has_value()) {
    m_updateIntervalMs = std::max(16, *patch.updateIntervalMs);
    startUpdateTimer();
  }
  if (patch.uiTree.has_value() && (!m_tree.has_value() || *patch.uiTree != *m_tree)) {
    m_tree = *patch.uiTree;
    m_treeDirty = true;
    requestLayout();
  }
}

scripting::ScriptSnapshot PluginLayer::makeScriptSnapshot() const {
  return scripting::ScriptSnapshot{
      .isVertical = false,
      .outputName = m_outputName,
      .barName = {},
      .focusedOutputName = {},
  };
}

std::string PluginLayer::resolvePluginPath(const std::string& path) const {
  if (path.empty()) {
    return {};
  }
  if (path[0] == '~') {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) + path.substr(1) : path;
  }
  if (path[0] == '/') {
    return path;
  }
  return (m_pluginDir / path).string();
}
