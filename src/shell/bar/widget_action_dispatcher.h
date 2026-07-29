#pragma once

#include "ipc/ipc_invocation_context.h"
#include "shell/bar/widget_action.h"

#include <string_view>

class IpcService;

namespace noctalia::bar {

  // Panel verbs are routed through Widget::requestPanelToggle rather than the IPC handler, so the
  // panel anchors at the widget that triggered it instead of the compositor's focused output.
  [[nodiscard]] bool isAnchoredPanelVerb(std::string_view verb) noexcept;

  // Splits `panel-toggle <id> [context]` style arguments.
  struct PanelVerbArgs {
    std::string_view panelId;
    std::string_view panelContext;
  };
  [[nodiscard]] PanelVerbArgs parsePanelVerbArgs(std::string_view args) noexcept;

  // Runs resolved gesture actions. Owned by Bar; widgets hold a pointer to it.
  class WidgetActionDispatcher {
  public:
    void setIpcService(IpcService* ipc) noexcept { m_ipc = ipc; }

    // Returns false when the action could not be run. Panel verbs must be handled by the caller.
    bool run(const WidgetAction& action, IpcInvocationContext context) const;

    // True when the action's verb declared itself as stepping one position along an ordered set
    // (IpcService::registerCycleHandler). Bound to a scroll gesture, those run once per flick
    // rather than once per notch. Exec actions are opaque and never count as cycles.
    [[nodiscard]] bool cycles(const WidgetAction& action) const noexcept;

  private:
    IpcService* m_ipc = nullptr;
  };

} // namespace noctalia::bar
