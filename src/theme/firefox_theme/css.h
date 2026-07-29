#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace noctalia::theme::firefox_theme::css {

  [[nodiscard]] std::optional<std::filesystem::path> firefoxChromePath(std::string* error = nullptr);
  bool enable(const std::filesystem::path& chromePath, std::string_view name, std::string* message);
  bool disable(const std::filesystem::path& chromePath, std::string_view name, std::string* message);
  bool setFontSize(const std::filesystem::path& chromePath, std::string_view name, int size, std::string* message);

} // namespace noctalia::theme::firefox_theme::css
