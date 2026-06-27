#pragma once

#include <string>
#include <string_view>
#include <vector>

class CompositorPlatform;

namespace scripting {
  struct ScriptSnapshot;

  // A live plugin entry instance reachable by IPC. Any surface that hosts a
  // plugin entry (bar widget today; panels/shortcuts/etc. later) implements this
  // and registers with the router for its lifetime.
  class PluginIpcEndpoint {
  public:
    enum class DispatchResult {
      Handled,
      MissingHost,
      MissingCallback,
      Failed,
    };

    virtual ~PluginIpcEndpoint() = default;

    [[nodiscard]] virtual std::string_view ipcEntryId() const = 0;    // "author/plugin:entry"
    [[nodiscard]] virtual std::string_view ipcOutputName() const = 0; // connector, or empty
    [[nodiscard]] virtual std::string_view ipcBarName() const = 0;    // bar name, or empty for non-bar surfaces
    [[nodiscard]] virtual DispatchResult
    dispatchIpc(std::string_view event, std::string_view payload, const ScriptSnapshot& snapshot) = 0;
  };

  // Routes `noctalia msg plugin <author/plugin:entry> <target> <event> [payload]`
  // to registered endpoints. Owned by the plugin layer, not by any UI surface.
  class PluginIpcRouter {
  public:
    static PluginIpcRouter& instance();

    void setPlatform(CompositorPlatform* platform) { m_platform = platform; }

    void registerEndpoint(PluginIpcEndpoint* endpoint);
    void unregisterEndpoint(PluginIpcEndpoint* endpoint);

    // Handle the `plugin` IPC command body; returns the socket reply line.
    [[nodiscard]] std::string dispatch(std::string_view args) const;

  private:
    std::vector<PluginIpcEndpoint*> m_endpoints;
    CompositorPlatform* m_platform = nullptr;
  };

} // namespace scripting
