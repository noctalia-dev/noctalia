#include "shell/bar/widgets/plugin_widget.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <optional>
#include <sstream>
#include <vector>

namespace {
  constexpr Logger kLog("plugin-widget");
  constexpr std::chrono::milliseconds kDeferredUpdateRetry{50};
  constexpr std::chrono::milliseconds kImageReloadRetry{150};
  constexpr int kImageReloadRetryCount = 2;
  constexpr std::chrono::milliseconds kTimerPhaseStep{50};
  constexpr std::chrono::milliseconds kTimerMaxPhase{500};

  bool
  settingBool(const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key, bool def) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return def;
    }
    const auto* value = std::get_if<bool>(&it->second);
    return value != nullptr ? *value : def;
  }

  std::int64_t settingInt(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key, std::int64_t def
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return def;
    }
    const auto* value = std::get_if<std::int64_t>(&it->second);
    return value != nullptr ? *value : def;
  }

  // The bar surface never takes keyboard focus and clips to bar thickness, so
  // keyboard/scroll controls cannot work in a bar tree. Removes them in place,
  // recording each dropped type; returns false when `node` itself is dropped.
  bool filterUnsupportedBarControls(ui::UiTreeNode& node, std::vector<std::string>& dropped) {
    const bool unsupported = node.type == "input" || node.type == "select" || node.type == "scroll";
    if (unsupported) {
      if (std::ranges::find(dropped, node.type) == dropped.end()) {
        dropped.push_back(node.type);
      }
      return false;
    }
    std::erase_if(node.children, [&dropped](ui::UiTreeNode& child) {
      return !filterUnsupportedBarControls(child, dropped);
    });
    return true;
  }

  std::string joinSpectrumValues(const std::vector<float>& values) {
    std::ostringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out << std::setprecision(4);
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        out << ',';
      }
      out << values[i];
    }
    return out.str();
  }

  std::uint32_t nextTimerPhase() {
    static std::atomic<std::uint32_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f)
      return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

} // namespace

