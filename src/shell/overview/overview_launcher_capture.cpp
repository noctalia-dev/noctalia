#include "shell/overview/overview_launcher_capture.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_runtime.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "shell/panel/panel_manager.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <nlohmann/json.hpp>
#include <string>

namespace {

  constexpr Logger kLog("overview-launcher");

  [[nodiscard]] std::string utf32ToUtf8(std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
      return std::string(1, static_cast<char>(codepoint));
    }
    if (codepoint <= 0x7FF) {
      std::string out(2, '\0');
      out[0] = static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
      out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
      return out;
    }
    if (codepoint <= 0xFFFF) {
      std::string out(3, '\0');
      out[0] = static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
      out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
      return out;
    }
    if (codepoint <= 0x10FFFF) {
      std::string out(4, '\0');
      out[0] = static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
      out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
      out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
      return out;
    }
    return {};
  }

  [[nodiscard]] bool isLauncherTypingKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0) {
      return false;
    }
    return event.utf32 > 0x20U && event.utf32 != 0x7FU;
  }

} // namespace

bool OverviewLauncherCapture::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, CompositorPlatform& platform, PanelManager& panelManager
) {
  if (!compositors::isNiri() || !platform.tracksOverviewState()) {
    return true;
  }

  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_platform = &platform;
  m_panelManager = &panelManager;
  return true;
}

void OverviewLauncherCapture::setOpenLauncherCallback(OpenLauncherCallback callback) {
  m_openLauncher = std::move(callback);
}

void OverviewLauncherCapture::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  sync();
}

bool OverviewLauncherCapture::shouldBeActive() const {
  if (!m_enabled) {
    return false;
  }
  if (m_platform == nullptr || m_panelManager == nullptr) {
    return false;
  }
  if (!m_platform->hasOverviewState() || !m_platform->isOverviewOpen()) {
    return false;
  }
  if (m_panelManager->isOpen() || m_panelManager->isPanelTransitionActive()) {
    return false;
  }
  return true;
}

bool OverviewLauncherCapture::surfacesMatchOutputs() const {
  if (m_wayland == nullptr) {
    return m_instances.empty();
  }
  const auto& outputs = m_wayland->outputs();
  if (m_instances.size() != outputs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    const auto* instance = m_instances[i].get();
    if (instance == nullptr || instance->output != outputs[i].output || instance->surface == nullptr) {
      return false;
    }
  }
  return true;
}

void OverviewLauncherCapture::sync() {
  if (!shouldBeActive()) {
    destroySurfaces();
    return;
  }
  if (!m_instances.empty() && surfacesMatchOutputs()) {
    return;
  }
  destroySurfaces();
  ensureSurfaces();
}

void OverviewLauncherCapture::onOutputChange() { sync(); }

void OverviewLauncherCapture::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr || !m_instances.empty()) {
    return;
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }

    auto config = LayerSurfaceConfig{
        .nameSpace = "noctalia-overview-launcher",
        .layer = LayerShellLayer::Overlay,
        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Left,
        .width = 1,
        .height = 1,
        .exclusiveZone = -1,
        .keyboard = LayerShellKeyboard::Exclusive,
        .defaultWidth = 1,
        .defaultHeight = 1,
    };

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
    inst->surface->setRenderContext(m_renderContext);
    inst->surface->setConfigureCallback([](std::uint32_t /*width*/, std::uint32_t /*height*/) {});
    inst->surface->setClickThrough(true);

    if (!inst->surface->initialize(output.output)) {
      kLog.warn("failed to initialize overview type-to-launch capture surface on {}", output.connectorName);
      continue;
    }

    m_instances.push_back(std::move(inst));
  }
}

void OverviewLauncherCapture::destroySurfaces() { m_instances.clear(); }

OverviewLauncherCapture::Instance* OverviewLauncherCapture::instanceForKeyboardSurface(wl_surface* surface) noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  for (auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instance->surface->wlSurface() == surface) {
      return instance.get();
    }
  }
  return nullptr;
}

bool OverviewLauncherCapture::sendNiriAction(const char* actionName) const {
  if (m_platform == nullptr || actionName == nullptr || actionName[0] == '\0') {
    return false;
  }
  return m_platform->niriRuntime().requestAction(nlohmann::json{{actionName, nlohmann::json::object()}});
}

bool OverviewLauncherCapture::handleNiriOverviewKey(const KeyboardEvent& event) const {
  if (!event.pressed || event.preedit) {
    return false;
  }

  if (KeySymbol::isEnter(event.sym) || KeySymbol::isEscape(event.sym)) {
    (void)sendNiriAction("ToggleOverview");
    return true;
  }
  if (KeySymbol::isLeft(event.sym)) {
    (void)sendNiriAction("FocusColumnLeft");
    return true;
  }
  if (KeySymbol::isRight(event.sym)) {
    (void)sendNiriAction("FocusColumnRight");
    return true;
  }
  if (KeySymbol::isUp(event.sym)) {
    (void)sendNiriAction("FocusWorkspaceUp");
    return true;
  }
  if (KeySymbol::isDown(event.sym)) {
    (void)sendNiriAction("FocusWorkspaceDown");
    return true;
  }

  return false;
}

bool OverviewLauncherCapture::handleKeyboardEvent(const KeyboardEvent& event) {
  if (m_panelManager != nullptr && (m_panelManager->isOpen() || m_panelManager->isPanelTransitionActive())) {
    if (!m_instances.empty()) {
      destroySurfaces();
    }
    return false;
  }

  if (m_instances.empty() || m_wayland == nullptr) {
    return false;
  }

  Instance* const instance = instanceForKeyboardSurface(m_wayland->lastKeyboardSurface());
  if (instance == nullptr) {
    return false;
  }

  if (handleNiriOverviewKey(event)) {
    return true;
  }

  if (!isLauncherTypingKey(event) || !m_openLauncher) {
    return true;
  }

  const std::string initialQuery = utf32ToUtf8(event.utf32);
  if (initialQuery.empty()) {
    return true;
  }

  m_openLauncher(initialQuery, instance->output, {});
  destroySurfaces();
  return true;
}
