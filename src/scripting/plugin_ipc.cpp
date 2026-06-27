#include "scripting/plugin_ipc.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "scripting/script_runtime_types.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace scripting {

  namespace {
    constexpr std::string_view kUsage = "plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]";

    bool takeWord(std::string_view& text, std::string& word) {
      text = StringUtils::trimLeftView(text);
      if (text.empty()) {
        return false;
      }
      std::size_t end = 0;
      while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
        ++end;
      }
      word.assign(text.substr(0, end));
      text.remove_prefix(end);
      return true;
    }
  } // namespace

  PluginIpcRouter& PluginIpcRouter::instance() {
    static PluginIpcRouter router;
    return router;
  }

  void PluginIpcRouter::registerEndpoint(PluginIpcEndpoint* endpoint) {
    if (endpoint != nullptr) {
      m_endpoints.push_back(endpoint);
    }
  }

  void PluginIpcRouter::unregisterEndpoint(PluginIpcEndpoint* endpoint) { std::erase(m_endpoints, endpoint); }

  std::string PluginIpcRouter::dispatch(std::string_view args) const {
    std::string entryId;
    std::string target;
    std::string event;
    if (!takeWord(args, entryId) || !takeWord(args, target) || !takeWord(args, event)) {
      return std::string("error: usage: ") + std::string(kUsage) + "\n";
    }
    const std::string payload(StringUtils::trimLeftView(args));

    std::string outputSelector;
    std::string barName;
    bool hasBarName = false;
    if (const std::size_t sep = target.find(':'); sep == std::string::npos) {
      outputSelector = target;
    } else {
      outputSelector = target.substr(0, sep);
      barName = target.substr(sep + 1);
      hasBarName = true;
    }
    if (outputSelector.empty() || (hasBarName && barName.empty())) {
      return std::string("error: usage: ") + std::string(kUsage) + "\n";
    }
    const bool allOutputs = outputSelector == "all";

    const auto matchesEntry = [&](const PluginIpcEndpoint* e) {
      return e->ipcEntryId() == entryId && (!hasBarName || e->ipcBarName() == barName);
    };

    const auto focusedOutputName = [&]() -> std::string {
      if (m_platform == nullptr) {
        return {};
      }
      const auto* info = m_platform->findOutputByWl(m_platform->preferredInteractiveOutput());
      return info != nullptr ? info->connectorName : std::string{};
    };

    std::vector<PluginIpcEndpoint*> candidates;
    if (allOutputs) {
      for (auto* e : m_endpoints) {
        if (matchesEntry(e)) {
          candidates.push_back(e);
        }
      }
    } else if (outputSelector == "focused") {
      const std::string focused = focusedOutputName();
      for (auto* e : m_endpoints) {
        if (matchesEntry(e) && e->ipcOutputName() == focused && !focused.empty()) {
          candidates.push_back(e);
        }
      }
    } else {
      std::vector<std::string> connectors;
      if (m_platform != nullptr) {
        for (const auto& output : m_platform->outputs()) {
          if (outputMatchesSelector(outputSelector, output)) {
            connectors.push_back(output.connectorName);
          }
        }
      }
      if (connectors.size() > 1) {
        return "error: target '" + target + "' matched multiple outputs; use a connector name or 'all'\n";
      }
      for (auto* e : m_endpoints) {
        if (matchesEntry(e) && std::ranges::contains(connectors, e->ipcOutputName())) {
          candidates.push_back(e);
        }
      }
    }

    if (candidates.empty()) {
      return "error: no plugin entry matched '" + entryId + "' on target '" + target + "'\n";
    }
    if (!allOutputs && candidates.size() > 1) {
      return "error: target '" + target + "' matched multiple plugin instances; use '<target>:<bar-name>' or 'all'\n";
    }

    ScriptSnapshot snapshot;
    snapshot.focusedOutputName = focusedOutputName();

    int handled = 0;
    int failed = 0;
    int missingHost = 0;
    int missingCallback = 0;
    for (auto* e : candidates) {
      switch (e->dispatchIpc(event, payload, snapshot)) {
      case PluginIpcEndpoint::DispatchResult::Handled:
        ++handled;
        break;
      case PluginIpcEndpoint::DispatchResult::Failed:
        ++failed;
        break;
      case PluginIpcEndpoint::DispatchResult::MissingHost:
        ++missingHost;
        break;
      case PluginIpcEndpoint::DispatchResult::MissingCallback:
        ++missingCallback;
        break;
      }
    }

    if (failed > 0) {
      if (handled > 0) {
        return "error: dispatched " + std::to_string(handled) + ", failed " + std::to_string(failed) + "\n";
      }
      return "error: plugin onIpc callback failed\n";
    }
    if (handled > 0) {
      return "ok: dispatched " + std::to_string(handled) + "\n";
    }
    if (missingHost > 0) {
      return "error: matched plugin entry is not ready\n";
    }
    if (missingCallback > 0) {
      return "error: matched plugin entry has no onIpc callback\n";
    }
    return "error: no plugin entry matched '" + entryId + "' on target '" + target + "'\n";
  }

} // namespace scripting
