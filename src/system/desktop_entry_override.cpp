#include "system/desktop_entry_override.h"

#include "core/log.h"
#include "core/process/process.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// Resolve $XDG_DATA_HOME/applications with the standard fallback to
// ~/.local/share/applications. Mirrors the XDG logic in desktop_entry.cpp.
[[nodiscard]] fs::path userApplicationsDir() {
  const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
  if (xdgDataHome != nullptr && xdgDataHome[0] != '\0') {
    return fs::path(xdgDataHome) / "applications";
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return fs::path(home) / ".local" / "share" / "applications";
  }
  return fs::path("/tmp") / "applications";
}

[[nodiscard]] bool isUserLevelEntry(const DesktopEntry& entry) {
  std::error_code ec;
  const fs::path userDir = fs::weakly_canonical(userApplicationsDir(), ec);
  const fs::path entryDir = fs::weakly_canonical(fs::path(entry.path).parent_path(), ec);
  return !ec && entryDir == userDir;
}

// Rewrite `text` so the NoDisplay= line reflects `hidden`. If the key is
// absent, insert it right after the [Desktop Entry] header.
[[nodiscard]] std::string rewriteNoDisplay(std::string text, bool hidden) {
  const std::string target = hidden ? "NoDisplay=true" : "NoDisplay=false";

  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t lineStart = pos;
    const std::size_t lineEnd = text.find('\n', pos);
    const std::size_t lineLen = (lineEnd == std::string::npos) ? text.size() - pos : lineEnd - pos;
    const std::string line = text.substr(lineStart, lineLen);

    if (line.rfind("NoDisplay=", 0) == 0) {
      // Flip the existing key, preserving the line terminator.
      text.replace(lineStart, lineLen, target);
      return text;
    }
    if (lineEnd == std::string::npos) {
      break;
    }
    pos = lineEnd + 1;
  }

  // Key absent — insert after the [Desktop Entry] header.
  const std::string header = "[Desktop Entry]";
  const std::size_t headerPos = text.find(header);
  if (headerPos != std::string::npos) {
    const std::size_t insertPos = headerPos + header.size();
    text.insert(insertPos, "\n" + target);
  } else {
    text = target + "\n" + text;
  }
  return text;
}

[[nodiscard]] bool writeFileAtomic(const fs::path& path, const std::string& content, std::string* err) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (err != nullptr) {
      *err = "cannot create directory " + path.parent_path().string() + ": " + ec.message();
    }
    return false;
  }

  const fs::path tmp = path.string() + ".noctalia.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (err != nullptr) {
        *err = "cannot write " + tmp.string();
      }
      return false;
    }
    out << content;
    out.flush();
    if (!out) {
      if (err != nullptr) {
        *err = "failed writing " + tmp.string();
      }
      return false;
    }
  }

  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (err != nullptr) {
      *err = "cannot replace " + path.string() + ": " + ec.message();
    }
    return false;
  }
  return true;
}

