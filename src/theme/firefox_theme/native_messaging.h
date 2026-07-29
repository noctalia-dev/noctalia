#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace noctalia::theme::firefox_theme::native_messaging {

  bool writeMessage(const nlohmann::json& message);

  class StdinReader {
  public:
    [[nodiscard]] std::optional<nlohmann::json> tryRead();
    [[nodiscard]] bool eof() const noexcept { return m_eof; }

  private:
    enum class State { Length, Body };

    State m_state = State::Length;
    std::uint32_t m_expected = 0;
    std::vector<std::uint8_t> m_buf;
    bool m_eof = false;
  };

} // namespace noctalia::theme::firefox_theme::native_messaging
