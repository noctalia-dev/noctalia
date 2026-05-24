#include "shell/session/session_panel.h"

#include "compositors/compositor_detect.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/triad/triad_runtime.h"
#include "config/config_service.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <json.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

  constexpr Logger kLog("session");
  constexpr std::chrono::milliseconds kPowerCommandTimeout{5000};

  [[nodiscard]] const char* valueOrUnset(const char* value) {
    return value != nullptr && value[0] != '\0' ? value : "<unset>";
  }

  compositors::CompositorKind logActionContext(std::string_view action) {
    const compositors::CompositorKind compositor = compositors::detect();
    kLog.info(
        "{} requested: compositor={} env_hint=\"{}\" xdg_session_id={} user={}", action, compositors::name(compositor),
        compositors::envHint(), valueOrUnset(std::getenv("XDG_SESSION_ID")), valueOrUnset(std::getenv("USER"))
    );
    return compositor;
  }

  void logLabwcExitFailure(std::string_view command, const process::RunResult& result) {
    if (!result.err.empty()) {
      kLog.warn("logout: {} failed with code {}: {}", command, result.exitCode, result.err);
    } else if (!result.out.empty()) {
      kLog.warn("logout: {} failed with code {}: {}", command, result.exitCode, result.out);
    } else {
      kLog.warn("logout: {} failed with code {}", command, result.exitCode);
    }
  }

  [[nodiscard]] std::string commandLabel(std::initializer_list<const char*> args) {
    std::string label;
    for (const char* arg : args) {
      if (arg == nullptr) {
        continue;
      }
      if (!label.empty()) {
        label += ' ';
      }
      label += arg;
    }
    return label.empty() ? "<empty>" : label;
  }

  void
  logSessionCommandFailure(std::string_view action, std::string_view commandLabel, const process::RunResult& result) {
    if (result.timedOut) {
      kLog.warn("{}: {} timed out after {}ms", action, commandLabel, kPowerCommandTimeout.count());
    } else if (!result.err.empty()) {
      kLog.warn("{}: {} failed with code {}: {}", action, commandLabel, result.exitCode, result.err);
    } else if (!result.out.empty()) {
      kLog.warn("{}: {} failed with code {}: {}", action, commandLabel, result.exitCode, result.out);
    } else {
      kLog.warn("{}: {} failed with code {}", action, commandLabel, result.exitCode);
    }
  }

  [[nodiscard]] bool runCheckedSessionCommand(
      std::string_view action, std::initializer_list<std::initializer_list<const char*>> commands
  ) {
    bool attempted = false;
    for (const auto& command : commands) {
      if (command.size() == 0) {
        continue;
      }
      const char* executable = *command.begin();
      if (executable == nullptr || executable[0] == '\0') {
        continue;
      }
      if (!process::commandExists(executable)) {
        kLog.debug("{}: {} not found", action, executable);
        continue;
      }

      attempted = true;
      const std::string label = commandLabel(command);
      const process::RunResult result = process::runSyncWithTimeout(command, kPowerCommandTimeout);
      if (result) {
        kLog.info("{}: {} accepted", action, label);
        return true;
      }
      logSessionCommandFailure(action, label, result);
    }

    if (!attempted) {
      kLog.warn("{}: no supported command found", action);
    } else {
      kLog.warn("{}: all command methods failed", action);
    }
    return false;
  }

  bool terminateLabwcPid() {
    const char* pidEnv = std::getenv("LABWC_PID");
    if (pidEnv == nullptr || pidEnv[0] == '\0') {
      kLog.warn("logout: LABWC_PID is not set");
      return false;
    }

    errno = 0;
    char* end = nullptr;
    const long pid = std::strtol(pidEnv, &end, 10);
    if (errno != 0 || end == pidEnv || (end != nullptr && *end != '\0') || pid <= 1) {
      kLog.warn("logout: LABWC_PID has invalid value \"{}\"", pidEnv);
      return false;
    }

    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
      kLog.warn("logout: failed to terminate LABWC_PID={}", pidEnv);
      return false;
    }
    return true;
  }

  bool doLabwcLogout() {
    if (process::commandExists("labwc")) {
      const process::RunResult longResult = process::runSync({"labwc", "--exit"});
      if (longResult) {
        return true;
      }
      logLabwcExitFailure("labwc --exit", longResult);

      const process::RunResult shortResult = process::runSync({"labwc", "-e"});
      if (shortResult) {
        return true;
      }
      logLabwcExitFailure("labwc -e", shortResult);
    } else {
      kLog.warn("logout: labwc executable not found");
    }

    return terminateLabwcPid();
  }

  bool doLogout(compositors::niri::NiriRuntime* niriRuntime) {
    const compositors::CompositorKind compositor = logActionContext("logout");

    switch (compositor) {
    case compositors::CompositorKind::Hyprland: {
      compositors::hyprland::HyprlandRuntime runtime;
      if (runtime.configIsLua()) {
        return (runtime.request("dispatch hl.dsp.exit()") != std::nullopt);
      } else {
        return (runtime.request("dispatch exit") != std::nullopt);
      }
    }
    case compositors::CompositorKind::Sway:
      return process::launchFirstAvailable({{"swaymsg", "exit"}, {"i3-msg", "exit"}});
    case compositors::CompositorKind::Niri: {
      compositors::niri::NiriRuntime scratch;
      compositors::niri::NiriRuntime& runtime = niriRuntime != nullptr ? *niriRuntime : scratch;
      return runtime.requestAction(nlohmann::json{{"Quit", nlohmann::json{{"skip_confirmation", true}}}}, true);
    }
    case compositors::CompositorKind::Triad: {
      compositors::triad::TriadRuntime runtime;
      return runtime.requestAction("exit-session");
    }
    case compositors::CompositorKind::Mango:
      return process::launchFirstAvailable({{"mmsg", "-q"}});
    case compositors::CompositorKind::Labwc:
      if (doLabwcLogout()) {
        return true;
      }
      break;
    case compositors::CompositorKind::Unknown:
      break;
    }

    if (const char* sessionId = std::getenv("XDG_SESSION_ID"); sessionId != nullptr && sessionId[0] != '\0') {
      if (process::launchFirstAvailable({{"loginctl", "terminate-session", sessionId}})) {
        return true;
      }
    }
    if (process::launchFirstAvailable({{"systemctl", "--user", "stop", "graphical-session.target"}})) {
      return true;
    }
    if (const char* user = std::getenv("USER"); user != nullptr && user[0] != '\0') {
      if (process::launchFirstAvailable({{"loginctl", "terminate-user", user}})) {
        return true;
      }
    }
    return false;
  }

  bool doSuspend() {
    logActionContext("suspend");
    return runCheckedSessionCommand(
        "suspend", {
                       {"systemctl", "suspend"},
                       {"loginctl", "suspend"},
                   }
    );
  }

  bool doReboot() {
    logActionContext("reboot");
    return runCheckedSessionCommand(
        "reboot", {
                      {"systemctl", "reboot"},
                      {"loginctl", "reboot"},
                      {"reboot"},
                      {"/sbin/reboot"},
                      {"/usr/sbin/reboot"},
                  }
    );
  }

  bool doShutdown() {
    logActionContext("shutdown");
    return runCheckedSessionCommand(
        "shutdown", {
                        {"systemctl", "poweroff"},
                        {"loginctl", "poweroff"},
                        {"poweroff"},
                        {"/sbin/poweroff"},
                        {"/usr/sbin/poweroff"},
                    }
    );
  }

  bool doLock() {
    logActionContext("lock");
    LockScreen* lockScreen = LockScreen::instance();
    if (lockScreen == nullptr) {
      kLog.warn("lock: lock screen service unavailable");
      return false;
    }
    if (!lockScreen->lock()) {
      kLog.warn("lock: lock screen request failed");
      return false;
    }
    kLog.info("lock: lock screen requested");
    return true;
  }

  void runPowerAction(std::function<bool()> hook, std::function<bool()> action, std::string_view actionName) {
    std::thread([hook = std::move(hook), action = std::move(action), actionName = std::string(actionName)]() mutable {
      if (hook && !hook()) {
        kLog.warn("{} cancelled because a configured hook failed", actionName);
        return;
      }
      if (!action()) {
        kLog.warn("{} failed after hooks completed", actionName);
      }
    }).detach();
  }

  void runShellCommand(std::function<bool()> hook, std::string command, std::string_view actionName) {
    std::thread([hook = std::move(hook), command = std::move(command), actionName = std::string(actionName)]() mutable {
      if (hook && !hook()) {
        kLog.warn("{} cancelled because a configured hook failed", actionName);
        return;
      }
      if (!process::runAsync(command)) {
        kLog.warn("{}: command failed", actionName);
      }
    }).detach();
  }

  [[nodiscard]] bool isKnownAction(std::string_view action) {
    return action == "lock" || action == "logout" || action == "suspend" || action == "reboot" ||
           action == "shutdown" || action == "command";
  }

  [[nodiscard]] const char* labelKeyForAction(std::string_view action) {
    if (action == "lock") {
      return "session.actions.lock";
    }
    if (action == "logout") {
      return "session.actions.logout";
    }
    if (action == "suspend") {
      return "session.actions.suspend";
    }
    if (action == "reboot") {
      return "session.actions.reboot";
    }
    if (action == "shutdown") {
      return "session.actions.shutdown";
    }
    return "session.actions.custom";
  }

  [[nodiscard]] const char* defaultGlyphForAction(std::string_view action) {
    if (action == "lock") {
      return "lock";
    }
    if (action == "logout") {
      return "logout";
    }
    if (action == "suspend") {
      return "suspend";
    }
    if (action == "reboot") {
      return "reboot";
    }
    if (action == "shutdown") {
      return "shutdown";
    }
    return "terminal";
  }

  [[nodiscard]] ButtonVariant buttonVariantFor(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
      return ButtonVariant::Default;
    case SessionActionButtonVariant::Primary:
      return ButtonVariant::Primary;
    case SessionActionButtonVariant::Secondary:
      return ButtonVariant::Secondary;
    case SessionActionButtonVariant::Destructive:
      return ButtonVariant::Destructive;
    case SessionActionButtonVariant::Outline:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

  [[nodiscard]] Button::ButtonPalette actionButtonPalette(const SessionPanelActionConfig& cfg, float fillOpacity) {
    constexpr float kDisabledAlpha = 0.55f;
    const float opacity = std::clamp(fillOpacity, 0.0f, 1.0f);

    switch (cfg.variant) {
    case SessionActionButtonVariant::Primary:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Primary, opacity),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnPrimary),
              },
          .hover =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnHover),
              },
          .pressed =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Primary),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnPrimary),
              },
          .disabled = Button::ButtonStateColors{
              .bg = colorSpecFromRole(ColorRole::Primary, opacity * kDisabledAlpha),
              .border = clearColorSpec(),
              .label = colorSpecFromRole(ColorRole::OnPrimary, kDisabledAlpha),
          },
      };
    case SessionActionButtonVariant::Secondary:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Secondary, opacity),
                  .border = colorSpecFromRole(ColorRole::Outline, 0.5f),
                  .label = colorSpecFromRole(ColorRole::OnSecondary),
              },
          .hover =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnHover),
              },
          .pressed =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Primary),
                  .border = colorSpecFromRole(ColorRole::Primary),
                  .label = colorSpecFromRole(ColorRole::OnPrimary),
              },
          .disabled = Button::ButtonStateColors{
              .bg = colorSpecFromRole(ColorRole::Secondary, opacity * kDisabledAlpha),
              .border = colorSpecFromRole(ColorRole::Outline, 0.5f * kDisabledAlpha),
              .label = colorSpecFromRole(ColorRole::OnSecondary),
          },
      };
    case SessionActionButtonVariant::Destructive:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Error, opacity),
                  .border = colorSpecFromRole(ColorRole::Error, 0.5f),
                  .label = colorSpecFromRole(ColorRole::OnError),
              },
          .hover =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Error, std::max(opacity, 0.78f)),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnError),
              },
          .pressed =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Error),
                  .border = colorSpecFromRole(ColorRole::Error),
                  .label = colorSpecFromRole(ColorRole::OnError),
              },
          .disabled = Button::ButtonStateColors{
              .bg = colorSpecFromRole(ColorRole::Error, opacity * kDisabledAlpha),
              .border = colorSpecFromRole(ColorRole::Error, 0.5f * kDisabledAlpha),
              .label = colorSpecFromRole(ColorRole::OnError, kDisabledAlpha),
          },
      };
    case SessionActionButtonVariant::Outline:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Surface, opacity),
                  .border = colorSpecFromRole(ColorRole::Outline, 0.5f),
                  .label = colorSpecFromRole(ColorRole::OnSurface),
              },
          .hover =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnHover),
              },
          .pressed =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Primary),
                  .border = colorSpecFromRole(ColorRole::Primary),
                  .label = colorSpecFromRole(ColorRole::OnPrimary),
              },
          .disabled = Button::ButtonStateColors{
              .bg = colorSpecFromRole(ColorRole::Surface, opacity * kDisabledAlpha),
              .border = colorSpecFromRole(ColorRole::Outline, 0.5f * kDisabledAlpha),
              .label = colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha),
          },
      };
    case SessionActionButtonVariant::Ghost:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal =
              Button::ButtonStateColors{
                  .bg = clearColorSpec(),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnSurface),
              },
          .hover =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnHover),
              },
          .pressed =
              Button::ButtonStateColors{
                  .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity),
                  .border = clearColorSpec(),
                  .label = colorSpecFromRole(ColorRole::OnSurface),
              },
          .disabled = Button::ButtonStateColors{
              .bg = clearColorSpec(),
              .border = clearColorSpec(),
              .label = colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha),
          },
      };
    case SessionActionButtonVariant::Default:
      break;
    }

    return Button::ButtonPalette{
        .borderWidth = Style::borderWidth,
        .normal =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity),
                .border = colorSpecFromRole(ColorRole::Outline, 0.5f),
                .label = colorSpecFromRole(ColorRole::OnSurface),
            },
        .hover =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                .border = clearColorSpec(),
                .label = colorSpecFromRole(ColorRole::OnHover),
            },
        .pressed =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Primary),
                .border = colorSpecFromRole(ColorRole::Primary),
                .label = colorSpecFromRole(ColorRole::OnPrimary),
            },
        .disabled = Button::ButtonStateColors{
            .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity * kDisabledAlpha),
            .border = colorSpecFromRole(ColorRole::Outline, 0.5f * kDisabledAlpha),
            .label = colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha),
        },
    };
  }

  void applyActionButtonPalette(Button& button, const SessionPanelActionConfig& cfg, float fillOpacity) {
    button.setVariant(buttonVariantFor(cfg.variant));
    button.setCustomPalette(actionButtonPalette(cfg, fillOpacity));
  }

} // namespace

