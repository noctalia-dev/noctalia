#include "shell/bar/widget_action_dispatcher.h"

#include "core/log.h"
#include "core/process/process.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <utility>

namespace noctalia::bar {

  namespace {

    constexpr Logger kLog("bar.actions");
    constexpr std::array<std::string_view, 2> kAnchoredPanelVerbs{"panel-toggle", "panel-open"};

  } // namespace

  bool isAnchoredPanelVerb(std::string_view verb) noexcept { return std::ranges::contains(kAnchoredPanelVerbs, verb); }

  PanelVerbArgs parsePanelVerbArgs(std::string_view args) noexcept {
    const std::string_view trimmed = StringUtils::trimLeftView(args);
    const auto space = trimmed.find(' ');
    if (space == std::string_view::npos) {
      return PanelVerbArgs{.panelId = trimmed};
    }
    return PanelVerbArgs{
        .panelId = trimmed.substr(0, space),
        .panelContext = StringUtils::trimLeftView(trimmed.substr(space + 1)),
    };
  }

  namespace {

    // Not every gesture surface is a widget: the bar dead zone dispatches with no widget name.
    std::string sourceLabel(const IpcInvocationContext& context) {
      if (!context.widgetName.empty()) {
        return std::format("widget '{}'", context.widgetName);
      }
      if (!context.barName.empty()) {
        return std::format("bar '{}' dead zone", context.barName);
      }
      return "bar gesture";
    }

  } // namespace

  bool WidgetActionDispatcher::cycles(const WidgetAction& action) const noexcept {
    return action.kind == WidgetAction::Kind::Ipc && m_ipc != nullptr && m_ipc->handlerCycles(action.verb);
  }

  bool WidgetActionDispatcher::run(const WidgetAction& action, IpcInvocationContext context) const {
    switch (action.kind) {
    case WidgetAction::Kind::None:
      return false;

    case WidgetAction::Kind::Exec:
      if (!process::runAsync(action.args)) {
        kLog.warn("{}: failed to launch \"{}\"", sourceLabel(context), action.args);
        return false;
      }
      return true;

    case WidgetAction::Kind::Ipc: {
      if (m_ipc == nullptr) {
        kLog.warn("{}: no IPC service, dropping \"{}\"", sourceLabel(context), action.verb);
        return false;
      }
      const std::string source = sourceLabel(context);
      const std::string line = action.commandLine();
      const IpcService::InvocationScope scope(*m_ipc, std::move(context));
      const std::string response = m_ipc->execute(line);
      if (response.starts_with("error:")) {
        kLog.warn("{}: \"{}\" -> {}", source, line, StringUtils::trimRightView(response));
        return false;
      }
      return true;
    }
    }
    return false;
  }

} // namespace noctalia::bar
