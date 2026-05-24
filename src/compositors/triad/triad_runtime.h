#pragma once

#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::triad {

  class TriadRuntime {
  public:
    TriadRuntime() = default;

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& socketPath() const;
    [[nodiscard]] std::optional<nlohmann::json> requestJson(std::string_view requestName) const;
    [[nodiscard]] std::optional<nlohmann::json> requestPayload(const nlohmann::json& payload) const;
    [[nodiscard]] bool requestAction(std::string_view action, nlohmann::json args = nlohmann::json::object()) const;
    void refresh();

  private:
    struct IpcReply;

    [[nodiscard]] IpcReply request(std::string_view payload) const;
    void ensureResolved() const;
    void resolveSocketPath() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
  };

} // namespace compositors::triad
