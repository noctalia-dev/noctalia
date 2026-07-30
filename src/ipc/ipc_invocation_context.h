#pragma once

#include <string>

struct wl_output;

// Origin of an IPC command that was invoked in-process rather than over the socket. Handlers that
// need to know where a command came from, such as opening the settings window at the invoking
// widget, read it through IpcService::invocationContext(), which is only populated for the
// duration of an IpcService::InvocationScope.
//
// Bar widget gesture actions are the only producer today. Panel actions do not travel this way:
// they re-enter through the bar's panel callback so they anchor at the widget.
struct IpcInvocationContext {
  std::string widgetName; // [widget.<name>] instance id
  std::string widgetType;
  std::string barName;
  wl_output* output = nullptr;
};