PluginWidget::PluginWidget(
    scripting::PluginRuntimeContext context, std::string barName, std::string outputName, bool enableScroll
)
    : m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(m_sourcePath.parent_path()), m_barName(std::move(barName)), m_outputName(std::move(outputName)),
      m_scriptApi(context.scriptApi), m_settings(std::move(context.settings)), m_fileWatcher(context.fileWatcher),
      m_platform(context.platform), m_clipboard(context.clipboard), m_httpClient(context.httpClient),
      m_audioSpectrum(context.audioSpectrum), m_mpris(context.mpris), m_timerPhase(nextTimerPhase()),
      m_enableScroll(enableScroll) {
  m_audioSpectrumEnabled = settingBool(m_settings, "audio_spectrum", false);
  m_audioSpectrumBands =
      static_cast<int>(std::clamp<std::int64_t>(settingInt(m_settings, "audio_spectrum_bands", 16), 1, 128));
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

PluginWidget::~PluginWidget() {
  scripting::PluginIpcRouter::instance().unregisterEndpoint(this);
  if (m_alive) {
    *m_alive = false;
  }
  teardownAudioSpectrum();
  teardownImageWatch();
  teardownScriptWatch();
  if (m_runtime != nullptr) {
    if (m_runtimeSubscription != 0) {
      m_runtime->unsubscribe(m_runtimeSubscription);
    }
    m_runtime->stop();
  }
}

std::filesystem::path PluginWidget::resolvePluginPath(std::string_view path) const {
  if (path.empty()) {
    return {};
  }
  if (path[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      return std::string(home) + std::string(path.substr(1));
    }
    return std::filesystem::path(path);
  }
  if (path[0] == '/') {
    return std::filesystem::path(path);
  }
  return m_pluginDir / path;
}

void PluginWidget::create() {
  auto area = ui::inputArea({});
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (!m_runtime)
      return;
    const char* fn = nullptr;
    switch (data.button) {
    case BTN_LEFT:
      fn = "onClick";
      break;
    case BTN_RIGHT:
      fn = "onRightClick";
      break;
    case BTN_MIDDLE:
      fn = "onMiddleClick";
      break;
    default:
      return;
    }
    (void)m_runtime->enqueueCall(fn, makeScriptSnapshot());
  });
  area->setOnEnter([this](const InputArea::PointerData&) {
    if (m_runtime)
      (void)m_runtime->enqueueCallBool("onHover", true, makeScriptSnapshot());
  });
  area->setOnLeave([this]() {
    if (m_runtime)
      (void)m_runtime->enqueueCallBool("onHover", false, makeScriptSnapshot());
  });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_enableScroll || m_runtime == nullptr || !m_runtime->hasOnScroll())
      return false;
    // Whole detent steps, so a wheel notch and a touchpad flick mean the same
    // thing to the script. Continuous sources report 0 until a detent accrues.
    const float steps = data.scrollSteps();
    if (steps == 0.0F)
      return false;
    const char* axis = data.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "vertical" : "horizontal";
    // The third argument is true only on the first step of a flick in that direction. A script
    // stepping through a list acts on it alone; one ramping a value ignores it.
    (void)m_runtime->enqueueCallArgs(
        "onScroll", {std::string(axis), static_cast<double>(steps), data.scrollStepStartsGesture()},
        makeScriptSnapshot()
    );
    return true;
  });

  auto flex = ui::row({
      .out = &m_flex,
      .align = FlexAlign::Center,
      .gap = Style::spaceXs,
  });

  flex->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .visible = false,
      })
  );

  flex->addChild(
      ui::image({
          .out = &m_image,
          .fit = ImageFit::Contain,
          .visible = false,
      })
  );

  flex->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = false,
      })
  );

  area->addChild(std::move(flex));

  // Declarative host: barWidget.render(tree) reconciles into this Flex and
  // hides the imperative row above.
  area->addChild(
      ui::row({
          .out = &m_uiHost,
          .align = FlexAlign::Center,
          .visible = false,
      })
  );

  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, makeScriptSnapshot(), callback.coalesce
      );
    }
  });
  m_reconciler.setPathResolver([this](const std::string& path) { return resolvePluginPath(path).string(); });
  m_reconciler.setCompactControls(true);

  m_area = area.get();
  setRoot(std::move(area));

  if (m_sourcePath.empty()) {
    kLog.warn("plugin widget '{}': no source path", m_entryId);
    return;
  }
  std::string source = readFile(m_sourcePath);
  if (source.empty()) {
    kLog.warn("plugin widget '{}': failed to read '{}'", m_entryId, m_sourcePath.string());
    return;
  }

  auto alive = std::weak_ptr<bool>(m_alive);
  m_runtime = std::make_shared<scripting::ScriptRuntime>(
      m_entryId, m_settings, m_scriptApi, m_pluginDir, m_httpClient, m_clipboard,
      [this, alive](std::string_view panelId) {
        auto token = alive.lock();
        if (token == nullptr || !*token) {
          return;
        }
        requestPanelToggle(panelId);
      }
  );

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
  setupScriptWatch();
  setupAudioSpectrum();
}

void PluginWidget::setupAudioSpectrum() {
  if (!m_audioSpectrumEnabled || m_audioSpectrum == nullptr || m_audioSpectrumListenerId != 0) {
    return;
  }
  m_audioSpectrumListenerId =
      m_audioSpectrum->addChangeListener(m_audioSpectrumBands, [this]() { handleAudioSpectrumChanged(); });
}

void PluginWidget::teardownAudioSpectrum() {
  if (m_audioSpectrum != nullptr && m_audioSpectrumListenerId != 0) {
    m_audioSpectrum->removeChangeListener(m_audioSpectrumListenerId);
  }
  m_audioSpectrumListenerId = 0;
}

void PluginWidget::handleAudioSpectrumChanged() {
  if (m_runtime == nullptr || m_audioSpectrum == nullptr || m_audioSpectrumListenerId == 0) {
    return;
  }
  const bool audioActive = !m_audioSpectrum->idle();
  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  const bool mprisPlaying = active.has_value() && active->playbackStatus == "Playing";
  const std::string state = std::string(audioActive ? "1" : "0") + "," + (mprisPlaying ? "1" : "0");
  // Coalesce: at ~60Hz only the latest frame matters, so a slow script can't
  // accumulate stale spectrum events.
  (void)m_runtime->enqueueCallStrings(
      "onAudioSpectrum", joinSpectrumValues(m_audioSpectrum->values(m_audioSpectrumListenerId)), state,
      makeScriptSnapshot(), /*coalesce=*/true
  );
}

void PluginWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  m_isVertical = containerHeight > containerWidth;
  if (!m_flex)
    return;

  m_flex->setDirection(m_isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal);

  if (m_tree.has_value() && m_uiHost != nullptr) {
    m_uiHost->setDirection(m_isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal);
    m_reconciler.setScale(contentScale());
    m_reconciler.setTextDefaults(labelFontFamily(), labelFontWeight());
    (void)m_reconciler.reconcile(*m_uiHost, *m_tree, renderer);
    m_uiHost->layout(renderer);
    if (m_area)
      m_area->setSize(m_uiHost->width(), m_uiHost->height());
    return;
  }

  m_label->setColor(resolveScriptColor(m_textColor));
  m_label->setFontWeight(labelFontWeight());
  m_label->setVisible(!m_label->text().empty());
  if (m_label->visible()) {
    m_label->measure(renderer);
  }

  if (m_glyphVisible) {
    m_glyph->setColor(resolveScriptColor(m_glyphColor));
    m_glyph->measure(renderer);
  }

  syncImage(renderer);

  m_flex->layout(renderer);

  if (m_area)
    m_area->setSize(m_flex->width(), m_flex->height());
}

void PluginWidget::doUpdate(Renderer&) {}

void PluginWidget::luaSetText(std::string_view text) {
  if (!m_label)
    return;
  bool changed = m_label->setText(text);
  bool vis = !text.empty();
  if (m_label->visible() != vis) {
    m_label->setVisible(vis);
    changed = true;
  }
  m_dirty |= changed;
}

void PluginWidget::luaSetGlyph(std::string_view name) {
  if (!m_glyph)
    return;
  bool changed = m_glyph->setGlyph(name);
  if (!m_imagePath.empty()) {
    m_imagePath.clear();
    m_resolvedImagePath.clear();
    m_imageWidth = 0.0F;
    m_imageHeight = 0.0F;
    m_imageWatch = false;
    m_imageForceReload = false;
    m_imageDirty = true;
    m_imageReloadRetries = 0;
    m_imageReloadRetryTimer.stop();
    teardownImageWatch();
    if (m_image != nullptr) {
      m_image->setVisible(false);
    }
    changed = true;
  }
  if (!m_glyphVisible) {
    m_glyph->setVisible(true);
    m_glyphVisible = true;
    changed = true;
  }
  m_dirty |= changed;
}

void PluginWidget::luaSetImage(std::string_view path, bool watch, float width, float height) {
  if (m_image == nullptr) {
    return;
  }

  std::string nextPath(path);
  const bool nextWatch = watch && !nextPath.empty();
  const float nextWidth = std::max(0.0F, width);
  const float nextHeight = std::max(0.0F, height);
  const bool pathChanged = nextPath != m_imagePath;
  const bool watchChanged = nextWatch != m_imageWatch;
  const bool sizeChanged = nextWidth != m_imageWidth || nextHeight != m_imageHeight;
  if (!pathChanged && !watchChanged && !sizeChanged && !m_glyphVisible) {
    return;
  }

  m_imagePath = std::move(nextPath);
  m_resolvedImagePath = m_imagePath.empty() ? std::filesystem::path{} : resolvePluginPath(m_imagePath);
  m_imageWatch = nextWatch;
  m_imageWidth = nextWidth;
  m_imageHeight = nextHeight;
  m_imageDirty = true;
  m_imageForceReload = false;
  m_imageReloadRetries = 0;
  m_imageReloadRetryTimer.stop();

  if (m_glyph != nullptr && m_glyphVisible) {
    m_glyph->setVisible(false);
    m_glyphVisible = false;
  }

  setupImageWatch();
  m_dirty = true;
}

void PluginWidget::luaSetTooltip(const scripting::ScriptTooltipPatch& tooltip) {
  if (m_area == nullptr) {
    return;
  }

  if (tooltip.clear || (!tooltip.hasRows() && tooltip.text.empty())) {
    m_area->clearTooltip();
    return;
  }

  if (tooltip.hasRows()) {
    std::vector<TooltipRow> rows;
    rows.reserve(tooltip.rows.size());
    for (const auto& row : tooltip.rows) {
      rows.push_back({.key = row.key, .value = row.value});
    }
    m_area->setTooltip(std::move(rows));
    return;
  }

  m_area->setTooltip(tooltip.text);
}

void PluginWidget::luaSetFont(std::string_view family, std::string_view baseline) {
  if (!m_label)
    return;
  LabelBaselineMode mode = LabelBaselineMode::Text;
  if (!baseline.empty()) {
    if (auto parsed = labelBaselineModeFromToken(baseline)) {
      mode = *parsed;
    } else {
      kLog.warn("plugin widget '{}': unknown font baseline '{}'", m_entryId, baseline);
    }
  }
  m_label->setBaselineMode(mode);
  m_label->setFontFamily(std::string(family));
  m_dirty = true;
}

