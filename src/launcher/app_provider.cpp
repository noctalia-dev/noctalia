#include "launcher/app_provider.h"

#include "config/config_service.h"
#include "core/process.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace {

  constexpr std::size_t kMaxSearchResults = 50;
  constexpr std::string_view kDefaultAppIcon = "application-x-executable";

  double scoreEntry(std::string_view pattern, const DesktopEntry& entry) {
    if (pattern.empty()) {
      return 0.0;
    }

    double nameScore = FuzzyMatch::score(pattern, entry.nameLower) * 5.0;
    if (FuzzyMatch::isMatch(nameScore) && entry.nameLower.starts_with(pattern)) {
      nameScore += 500.0;
    }
    const double genericScore = FuzzyMatch::score(pattern, entry.genericNameLower) * 2.0;

    auto scoreList = [&](std::string_view list, double weight) {
      double best = FuzzyMatch::noMatchScore;
      std::size_t start = 0;
      while (start < list.size()) {
        auto semi = list.find(';', start);
        auto word = (semi == std::string_view::npos) ? list.substr(start) : list.substr(start, semi - start);
        if (!word.empty()) {
          best = std::max(best, FuzzyMatch::score(pattern, word) * weight);
        }
        if (semi == std::string_view::npos)
          break;
        start = semi + 1;
      }
      return best;
    };

    const double keywordScore = scoreList(entry.keywordsLower, 0.8);
    const double catScore = scoreList(entry.categoriesLower, 0.3);
    const double idScore = FuzzyMatch::score(pattern, entry.idLower) * 1.5;
    const double execScore = FuzzyMatch::score(pattern, entry.execLower);

    return std::max({nameScore, genericScore, keywordScore, catScore, idScore, execScore});
  }

  std::string stripFieldCodes(const std::string& exec) {
    std::string result;
    result.reserve(exec.size());
    for (std::size_t i = 0; i < exec.size(); ++i) {
      if (exec[i] == '%' && i + 1 < exec.size()) {
        char next = exec[i + 1];
        if (next == 'f' || next == 'F' || next == 'u' || next == 'U' || next == 'd' || next == 'D' || next == 'n' ||
            next == 'N' || next == 'i' || next == 'c' || next == 'k') {
          ++i; // Skip the field code
          // Also skip trailing space
          if (i + 1 < exec.size() && exec[i + 1] == ' ') {
            ++i;
          }
          continue;
        }
        if (next == '%') {
          result += '%';
          ++i;
          continue;
        }
      }
      result += exec[i];
    }

    // Trim trailing whitespace
    while (!result.empty() && result.back() == ' ') {
      result.pop_back();
    }
    return result;
  }

  std::vector<std::string> tokenize(const std::string& cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (std::size_t i = 0; i < cmd.size(); ++i) {
      char c = cmd[i];

      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary[0] != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  bool isExecutableOnPath(std::string_view binary) {
    if (binary.empty()) {
      return false;
    }
    if (binary.find('/') != std::string_view::npos) {
      const std::string expanded = expandExecutablePath(binary);
      return access(expanded.c_str(), X_OK) == 0;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return false;
    }

    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto sep = path.find(':', start);
      const auto segment = sep == std::string_view::npos ? path.substr(start) : path.substr(start, sep - start);
      if (!segment.empty()) {
        std::string candidate(segment);
        candidate.push_back('/');
        candidate.append(binary);
        if (access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (sep == std::string_view::npos) {
        break;
      }
      start = sep + 1;
    }
    return false;
  }

  std::vector<std::string> terminalLaunchArgs(const std::string& command) {
    std::vector<std::string> terminal;
    if (const char* envTerminal = std::getenv("TERMINAL"); envTerminal != nullptr && envTerminal[0] != '\0') {
      terminal = tokenize(envTerminal);
      if (!terminal.empty() && !isExecutableOnPath(terminal.front())) {
        terminal.clear();
      }
    }

    if (terminal.empty()) {
      static constexpr std::array<std::string_view, 9> kTerminalCandidates = {
          "x-terminal-emulator", "ghostty", "kitty", "alacritty", "wezterm", "foot", "konsole",
          "gnome-terminal",      "xterm"
      };
      for (const auto candidate : kTerminalCandidates) {
        if (isExecutableOnPath(candidate)) {
          terminal.emplace_back(candidate);
          break;
        }
      }
    }

    if (terminal.empty()) {
      return {};
    }

    const std::string& termBin = terminal.front();
    if (termBin == "gnome-terminal" || termBin == "kgx" || termBin == "ptyxis") {
      terminal.emplace_back("--");
      terminal.emplace_back("sh");
      terminal.emplace_back("-lc");
      terminal.emplace_back(command);
    } else {
      terminal.emplace_back("-e");
      terminal.emplace_back("sh");
      terminal.emplace_back("-lc");
      terminal.emplace_back(command);
    }
    return terminal;
  }

  void launchCommand(
      const std::string& exec, bool terminal, const std::string& activationToken, const std::string& workingDir,
      const std::string& appName, bool runAsSystemdService
  ) {
    std::string cleanExec = stripFieldCodes(exec);
    std::vector<std::string> args = terminal ? terminalLaunchArgs(cleanExec) : tokenize(cleanExec);

    if (!args.empty() && args.front().find('/') != std::string::npos) {
      args.front() = expandExecutablePath(args.front());
    }

    if (args.empty()) {
      return;
    }

    if (runAsSystemdService) {
      process::runAsyncAsSystemdService(args, appName, activationToken, workingDir);
    } else {
      (void)process::runAsync(args, activationToken, workingDir);
    }
  }

  std::string_view primaryCategory(std::string_view categories) {
    std::size_t start = 0;
    while (start < categories.size()) {
      auto semi = categories.find(';', start);
      auto token = (semi == std::string_view::npos) ? categories.substr(start) : categories.substr(start, semi - start);
      if (token == "AudioVideo" || token == "Audio" || token == "Video") {
        return "Multimedia";
      }
      if (token == "Development") {
        return "Development";
      }
      if (token == "Game") {
        return "Games";
      }
      if (token == "Graphics") {
        return "Graphics";
      }
      if (token == "Network") {
        return "Internet";
      }
      if (token == "Office") {
        return "Office";
      }
      if (token == "System") {
        return "System";
      }
      if (token == "Utility" || token == "Settings") {
        return "Utilities";
      }
      if (token == "Education" || token == "Science") {
        return "Education";
      }
      if (semi == std::string_view::npos) {
        break;
      }
      start = semi + 1;
    }
    return {};
  }

} // namespace

AppProvider::AppProvider(ConfigService* config, WaylandConnection* wayland) : m_config(config), m_wayland(wayland) {}

void AppProvider::initialize() { refreshEntriesIfNeeded(); }

std::vector<LauncherCategory> AppProvider::categories() const {
  return {
      {"Internet", "world"},         {"Multimedia", "player-play"}, {"Development", "code"},
      {"Games", "device-gamepad-2"}, {"Graphics", "photo"},         {"Office", "briefcase"},
      {"Education", "school"},       {"System", "settings"},        {"Utilities", "tool"},
  };
}

void AppProvider::refreshEntriesIfNeeded() const {
  const auto version = desktopEntriesVersion();
  if (version == m_entriesVersion) {
    return;
  }

  m_entries = desktopEntries();
  m_entriesVersion = version;
}

std::vector<LauncherResult> AppProvider::query(std::string_view text) const {
  refreshEntriesIfNeeded();
  const std::string normalizedText = StringUtils::toLower(text);
  const std::string_view pattern = normalizedText;

  auto buildResult = [&](const DesktopEntry& entry, double s) {
    LauncherResult result;
    result.id = entry.path;
    result.title = entry.name;
    result.subtitle = entry.genericName.empty() ? entry.comment : entry.genericName;
    result.iconName = entry.icon.empty() ? std::string(kDefaultAppIcon) : entry.icon;
    result.glyphName = "app-window";
    result.category = std::string(primaryCategory(entry.categories));
    result.score = s;
    return result;
  };

  // Empty query: return all entries in alphabetical order (as stored)
  if (pattern.empty()) {
    std::vector<LauncherResult> results;
    results.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
      results.push_back(buildResult(entry, 0));
    }
    return results;
  }

  std::vector<std::pair<double, const DesktopEntry*>> scored;
  for (const auto& entry : m_entries) {
    const double s = scoreEntry(pattern, entry);
    if (FuzzyMatch::isMatch(s)) {
      scored.emplace_back(s, &entry);
    }
  }
  const auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };
  const std::size_t limit = std::min(scored.size(), kMaxSearchResults);
  std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(limit), scored.end(), cmp);

  std::vector<LauncherResult> results;
  results.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& [s, entry] = scored[i];
    results.push_back(buildResult(*entry, s));
  }
  return results;
}

bool AppProvider::activate(const LauncherResult& result) {
  refreshEntriesIfNeeded();

  for (const auto& entry : m_entries) {
    if (entry.path != result.id) {
      continue;
    }

    std::string execLine = entry.exec;
    if (!result.desktopActionId.empty()) {
      const DesktopAction* chosen = nullptr;
      for (const auto& action : entry.actions) {
        if (action.id == result.desktopActionId) {
          chosen = &action;
          break;
        }
      }
      if (chosen == nullptr || chosen->exec.empty()) {
        return false;
      }
      execLine = chosen->exec;
    }

    std::string token;
    if (m_wayland != nullptr && m_wayland->hasXdgActivation()) {
      token = m_wayland->requestActivationToken(nullptr);
    }
    launchCommand(
        execLine, entry.terminal, token, entry.workingDir, entry.id,
        m_config->config().shell.launchAppsAsSystemdServices
    );
    return true;
  }
  return false;
}
