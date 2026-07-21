#include "theme/firefox_theme/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace noctalia::theme::firefox_theme::settings {
  namespace {

    [[nodiscard]] std::filesystem::path configPath() {
      std::filesystem::path configHome;
      if (const char* env = std::getenv("XDG_CONFIG_HOME"); env != nullptr && env[0] != '\0') {
        configHome = env;
      } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        configHome = std::filesystem::path(home) / ".config";
      }
      if (configHome.empty()) {
        return {};
      }
      // Extension wire path — keep stable even though noctalia naming avoids "pywalfox".
      return configHome / "pywalfox" / "config.json";
    }

    [[nodiscard]] nlohmann::json load() {
      const auto path = configPath();
      if (path.empty()) {
        return nlohmann::json::object();
      }
      std::ifstream in(path);
      if (!in) {
        return nlohmann::json::object();
      }
      try {
        nlohmann::json root;
        in >> root;
        return root.is_object() ? root : nlohmann::json::object();
      } catch (...) {
        return nlohmann::json::object();
      }
    }

  } // namespace

  std::optional<std::string> get(std::string_view key) {
    const auto root = load();
    const auto it = root.find(std::string(key));
    if (it == root.end() || !it->is_string()) {
      return std::nullopt;
    }
    return it->get<std::string>();
  }

  bool set(std::string_view key, std::string_view value) {
    const auto path = configPath();
    if (path.empty()) {
      return false;
    }
    auto root = load();
    root[std::string(key)] = std::string(value);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << root.dump(2) << '\n';
    return true;
  }

} // namespace noctalia::theme::firefox_theme::settings