std::vector<SessionPanelActionConfig> SessionPanel::effectiveActions() const {
  std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();

  std::vector<SessionPanelActionConfig> out;
  out.reserve(src.size());
  for (const auto& row : src) {
    if (!row.enabled) {
      continue;
    }
    if (!isKnownAction(row.action)) {
      kLog.warn("session panel: skipping unknown action \"{}\"", row.action);
      continue;
    }
    if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
      kLog.warn("session panel: skipping \"command\" entry with no command");
      continue;
    }
    out.push_back(row);
  }
  return out;
}

std::function<bool()> SessionPanel::hookFor(const std::string& action) const {
  if (action == "logout") {
    return m_actionHooks.onLogout;
  }
  if (action == "reboot") {
    return m_actionHooks.onReboot;
  }
  if (action == "shutdown") {
    return m_actionHooks.onShutdown;
  }
  return {};
}

PanelPlacement SessionPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.sessionPlacement : PanelPlacement::Attached;
}

float SessionPanel::preferredWidth() const {
  const std::size_t n = visibleColumnCount();
  const float gap = Style::spaceSm;
  const float w = kButtonMinWidth * static_cast<float>(n) + gap * static_cast<float>(n > 1 ? n - 1 : 0) +
                  Style::panelPadding * 2.0f;
  return scaled(std::max(kPanelMinWidth, w));
}

