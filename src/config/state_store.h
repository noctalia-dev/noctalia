#pragma once

#include "core/toml.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class StateStore {
public:
  StateStore() = default;
  explicit StateStore(std::filesystem::path path);

  void setPath(std::filesystem::path path);
  void load();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
  [[nodiscard]] const std::string& parseError() const noexcept { return m_parseError; }
  [[nodiscard]] std::optional<bool> boolValue(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::optional<std::string> stringValue(std::string_view owner, std::string_view key) const;

  bool setBool(std::string_view owner, std::string_view key, bool value);
  bool setString(std::string_view owner, std::string_view key, std::string_view value);
  bool clearOwner(std::string_view owner);

private:
  [[nodiscard]] bool write();

  std::filesystem::path m_path;
  toml::table m_state;
  std::string m_parseError;
};
