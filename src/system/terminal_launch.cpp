#include "system/terminal_launch.h"

#include "system/process.h"
#include "util/file_utils.h"

#include <array>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (const char c : cmd) {
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
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  bool isExecutableOnPath(std::string_view binary) {
    if (binary.empty()) {
      return false;
    }
    if (binary.contains('/')) {
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

  std::vector<std::string> discoverTerminal(const terminal_launch::Options& options) {
    if (!options.terminalCandidates.empty()) {
      for (const auto& candidate : options.terminalCandidates) {
        std::vector<std::string> terminal = tokenize(candidate);
        if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
          return terminal;
        }
      }
      return {};
    }
    if (!options.useSystemTerminalDiscovery) {
      return {};
    }

    if (const char* envTerminal = std::getenv("TERMINAL"); envTerminal != nullptr && envTerminal[0] != '\0') {
      std::vector<std::string> terminal = tokenize(envTerminal);
      if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
        return terminal;
      }
    }

    static constexpr std::array<std::string_view, 11> kTerminalCandidates = {
        "x-terminal-emulator", "ghostty", "kitty",  "alacritty", "wezterm", "foot", "konsole",
        "gnome-terminal",      "kgx",     "ptyxis", "xterm",
    };
    for (const auto candidate : kTerminalCandidates) {
      if (isExecutableOnPath(candidate)) {
        return {std::string(candidate)};
      }
    }
    return {};
  }

  bool usesCommandSeparator(std::string_view terminal) {
    return terminal == "gnome-terminal" || terminal == "kgx" || terminal == "ptyxis";
  }

} // namespace

namespace terminal_launch {

  std::optional<std::vector<std::string>> prepareCommand(std::string_view command, const Options& options) {
    if (command.empty()) {
      return std::nullopt;
    }

    std::vector<std::string> terminal = discoverTerminal(options);
    if (terminal.empty()) {
      return std::nullopt;
    }

    if (terminal.front().contains('/')) {
      terminal.front() = expandExecutablePath(terminal.front());
    }

    const std::string termBin = terminal.front();
    if (usesCommandSeparator(termBin)) {
      terminal.emplace_back("--");
    } else {
      terminal.emplace_back("-e");
    }
    terminal.emplace_back("sh");
    terminal.emplace_back("-lc");
    terminal.emplace_back(command);
    return terminal;
  }

  bool launch(std::string_view command, const Options& options) {
    auto prepared = prepareCommand(command, options);
    return prepared.has_value() && process::runAsync(*prepared);
  }

} // namespace terminal_launch