float SessionPanel::preferredHeight() const {
  const std::size_t rows = visibleRowCount();
  const float gap = Style::spaceSm;
  const float h = kActionButtonMinHeight * static_cast<float>(rows) +
                  gap * static_cast<float>(rows > 1 ? rows - 1 : 0) + Style::panelPadding * 2.0f;
  return std::ceil(scaled(h));
}

std::size_t SessionPanel::entryCountForLayout() const {
  if (!m_visibleEntries.empty()) {
    return m_visibleEntries.size();
  }
  return effectiveActions().size();
}

std::size_t SessionPanel::visibleColumnCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  if (n <= kMaxColumns) {
    return n;
  }
  return std::min<std::size_t>(kMaxColumns, (n + 1) / 2);
}

std::size_t SessionPanel::visibleRowCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  const std::size_t columns = visibleColumnCount();
  return (n + columns - 1) / columns;
}

void SessionPanel::create() {
  const float scale = contentScale();
  m_visibleEntries = effectiveActions();
  const std::size_t columns = visibleColumnCount();

  auto rootLayout = std::make_unique<GridView>();
  rootLayout->setColumns(columns);
  rootLayout->setColumnGap(Style::spaceSm * scale);
  rootLayout->setRowGap(Style::spaceSm * scale);
  rootLayout->setStretchItems(true);
  rootLayout->setUniformCellSize(true);
  rootLayout->setMinCellWidth(kButtonMinWidth * scale);
  rootLayout->setMinCellHeight(kActionButtonMinHeight * scale);
  m_rootLayout = rootLayout.get();

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  focusArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (key.pressed) {
      handleKeyEvent(key.sym, key.modifiers);
    }
  });
  m_focusArea = static_cast<InputArea*>(rootLayout->addChild(std::move(focusArea)));

  m_visibleButtons.clear();
  m_visibleButtons.reserve(m_visibleEntries.size());
  for (const auto& cfg : m_visibleEntries) {
    if (Button* b = createActionButton(cfg, scale); b != nullptr) {
      m_visibleButtons.push_back(b);
      rootLayout->addChild(std::unique_ptr<Button>(b));
    }
  }

  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  updateSelectionVisuals();
}