void PluginWidget::luaSetColor(std::string_view role, std::string_view mode) {
  ScriptColorState next{.color = scriptColorFromToken(role), .mode = scriptColorModeFromToken(mode)};
  if (!next.color.has_value()) {
    next.mode = ScriptColorMode::Auto;
  }
  if (next != m_textColor) {
    m_textColor = next;
    m_dirty = true;
  }
}

void PluginWidget::luaSetGlyphColor(std::string_view role, std::string_view mode) {
  ScriptColorState next{.color = scriptColorFromToken(role), .mode = scriptColorModeFromToken(mode)};
  if (!next.color.has_value()) {
    next.mode = ScriptColorMode::Auto;
  }
  if (next != m_glyphColor) {
    m_glyphColor = next;
    m_dirty = true;
  }
}

void PluginWidget::luaSetUpdateInterval(float ms) {
  m_updateIntervalMs = std::max(16, static_cast<int>(ms));
  startUpdateTimer();
}

void PluginWidget::setUpdateDeferralCallback(std::function<bool()> callback) {
  m_updateDeferralCallback = std::move(callback);
}

void PluginWidget::luaSetVisible(bool visible) {
  auto* node = root();
  if (!node || node->visible() == visible)
    return;
  node->setVisible(visible);
  m_dirty = true;
}

