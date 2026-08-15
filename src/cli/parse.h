#pragma once

#include "cli/schema.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::cli {

  struct ParsedArgs {
    std::vector<std::pair<const Flag*, std::string_view>> flagValues;
    std::vector<std::string_view> positionals;
    std::string joinedRemaining;
    bool helpRequested = false;

    [[nodiscard]] bool has(std::string_view canonicalName) const;
    [[nodiscard]] std::string_view value(std::string_view canonicalName) const;
    [[nodiscard]] std::string_view valueOr(std::string_view canonicalName, std::string_view defaultValue) const;
    [[nodiscard]] std::vector<std::string_view> values(std::string_view canonicalName) const;
  };

  [[nodiscard]] std::expected<ParsedArgs, std::string> parseArgs(const Command& command, std::span<char* const> args);

  [[nodiscard]] std::optional<ParsedArgs>
  parseOrReport(const Command& command, std::string_view path, std::span<char* const> args);

} // namespace noctalia::cli