Button* SessionPanel::createActionButton(const SessionPanelActionConfig& cfg, float scale) {
  auto button = std::make_unique<Button>();
  const std::string labelText =
      cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(labelKeyForAction(cfg.action));
  button->setText(labelText);
  if (cfg.shortcut.has_value() && cfg.shortcut->sym != 0) {
    button->setBadge(keyChordDisplayLabel(*cfg.shortcut));
  }
  button->setGlyph(cfg.glyph.has_value() && !cfg.glyph->empty() ? *cfg.glyph : defaultGlyphForAction(cfg.action));
  applyActionButtonPalette(*button, cfg, panelCardOpacity());
  button->setDirection(FlexDirection::Vertical);
  button->setAlign(FlexAlign::Center);
  button->setJustify(FlexJustify::Center);
  button->setGap(Style::spaceSm * scale);
  button->setContentAlign(ButtonContentAlign::Center);
  button->setFontSize((Style::fontSizeBody + 1.0f) * scale);
  button->setGlyphSize(28.0f * scale);
  button->setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
  button->setRadius(Style::scaledRadiusLg(scale));
  button->setMinWidth(kButtonMinWidth * scale);
  button->setMinHeight(kActionButtonMinHeight * scale);
  button->setFlexGrow(1.0f);

  SessionPanelActionConfig cfgCopy = cfg;
  button->setOnClick([this, cfgCopy]() {
    PanelManager::instance().close();
    invokeEntry(cfgCopy);
  });
  button->setOnMotion([this]() { activateMouse(); });
  button->setHoverSuppressed(!m_mouseActive);

  return button.release();
}

