#include "shell/bar/widgets/scripted_widget.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "core/resource_paths.h"
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
#include <cstdint>
#include <cstdlib>
#include <fontconfig/fontconfig.h>
#include <fstream>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace {
  constexpr Logger kLog("scripted-widget");
  constexpr std::chrono::milliseconds kDeferredUpdateRetry{50};
  constexpr std::chrono::milliseconds kTimerPhaseStep{50};
  constexpr std::chrono::milliseconds kTimerMaxPhase{500};

  std::unordered_set<std::string>& registeredFontFiles() {
    static std::unordered_set<std::string> s;
    return s;
  }

  std::string registerFontFile(const std::filesystem::path& path) {
    auto pathStr = path.string();
    const bool firstTime = !registeredFontFiles().contains(pathStr);
    if (firstTime) {
      if (!FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(pathStr.c_str()))) {
        kLog.warn("failed to register font file: {}", pathStr);
        return {};
      }
      registeredFontFiles().insert(pathStr);
    }
    // Extract family name from the font file
    FcPattern* pat = FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(pathStr.c_str()), 0, nullptr, nullptr);
    if (!pat) {
      kLog.warn("failed to query font family from: {}", pathStr);
      return {};
    }
    FcChar8* family = nullptr;
    FcPatternGetString(pat, FC_FAMILY, 0, &family);
    std::string result = family ? reinterpret_cast<const char*>(family) : "";
    FcPatternDestroy(pat);
    return result;
  }

  std::uint32_t nextTimerPhase() {
    static std::atomic<std::uint32_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  std::filesystem::path resolveScriptPath(const std::string& path) {
    if (path.empty())
      return {};
    if (path[0] == '~') {
      const char* home = std::getenv("HOME");
      if (home)
        return std::string(home) + path.substr(1);
      return path;
    }
    if (path[0] == '/')
      return path;
    return paths::assetPath(path);
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

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f)
      return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

} // namespace

ScriptedWidget::ScriptedWidget(
    std::string configName, std::string scriptPath, std::string barName, std::string outputName,
    const WidgetConfig* config, FileWatcher* fileWatcher, CompositorPlatform* platform, ClipboardService* clipboard,
    PipeWireSpectrum* audioSpectrum, MprisService* mpris
)
    : m_scriptPath(std::move(scriptPath)), m_widgetConfigName(std::move(configName)), m_barName(std::move(barName)),
      m_outputName(std::move(outputName)), m_fileWatcher(fileWatcher), m_platform(platform), m_clipboard(clipboard),
      m_audioSpectrum(audioSpectrum), m_mpris(mpris), m_timerPhase(nextTimerPhase()) {
  if (config) {
    m_settings = config->settings;
    m_hotReload = config->getBool("hot_reload", false);
    m_sharedScope = config->getString("scope", "instance") == "shared";
    m_audioSpectrumEnabled = config->getBool("audio_spectrum", false);
    m_audioSpectrumBands =
        static_cast<int>(std::clamp<std::int64_t>(config->getInt("audio_spectrum_bands", 16), 1, 128));
  }
}

ScriptedWidget::~ScriptedWidget() {
  if (m_alive) {
    *m_alive = false;
  }
  teardownAudioSpectrum();
  teardownScriptWatch();
  if (m_runtime != nullptr && m_runtimeSubscription != 0) {
    m_runtime->unsubscribe(m_runtimeSubscription);
  }
  if (m_runtime != nullptr && !m_sharedScope) {
    m_runtime->stop();
  }
}

void ScriptedWidget::create() {
  auto area = std::make_unique<InputArea>();
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

  auto flex = ui::row({
      .out = &m_flex,
      .align = FlexAlign::Center,
      .gap = Style::spaceXs,
  });

  flex->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .visible = false,
      })
  );

  flex->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .visible = false,
      })
  );

  area->addChild(std::move(flex));
  m_area = area.get();
  setRoot(std::move(area));

  if (m_scriptPath.empty()) {
    kLog.warn("scripted widget: no script path");
    return;
  }
  m_resolvedPath = resolveScriptPath(m_scriptPath);
  std::string source = readFile(m_resolvedPath);
  if (source.empty()) {
    kLog.warn("scripted widget: failed to read '{}'", m_resolvedPath.string());
    return;
  }

  bool createdRuntime = true;
  if (m_sharedScope) {
    auto acquired = scripting::SharedScriptRuntimeRegistry::acquire(m_widgetConfigName, m_settings, m_clipboard);
    m_runtime = std::move(acquired.runtime);
    createdRuntime = acquired.created;
  } else {
    m_runtime = std::make_shared<scripting::ScriptRuntime>(
        m_widgetConfigName + ":" + m_barName + ":" + m_outputName, m_settings, m_clipboard
    );
  }

  auto alive = std::weak_ptr<bool>(m_alive);
  m_runtimeSubscription = m_runtime->subscribe([this, alive](scripting::ScriptWidgetResult result) {
    auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    handleScriptResult(std::move(result));
  });

  if (createdRuntime) {
    m_runtime->start(m_resolvedPath.string(), std::move(source), makeScriptSnapshot());
  }
  startUpdateTimer();
  setupAudioSpectrum();

  if (m_hotReload)
    setupScriptWatch();
}

void ScriptedWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  m_isVertical = containerHeight > containerWidth;
  if (!m_flex)
    return;

  if (m_fontConfigDirty) {
    renderer.notifyFontConfigChanged();
    m_fontConfigDirty = false;
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

  m_flex->layout(renderer);

  if (m_area)
    m_area->setSize(m_flex->width(), m_flex->height());
}

void ScriptedWidget::doUpdate(Renderer&) {}

void ScriptedWidget::luaSetText(std::string_view text) {
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

void ScriptedWidget::luaSetGlyph(std::string_view name) {
  if (!m_glyph)
    return;
  bool changed = m_glyph->setGlyph(name);
  if (!m_glyphVisible) {
    m_glyph->setVisible(true);
    m_glyphVisible = true;
    changed = true;
  }
  m_dirty |= changed;
}

void ScriptedWidget::luaSetFont(std::string_view familyOrPath) {
  if (!m_label)
    return;
  std::string family;
  // If it looks like a font file path, resolve and register it
  if (familyOrPath.ends_with(".otf") || familyOrPath.ends_with(".ttf") || familyOrPath.ends_with(".woff2")) {
    auto resolved = resolveScriptPath(std::string(familyOrPath));
    bool alreadyRegistered = registeredFontFiles().contains(resolved.string());
    family = registerFontFile(resolved);
    if (family.empty())
      return;
    if (!alreadyRegistered) {
      m_fontConfigDirty = true;
    }
  } else {
    family = std::string(familyOrPath);
  }
  m_label->setFontFamily(std::move(family));
  m_dirty = true;
}

void ScriptedWidget::luaSetColor(std::string_view role, std::string_view mode) {
  ScriptColorState next{.color = scriptColorFromToken(role), .mode = scriptColorModeFromToken(mode)};
  if (!next.color.has_value()) {
    next.mode = ScriptColorMode::Auto;
  }
  if (next != m_textColor) {
    m_textColor = next;
    m_dirty = true;
  }
}

void ScriptedWidget::luaSetGlyphColor(std::string_view role, std::string_view mode) {
  ScriptColorState next{.color = scriptColorFromToken(role), .mode = scriptColorModeFromToken(mode)};
  if (!next.color.has_value()) {
    next.mode = ScriptColorMode::Auto;
  }
  if (next != m_glyphColor) {
    m_glyphColor = next;
    m_dirty = true;
  }
}

void ScriptedWidget::luaSetUpdateInterval(float ms) {
  m_updateIntervalMs = std::max(16, static_cast<int>(ms));
  startUpdateTimer();
}

void ScriptedWidget::setUpdateDeferralCallback(std::function<bool()> callback) {
  m_updateDeferralCallback = std::move(callback);
}

void ScriptedWidget::luaSetVisible(bool visible) {
  auto* node = root();
  if (!node || node->visible() == visible)
    return;
  node->setVisible(visible);
  m_dirty = true;
}

ScriptedWidget::IpcDispatchResult ScriptedWidget::dispatchIpcEvent(std::string_view event, std::string_view payload) {
  if (!m_runtime) {
    return IpcDispatchResult::MissingHost;
  }
  if (m_hasOnIpcKnown && !m_hasOnIpc) {
    return IpcDispatchResult::MissingCallback;
  }
  if (!m_runtime->enqueueCallStrings("onIpc", std::string(event), std::string(payload), makeScriptSnapshot())) {
    return IpcDispatchResult::Failed;
  }
  return IpcDispatchResult::Handled;
}

ColorSpec ScriptedWidget::resolveScriptColor(const ScriptColorState& state) const noexcept {
  if (m_widgetForeground.has_value()) {
    return *m_widgetForeground;
  }
  const ColorSpec fallback = colorSpecFromRole(ColorRole::OnSurface);
  if (!state.color.has_value()) {
    return widgetForegroundOr(fallback);
  }
  if (!state.color->role.has_value() || state.mode == ScriptColorMode::Script ||
      *state.color->role != ColorRole::OnSurface) {
    return *state.color;
  }
  return widgetForegroundOr(fallback);
}

ScriptedWidget::ScriptColorMode ScriptedWidget::scriptColorModeFromToken(std::string_view token) noexcept {
  return token == "script" ? ScriptColorMode::Script : ScriptColorMode::Auto;
}

std::optional<ColorSpec> ScriptedWidget::scriptColorFromToken(std::string_view token) noexcept {
  if (auto role = colorRoleFromToken(token); role.has_value()) {
    return colorSpecFromRole(*role);
  }
  Color fixed;
  if (tryParseHexColor(token, fixed)) {
    return fixedColorSpec(fixed);
  }
  return std::nullopt;
}

void ScriptedWidget::startUpdateTimer() {
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

void ScriptedWidget::handleUpdateTimer() {
  if (shouldDeferUpdate()) {
    scheduleDeferredUpdate();
    return;
  }
  runScriptUpdate();
}

void ScriptedWidget::scheduleDeferredUpdate() {
  m_updateDeferred = true;
  if (m_deferredUpdateTimer.active()) {
    return;
  }
  armDeferredUpdate(m_updateTimerGeneration);
}

void ScriptedWidget::armDeferredUpdate(std::uint64_t generation) {
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

std::chrono::milliseconds ScriptedWidget::initialUpdateDelay(std::chrono::milliseconds interval) const noexcept {
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

void ScriptedWidget::runScriptUpdate() {
  if (m_runtime) {
    (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
  }
}

void ScriptedWidget::handleScriptResult(scripting::ScriptWidgetResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }

  if (result.unhealthy) {
    m_updateTimer.stop();
    m_deferredUpdateTimer.stop();
    kLog.warn("scripted widget '{}' disabled after repeated timeouts", m_widgetConfigName);
  }

  m_dirty = false;
  applyScriptPatch(result.patch);
  if (m_dirty) {
    requestUpdate();
  }
}

void ScriptedWidget::applyScriptPatch(const scripting::ScriptWidgetPatch& patch) {
  if (patch.fontFamily.has_value()) {
    luaSetFont(*patch.fontFamily);
  }
  if (patch.text.has_value()) {
    luaSetText(*patch.text);
  }
  if (patch.glyph.has_value()) {
    luaSetGlyph(*patch.glyph);
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

scripting::ScriptWidgetSnapshot ScriptedWidget::makeScriptSnapshot() const {
  return scripting::ScriptWidgetSnapshot{
      .isVertical = m_isVertical,
      .outputName = m_outputName,
      .barName = m_barName,
      .focusedOutputName = focusedOutputName(),
  };
}

std::string ScriptedWidget::focusedOutputName() const {
  if (m_platform == nullptr) {
    return {};
  }
  wl_output* output = m_platform->preferredInteractiveOutput();
  const auto* info = m_platform->findOutputByWl(output);
  return info != nullptr ? info->connectorName : std::string{};
}

void ScriptedWidget::setupAudioSpectrum() {
  if (!m_audioSpectrumEnabled || m_audioSpectrumListenerId != 0) {
    return;
  }
  if (m_audioSpectrum == nullptr) {
    kLog.warn("scripted widget '{}': audio_spectrum requested but PipeWireSpectrum is unavailable", m_widgetConfigName);
    return;
  }
  m_audioSpectrumListenerId =
      m_audioSpectrum->addChangeListener(m_audioSpectrumBands, [this]() { handleAudioSpectrumChanged(); });
}

void ScriptedWidget::teardownAudioSpectrum() {
  if (m_audioSpectrum != nullptr && m_audioSpectrumListenerId != 0) {
    m_audioSpectrum->removeChangeListener(m_audioSpectrumListenerId);
  }
  m_audioSpectrumListenerId = 0;
}

void ScriptedWidget::handleAudioSpectrumChanged() {
  if (m_runtime == nullptr || m_audioSpectrum == nullptr || m_audioSpectrumListenerId == 0) {
    return;
  }
  const bool audioActive = !m_audioSpectrum->idle();
  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  const bool mprisPlaying = active.has_value() && active->playbackStatus == "Playing";
  const std::string state = std::string(audioActive ? "1" : "0") + "," + (mprisPlaying ? "1" : "0");
  (void)m_runtime->enqueueCallStrings(
      "onAudioSpectrum", joinSpectrumValues(m_audioSpectrum->values(m_audioSpectrumListenerId)), state,
      makeScriptSnapshot()
  );
}

bool ScriptedWidget::shouldDeferUpdate() const { return m_updateDeferralCallback && m_updateDeferralCallback(); }

void ScriptedWidget::setupScriptWatch() {
  if (m_resolvedPath.empty() || !m_fileWatcher)
    return;
  m_watchId = m_fileWatcher->watch(m_resolvedPath, [this] { reloadScript(); });
}

void ScriptedWidget::teardownScriptWatch() {
  if (m_watchId == 0 || !m_fileWatcher)
    return;
  m_fileWatcher->unwatch(m_watchId);
  m_watchId = 0;
}

void ScriptedWidget::reloadScript() {
  m_updateTimer.stop();
  m_glyphVisible = false;
  m_textColor = {};
  m_glyphColor = {};
  m_updateIntervalMs = 250;
  if (m_glyph)
    m_glyph->setVisible(false);
  if (m_label) {
    m_label->setText("");
    m_label->setVisible(false);
  }

  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;

  std::string source = readFile(m_resolvedPath);
  auto name = m_resolvedPath.filename().string();
  if (source.empty() || !m_runtime) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    requestRedraw();
    return;
  }

  m_runtime->reload(m_resolvedPath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
  requestRedraw();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}