// Scan `path` for a top-level "key=value" line (no section awareness needed —
// these are freedesktop custom keys, unlikely to collide across sections).
[[nodiscard]] std::optional<std::string> readDesktopKey(std::string_view path, std::string_view key) {
  std::ifstream in(std::string(path), std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool containsAppImagePath(std::string_view exec) {
  const std::string lower = [&] {
    std::string s(exec);
    for (char& c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }();
  return lower.find(".appimage") != std::string::npos;
}

// Pull the whitespace-delimited Exec/TryExec token that ends in .AppImage,
// stripping surrounding quotes.
[[nodiscard]] std::string extractAppImagePath(std::string_view exec) {
  std::size_t pos = 0;
  while (pos < exec.size()) {
    std::size_t end = exec.find(' ', pos);
    if (end == std::string_view::npos) {
      end = exec.size();
    }
    std::string_view token = exec.substr(pos, end - pos);
    if (!token.empty() && (token.front() == '"' || token.front() == '\'')) {
      token.remove_prefix(1);
    }
    if (!token.empty() && (token.back() == '"' || token.back() == '\'')) {
      token.remove_suffix(1);
    }
    if (containsAppImagePath(token)) {
      return std::string(token);
    }
    pos = end + 1;
  }
  return {};
}

}  // namespace

bool setDesktopEntryHidden(const DesktopEntry& entry, bool hidden, std::string* err) {
  try {
    if (entry.path.empty()) {
      if (err != nullptr) {
        *err = "desktop entry has no path";
      }
      return false;
    }

    fs::path target = entry.path;
    if (!isUserLevelEntry(entry)) {
      // System entry — create/reuse a user-level override with the same name.
      target = userApplicationsDir() / fs::path(entry.path).filename();
    }

    std::error_code ec;
    std::string content;
    if (fs::exists(target, ec)) {
      std::ifstream in(target, std::ios::binary);
      if (!in) {
        if (err != nullptr) {
          *err = "cannot read " + target.string();
        }
        return false;
      }
      content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (content.empty() || content.rfind("[Desktop Entry]", 0) != 0) {
        // Override file exists but isn't a plain desktop file (or is a stub
        // that inherits via merge). Replace it with a full copy of the system
        // file so the override is complete, then flip NoDisplay below.
        std::ifstream sys(entry.path, std::ios::binary);
        if (sys) {
          content.assign(std::istreambuf_iterator<char>(sys), std::istreambuf_iterator<char>());
        }
      }
    } else {
      // No override yet — seed from the system file so Exec/Icon survive.
      std::ifstream sys(entry.path, std::ios::binary);
      if (sys) {
        content.assign(std::istreambuf_iterator<char>(sys), std::istreambuf_iterator<char>());
      }
    }

    const std::string rewritten = rewriteNoDisplay(content, hidden);
    return writeFileAtomic(target, rewritten, err);
  } catch (const std::exception& ex) {
    if (err != nullptr) {
      *err = ex.what();
    }
    return false;
  }
}

std::optional<std::string> resolveOwningPackage(std::string_view desktopFilePath) {
  if (desktopFilePath.empty()) {
    return std::nullopt;
  }
  const process::RunResult result = process::runSync({"pacman", "-Qo", std::string(desktopFilePath)});
  if (result.exitCode != 0) {
    return std::nullopt;
  }
  // Output: "/path/to/file.desktop is owned by <pkgname> <version>"
  const std::string& out = result.out;
  constexpr std::string_view kMarker = "is owned by ";
  const std::size_t marker = out.find(kMarker);
  if (marker == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t nameStart = marker + kMarker.size();
  const std::size_t nameEnd = out.find_first_of(" \t\n", nameStart);
  if (nameStart >= out.size() || nameEnd == std::string::npos || nameEnd <= nameStart) {
    return std::nullopt;
  }
  return out.substr(nameStart, nameEnd - nameStart);
}

bool shellyListsAppImage(const std::string& appImagePath) {
  if (appImagePath.empty()) {
    return false;
  }
  const process::RunResult result = process::runSync({"shelly", "list", "appimage", "-j"});
  if (result.exitCode != 0 || result.out.empty()) {
    return false;
  }
  std::error_code ec;
  const std::string resolved = fs::weakly_canonical(appImagePath, ec).string();
  const std::string& target = ec ? appImagePath : resolved;
  try {
    const auto entries = nlohmann::json::parse(result.out);
    if (!entries.is_array()) {
      return false;
    }
    for (const auto& item : entries) {
      const auto path = item.find("Path");
      if (path != item.end() && path->is_string() && path->get<std::string>() == target) {
        return true;
      }
    }
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  return false;
}

AppSource resolveAppSource(const DesktopEntry& entry) {
  if (auto appid = readDesktopKey(entry.path, "X-Flatpak")) {
    return {AppSourceBackend::Flatpak, *appid, false};
  }

  if (readDesktopKey(entry.path, "X-AppImage-Name") || readDesktopKey(entry.path, "X-AppImage-Version")
      || containsAppImagePath(entry.exec)) {
    std::string appImagePath = extractAppImagePath(entry.exec);
    const bool managed = shellyListsAppImage(appImagePath);
    return {AppSourceBackend::AppImage, std::move(appImagePath), managed};
  }

  if (auto pkg = resolveOwningPackage(entry.path)) {
    const bool foreign = process::runSync({"pacman", "-Qm", *pkg}).exitCode == 0;
    return {foreign ? AppSourceBackend::Aur : AppSourceBackend::Standard, *pkg, false};
  }

  return {AppSourceBackend::Custom, {}, false};
}