void SessionPanel::onPanelCardOpacityChanged(float opacity) {
  const std::size_t count = std::min(m_visibleButtons.size(), m_visibleEntries.size());
  for (std::size_t i = 0; i < count; ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    applyActionButtonPalette(*button, m_visibleEntries[i], opacity);
  }
}

InputArea* SessionPanel::initialFocusArea() const { return m_focusArea; }

void SessionPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex.reset();
  m_mouseActive = false;
  updateSelectionVisuals();
}

void SessionPanel::activateMouse() {
  if (m_mouseActive) {
    return;
  }
  m_mouseActive = true;
  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->setHoverSuppressed(false);
    }
  }
  PanelManager::instance().refresh();
}

void SessionPanel::activateSelected() {
  if (!m_selectedIndex.has_value() || m_visibleButtons.empty()) {
    return;
  }
  const std::size_t i = *m_selectedIndex;
  if (i >= m_visibleButtons.size() || i >= m_visibleEntries.size()) {
    return;
  }
  Button* button = m_visibleButtons[i];
  if (button != nullptr && button->enabled()) {
    PanelManager::instance().close();
    invokeEntry(m_visibleEntries[i]);
  }
}

void SessionPanel::invokeEntry(const SessionPanelActionConfig& cfg) {
  if (cfg.command.has_value()) {
    const std::string cmd = StringUtils::trim(*cfg.command);
    if (!cmd.empty()) {
      std::function<bool()> hook;
      if (cfg.action == "logout" || cfg.action == "reboot" || cfg.action == "shutdown") {
        hook = hookFor(cfg.action);
      }
      runShellCommand(std::move(hook), cmd, cfg.action);
      return;
    }
  }

  if (cfg.action == "command") {
    kLog.warn("session panel: custom action missing command");
    return;
  }

  if (cfg.action == "logout") {
    compositors::niri::NiriRuntime* niri = m_niriRuntime;
    runPowerAction(m_actionHooks.onLogout, [niri]() { return doLogout(niri); }, "logout");
    return;
  }
  if (cfg.action == "suspend") {
    runPowerAction({}, []() { return doSuspend(); }, "suspend");
    return;
  }
  if (cfg.action == "reboot") {
    runPowerAction(m_actionHooks.onReboot, []() { return doReboot(); }, "reboot");
    return;
  }
  if (cfg.action == "shutdown") {
    runPowerAction(m_actionHooks.onShutdown, []() { return doShutdown(); }, "shutdown");
    return;
  }
  if (cfg.action == "lock") {
    if (!doLock()) {
      notify::error("Noctalia", i18n::tr("session.errors.lock-title"), i18n::tr("session.errors.lock-body"));
    }
    return;
  }
}

