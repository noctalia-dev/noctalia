#include "theme/firefox_theme/css.h"

#include "core/files/resource_paths.h"
#include "theme/firefox_theme/settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace noctalia::theme::firefox_theme::css {
  namespace {

    [[nodiscard]] std::string withCssExtension(std::string_view name) {
      if (name.ends_with(".css")) {
        return std::string(name);
      }
      return std::string(name) + ".css";
    }

    [[nodiscard]] std::filesystem::path homeDir() {
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return home;
      }
      return {};
    }

    [[nodiscard]] std::filesystem::path xdgConfigHome() {
      if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && config[0] != '\0') {
        return config;
      }
      const auto home = homeDir();
      return home.empty() ? std::filesystem::path{} : home / ".config";
    }

    [[nodiscard]] std::optional<std::filesystem::path> profilesRoot() {
      if (const auto overridePath = settings::get("profile_path"); overridePath && !overridePath->empty()) {
        return std::filesystem::path(*overridePath);
      }

      const auto xdg = xdgConfigHome() / "firefox";
      if (std::filesystem::is_regular_file(xdg / "profiles.ini")) {
        return xdg;
      }
      const auto home = homeDir();
      if (home.empty()) {
        return std::nullopt;
      }
      return home / ".mozilla" / "firefox";
    }

    // Minimal profiles.ini reader: prefer [Install*] Default=, else Profile0 Path=.
    [[nodiscard]] std::optional<std::filesystem::path> defaultProfilePath(const std::filesystem::path& profilesDir) {
      const auto iniPath = profilesDir / "profiles.ini";
      std::ifstream in(iniPath);
      if (!in) {
        return std::nullopt;
      }

      std::string defaultRelative;
      std::string profile0Path;
      bool profile0Relative = true;
      std::string section;
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (line.empty() || line[0] == ';' || line[0] == '#') {
          continue;
        }
        if (line.front() == '[' && line.back() == ']') {
          section = line.substr(1, line.size() - 2);
          continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (section.starts_with("Install") && key == "Default") {
          defaultRelative = value;
        }
        if (section == "Profile0") {
          if (key == "Path") {
            profile0Path = value;
          } else if (key == "IsRelative") {
            profile0Relative = value == "1";
          }
        }
      }

      auto resolve = [&](const std::string& pathValue, bool relative) -> std::filesystem::path {
        if (pathValue.empty()) {
          return {};
        }
        if (relative) {
          return profilesDir / pathValue;
        }
        return pathValue;
      };

      if (!defaultRelative.empty()) {
        const auto path = resolve(defaultRelative, true);
        if (std::filesystem::is_directory(path)) {
          return path;
        }
      }
      const auto profile0 = resolve(profile0Path, profile0Relative);
      if (std::filesystem::is_directory(profile0)) {
        return profile0;
      }
      return std::nullopt;
    }

  } // namespace

  std::optional<std::filesystem::path> firefoxChromePath(std::string* error) {
    const auto root = profilesRoot();
    if (!root) {
      if (error != nullptr) {
        *error = "Could not find Firefox profiles folder";
      }
      return std::nullopt;
    }
    const auto profile = defaultProfilePath(*root);
    if (!profile) {
      if (error != nullptr) {
        *error = "Could not find profiles.ini in Firefox profiles folder";
      }
      return std::nullopt;
    }
    const auto chrome = *profile / "chrome";
    std::error_code ec;
    std::filesystem::create_directories(chrome, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "Could not create chrome folder: " + ec.message();
      }
      return std::nullopt;
    }
    return chrome;
  }

  bool enable(const std::filesystem::path& chromePath, std::string_view name, std::string* message) {
    const std::string filename = withCssExtension(name);
    const auto assets = paths::assetPath("firefox_theme/css");
    std::error_code ec;
    if (!std::filesystem::is_directory(assets, ec)) {
      if (message != nullptr) {
        *message = "Could not locate shipped Firefox theme CSS assets";
      }
      return false;
    }
    const auto src = assets / filename;
    const auto dst = chromePath / filename;
    ec.clear();
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      if (message != nullptr) {
        *message = "Could not copy custom CSS to folder: " + ec.message();
      }
      return false;
    }
    if (message != nullptr) {
      *message = "Custom CSS: " + filename + " has been enabled";
    }
    return true;
  }

  bool disable(const std::filesystem::path& chromePath, std::string_view name, std::string* message) {
    const std::string filename = withCssExtension(name);
    const auto dst = chromePath / filename;
    std::error_code ec;
    if (std::filesystem::is_regular_file(dst, ec)) {
      if (!std::filesystem::remove(dst, ec)) {
        if (message != nullptr) {
          *message = "Could not remove custom CSS: " + ec.message();
        }
        return false;
      }
    }
    if (message != nullptr) {
      *message = "Custom CSS: " + filename + " has been disabled";
    }
    return true;
  }

  bool setFontSize(const std::filesystem::path& chromePath, std::string_view name, int size, std::string* message) {
    const std::string filename = withCssExtension(name);
    const auto path = chromePath / filename;
    std::ifstream in(path);
    if (!in) {
      if (message != nullptr) {
        *message = "Could not set font size: file not found";
      }
      return false;
    }
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
      if (line.contains("--pywalfox-font-size:")) {
        out << "  --pywalfox-font-size: " << size << "px;\n";
      } else {
        out << line << '\n';
      }
    }
    in.close();
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
      if (message != nullptr) {
        *message = "Could not set font size: write failed";
      }
      return false;
    }
    file << out.str();
    if (message != nullptr) {
      *message = "Font size was set to: " + std::to_string(size);
    }
    return true;
  }

} // namespace noctalia::theme::firefox_theme::css
