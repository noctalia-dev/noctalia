#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::mango {

  class MangoRuntime {
  public:
    MangoRuntime() = default;

    bool available() const;
    const std::string& socketPath() const;
    std::optional<nlohmann::json> request(std::string_view command) const;
    bool dispatch(std::string_view command) const;
    void refresh();

  private:
    void ensureResolved() const;
    void resolveSocketPath() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
  };

} // namespace compositors::mango