PluginWidget::DispatchResult
PluginWidget::dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) {
  (void)snapshot;
  if (!m_runtime) {
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

ColorSpec PluginWidget::resolveScriptColor(const ScriptColorState& state) const noexcept {
  const ColorSpec fallback = colorSpecFromRole(ColorRole::OnSurface);
  if (!state.color.has_value()) {
    return widgetForegroundOr(fallback);
  }
  if (!state.color->role.has_value()
      || state.mode == ScriptColorMode::Script
      || *state.color->role != ColorRole::OnSurface) {
    return *state.color;
  }
  return widgetForegroundOr(fallback);
}

PluginWidget::ScriptColorMode PluginWidget::scriptColorModeFromToken(std::string_view token) noexcept {
  return token == "script" ? ScriptColorMode::Script : ScriptColorMode::Auto;
}

std::optional<ColorSpec> PluginWidget::scriptColorFromToken(std::string_view token) noexcept {
  if (auto role = colorRoleFromToken(token); role.has_value()) {
    return colorSpecFromRole(*role);
  }
  Color fixed;
  if (tryParseHexColor(token, fixed)) {
    return fixedColorSpec(fixed);
  }
  return std::nullopt;
}

void PluginWidget::startUpdateTimer() {
  ++m_updateTimerGeneration;
  m_updateDeferred = false;
  m_deferredUpdateTimer.stop();

  const auto interval = std::chrono::milliseconds(m_updateIntervalMs);
  const auto generation = m_updateTimerGeneration;
  m_updateTimer.start(initialUpdateDelay(interval), [this, generation, interval] {
    if (m_updateTimerGeneration != generation) {
      return;
    }
    handleUpdateTimer();
    if (m_updateTimerGeneration != generation) {
      return;
    }
    m_updateTimer.startRepeating(interval, [this, generation] {
      if (m_updateTimerGeneration == generation) {
        handleUpdateTimer();
      }
    });
  });
}

void PluginWidget::handleUpdateTimer() {
  if (shouldDeferUpdate()) {
    scheduleDeferredUpdate();
    return;
  }
  runScriptUpdate();
}

void PluginWidget::scheduleDeferredUpdate() {
  m_updateDeferred = true;
  if (m_deferredUpdateTimer.active()) {
    return;
  }
  armDeferredUpdate(m_updateTimerGeneration);
}

void PluginWidget::armDeferredUpdate(std::uint64_t generation) {
  m_deferredUpdateTimer.start(kDeferredUpdateRetry, [this, generation] {
    if (m_updateTimerGeneration != generation || !m_updateDeferred) {
      return;
    }
    if (shouldDeferUpdate()) {
      armDeferredUpdate(generation);
      return;
    }

    m_updateDeferred = false;
    runScriptUpdate();
    if (m_updateTimerGeneration == generation) {
      startUpdateTimer();
    }
  });
}

std::chrono::milliseconds PluginWidget::initialUpdateDelay(std::chrono::milliseconds interval) const noexcept {
  if (interval <= std::chrono::milliseconds(1)) {
    return interval;
  }

  const auto maxPhase = std::min({interval / 2, kTimerMaxPhase, interval - std::chrono::milliseconds(1)});
  const auto maxPhaseMs = maxPhase.count();
  if (maxPhaseMs <= 0) {
    return interval;
  }

  const auto phaseMs = (static_cast<std::int64_t>(m_timerPhase) * kTimerPhaseStep.count()) % (maxPhaseMs + 1);
  return interval + std::chrono::milliseconds(phaseMs);
}

void PluginWidget::runScriptUpdate() {
  if (m_runtime) {
    (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
  }
}

void PluginWidget::handleScriptResult(scripting::ScriptResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }

  if (result.unhealthy) {
    m_updateTimer.stop();
    m_deferredUpdateTimer.stop();
    kLog.warn("plugin widget '{}' disabled after repeated timeouts", m_entryId);
  }

  m_dirty = false;
  applyScriptPatch(result.patch);
  if (m_dirty) {
    requestUpdate();
  }
}

void PluginWidget::applyScriptPatch(const scripting::ScriptPatch& patch) {
  if (patch.uiTree.has_value()) {
    applyUiTreePatch(*patch.uiTree);
  }
  const bool imperativeContent = patch.text.has_value()
      || patch.glyph.has_value()
      || patch.image.has_value()
      || patch.fontFamily.has_value()
      || patch.textColor.has_value()
      || patch.glyphColor.has_value();
  if (m_tree.has_value() && imperativeContent && !m_warnedImperativeWhileDeclarative) {
    m_warnedImperativeWhileDeclarative = true;
    kLog.warn(
        "plugin widget '{}': setText/setGlyph/setImage/setFont/setColor have no visible effect while a render() tree "
        "is active",
        m_entryId
    );
  }
  if (patch.fontFamily.has_value()) {
    luaSetFont(*patch.fontFamily, patch.fontBaseline.value_or(std::string{}));
  }
  if (patch.text.has_value()) {
    luaSetText(*patch.text);
  }
  if (patch.glyph.has_value()) {
    luaSetGlyph(*patch.glyph);
  }
  if (patch.image.has_value()) {
    luaSetImage(patch.image->path, patch.image->watch, patch.image->width, patch.image->height);
  }
  if (patch.tooltip.has_value()) {
    luaSetTooltip(*patch.tooltip);
  }
  if (patch.textColor.has_value()) {
    luaSetColor(patch.textColor->role, patch.textColor->mode);
  }
  if (patch.glyphColor.has_value()) {
    luaSetGlyphColor(patch.glyphColor->role, patch.glyphColor->mode);
  }
  if (patch.visible.has_value()) {
    luaSetVisible(*patch.visible);
  }
  if (patch.updateIntervalMs.has_value()) {
    luaSetUpdateInterval(static_cast<float>(*patch.updateIntervalMs));
  }
}

void PluginWidget::applyUiTreePatch(const ui::UiTreeNode& patchTree) {
  if (m_uiHost == nullptr || m_flex == nullptr) {
    return;
  }
  ui::UiTreeNode filtered = patchTree;
  std::vector<std::string> dropped;
  const bool rootAllowed = filterUnsupportedBarControls(filtered, dropped);
  const bool changed = !m_tree.has_value() || filtered != *m_tree;
  if (!dropped.empty() && changed) {
    std::string typeList;
    for (const auto& type : dropped) {
      if (!typeList.empty()) {
        typeList += ", ";
      }
      typeList += type;
    }
    kLog.warn("plugin widget '{}': ui control(s) not supported in bar widgets, skipped: {}", m_entryId, typeList);
  }
  if (!rootAllowed || !changed) {
    return;
  }
  m_tree = std::move(filtered);
  m_flex->setVisible(false);
  m_uiHost->setVisible(true);
  m_dirty = true;
}

scripting::ScriptSnapshot PluginWidget::makeScriptSnapshot() const {
  return scripting::ScriptSnapshot{
      .isVertical = m_isVertical,
      .outputName = m_outputName,
      .barName = m_barName,
      .focusedOutputName = focusedOutputName(),
  };
}

std::string PluginWidget::focusedOutputName() const {
  if (m_platform == nullptr) {
    return {};
  }
  wl_output* output = m_platform->preferredInteractiveOutput();
  const auto* info = m_platform->findOutputByWl(output);
  return info != nullptr ? info->connectorName : std::string{};
}

void PluginWidget::syncImage(Renderer& renderer) {
  if (m_image == nullptr) {
    return;
  }

  if (m_resolvedImagePath.empty()) {
    if (m_imageDirty) {
      m_image->clear(renderer);
      m_imageDirty = false;
      m_imageForceReload = false;
    }
    m_image->setVisible(false);
    return;
  }

  const float logicalWidth = m_imageWidth > 0.0F ? m_imageWidth : Style::baseGlyphSize;
  const float logicalHeight = m_imageHeight > 0.0F ? m_imageHeight : logicalWidth;
  const float imageWidth = logicalWidth * m_contentScale;
  const float imageHeight = logicalHeight * m_contentScale;
  m_image->setSize(imageWidth, imageHeight);

  const int imageTargetSize = std::max(1, static_cast<int>(std::round(std::max(imageWidth, imageHeight) * 3.0F)));
  if (m_imageDirty) {
    const bool loaded = m_imageForceReload
        ? m_image->reloadSourceFile(renderer, m_resolvedImagePath.string(), imageTargetSize, true)
        : m_image->setSourceFile(renderer, m_resolvedImagePath.string(), imageTargetSize, true);
    if (loaded) {
      m_imageDirty = false;
      m_imageForceReload = false;
      m_imageReloadRetries = 0;
      m_imageReloadRetryTimer.stop();
    } else if (m_imageForceReload && m_image->hasImage() && m_imageReloadRetries > 0) {
      scheduleImageReloadRetry();
    } else {
      m_imageDirty = false;
      m_imageForceReload = false;
      m_imageReloadRetries = 0;
    }
  } else {
    (void)m_image->setSourceFile(renderer, m_resolvedImagePath.string(), imageTargetSize, true);
  }

  m_image->setVisible(m_image->hasImage());
}

void PluginWidget::setupImageWatch() {
  teardownImageWatch();
  if (!m_imageWatch || m_resolvedImagePath.empty() || m_fileWatcher == nullptr) {
    return;
  }

  m_imageWatchId = m_fileWatcher->watch(m_resolvedImagePath, [this] { reloadImage(); });
}

void PluginWidget::teardownImageWatch() {
  if (m_imageWatchId == 0 || m_fileWatcher == nullptr) {
    return;
  }
  m_fileWatcher->unwatch(m_imageWatchId);
  m_imageWatchId = 0;
}

void PluginWidget::reloadImage() {
  m_imageDirty = true;
  m_imageForceReload = true;
  m_imageReloadRetries = kImageReloadRetryCount;
  requestUpdate();
}

void PluginWidget::scheduleImageReloadRetry() {
  if (m_imageReloadRetryTimer.active()) {
    return;
  }
  --m_imageReloadRetries;
  m_imageReloadRetryTimer.start(kImageReloadRetry, [this] {
    if (m_imageForceReload) {
      requestUpdate();
    }
  });
}

bool PluginWidget::shouldDeferUpdate() const { return m_updateDeferralCallback && m_updateDeferralCallback(); }

void PluginWidget::setupScriptWatch() {
  m_scriptWatcher.start(m_fileWatcher, m_sourcePath, [this] { reloadScript(); });
}

void PluginWidget::teardownScriptWatch() { m_scriptWatcher.stop(); }

void PluginWidget::reloadScript() {
  std::string source = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (source.empty() || !m_runtime) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }

  m_updateTimer.stop();
  m_imageReloadRetryTimer.stop();
  teardownImageWatch();
  m_glyphVisible = false;
  m_imagePath.clear();
  m_resolvedImagePath.clear();
  m_imageWidth = 0.0F;
  m_imageHeight = 0.0F;
  m_textColor = {};
  m_glyphColor = {};
  m_updateIntervalMs = 250;
  m_imageWatch = false;
  m_imageDirty = true;
  m_imageForceReload = false;
  m_imageReloadRetries = 0;
  if (m_glyph)
    m_glyph->setVisible(false);
  if (m_image)
    m_image->setVisible(false);
  if (m_label) {
    m_label->setText("");
    m_label->setVisible(false);
  }
  m_tree.reset();
  m_warnedImperativeWhileDeclarative = false;
  if (m_uiHost)
    m_uiHost->setVisible(false);
  if (m_flex)
    m_flex->setVisible(true);

  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;

  m_runtime->reload(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
  requestRedraw();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}