bool SessionPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_visibleButtons.empty()) {
    return false;
  }

  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& cfg = m_visibleEntries[i];
    if (cfg.shortcut.has_value() && keyChordMatches(*cfg.shortcut, sym, modifiers)) {
      PanelManager::instance().close();
      invokeEntry(cfg);
      return true;
    }
  }

  const std::size_t lastIndex = m_visibleButtons.size() - 1;

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = lastIndex;
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    } else if (*m_selectedIndex > 0) {
      --(*m_selectedIndex);
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = 0;
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    } else if (*m_selectedIndex < lastIndex) {
      ++(*m_selectedIndex);
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = lastIndex;
    } else if (*m_selectedIndex >= columns) {
      *m_selectedIndex -= columns;
    }
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = 0;
    } else if (*m_selectedIndex + columns <= lastIndex) {
      *m_selectedIndex += columns;
    }
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}

void SessionPanel::updateSelectionVisuals() {
  for (std::size_t i = 0; i < m_visibleButtons.size(); ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    button->setSelected(m_selectedIndex.has_value() && i == *m_selectedIndex);
  }
}

void SessionPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->updateInputArea();
    }
  }
}

void SessionPanel::doUpdate(Renderer& /*renderer*/) {}

void SessionPanel::onClose() {
  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_visibleEntries.clear();
  m_visibleButtons.clear();
  clearReleasedRoot();
}
