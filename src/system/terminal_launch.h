#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terminal_launch {

  struct Options {
    std::vector<std::string> terminalCandidates;
    bool useSystemTerminalDiscovery = true;
  };

  std::optional<std::vector<std::string>> prepareCommand(std::string_view command, const Options& options = {});

  [[nodiscard]] bool launch(std::string_view command, const Options& options = {});

} // namespace terminal_launch
