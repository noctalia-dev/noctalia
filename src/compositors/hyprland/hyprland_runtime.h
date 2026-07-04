#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {

  class HyprlandEventHandler;

  struct IpcSocketPaths {
    std::string request;
    std::string event;
  };

  class HyprlandRuntime {
  public:
    HyprlandRuntime();

    [[nodiscard]] bool available() const;
    [[nodiscard]] bool connectSocket();
    [[nodiscard]] bool configIsLua() const;
    [[nodiscard]] const std::string& requestSocketPath() const;
    [[nodiscard]] const std::string& eventSocketPath() const;
    [[nodiscard]] std::optional<std::string> request(std::string_view command) const;
    [[nodiscard]] std::optional<nlohmann::json> requestJson(std::string_view command) const;
    void refresh();
    void cleanup();

    void registerEventHandler(HyprlandEventHandler* handler);
    void unregisterEventHandler(HyprlandEventHandler* handler);
    [[nodiscard]] int pollFd() const noexcept { return m_eventSocketFd; }
    void dispatchPoll(short revents);

  private:
    void ensureResolved() const;
    void resolveSocketPaths() const;
    void updateConfigProvider();
    void readSocket();
    void parseMessages();
    void handleEvent(std::string_view line);

    int m_eventSocketFd = -1;
    std::vector<char> m_readBuffer;
    mutable bool m_resolved = false;
    bool m_configIsLua = false;
    mutable IpcSocketPaths m_socketPaths;
    std::vector<HyprlandEventHandler*> m_eventHandlers;
  };

} // namespace compositors::hyprland
