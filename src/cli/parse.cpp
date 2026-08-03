#include "cli/parse.h"

#include <cstdio>
#include <cstring>
#include <print>

namespace noctalia::cli_schema {
  namespace {

    const CliFlag* findFlag(const CliCommand& cmd, std::string_view arg) {
      for (const auto& flag : cmd.flags) {
        if (arg == flag.longName || (!flag.shortName.empty() && arg == flag.shortName)) {
          return &flag;
        }
      }
      return nullptr;
    }

    bool matchesChoice(const CliPositional& pos, std::string_view value) {
      if (pos.choices.empty()) {
        return true; // free-form
      }
      for (const auto& choice : pos.choices) {
        if (value == choice) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  std::expected<ParsedArgs, std::string> parseArgs(int argc, char* argv[], int startIndex, const CliCommand& cmd) {
    ParsedArgs result;
    std::size_t nextPositional = 0;

    for (int i = startIndex; i < argc; ++i) {
      const std::string_view arg = argv[i];

      if (arg == "--help") {
        std::println("{}", cmd.helpText);
        result.helpRequested = true;
        return result;
      }

      if (arg.starts_with('-')) {
        const CliFlag* flag = findFlag(cmd, arg);
        if (flag == nullptr) {
          return std::unexpected(std::string("unknown argument: ") + std::string(arg));
        }
        if (flag->takesValue) {
          if (i + 1 >= argc) {
            const std::string_view msg = flag->missingValueError.empty() ? "" : flag->missingValueError;
            if (!msg.empty()) {
              return std::unexpected(std::string(msg));
            }
            return std::unexpected(std::string(flag->longName) + " requires a value");
          }
          result.flags[flag->longName] = argv[++i];
        } else {
          result.flags[flag->longName] = "1"; // presence marker
        }
        continue;
      }

      if (nextPositional < cmd.positionals.size()) {
        const CliPositional& pos = cmd.positionals[nextPositional];
        if (!matchesChoice(pos, arg)) {
          if (!pos.invalidChoiceError.empty()) {
            return std::unexpected(std::string(pos.invalidChoiceError));
          }
          return std::unexpected(std::string("unexpected argument: ") + std::string(arg));
        }
        result.positionals.emplace_back(arg);
        ++nextPositional;
        continue;
      }

      if (!cmd.extraArgError.empty()) {
        return std::unexpected(std::string(cmd.extraArgError));
      }
      return std::unexpected(std::string("unexpected argument: ") + std::string(arg));
    }

    for (std::size_t i = 0; i < cmd.positionals.size(); ++i) {
      if (cmd.positionals[i].required && i >= result.positionals.size()) {
        if (!cmd.positionals[i].missingError.empty()) {
          return std::unexpected(std::string(cmd.positionals[i].missingError));
        }
        return std::unexpected("missing " + std::string(cmd.positionals[i].name));
      }
    }
    for (const auto& flag : cmd.flags) {
      if (flag.required && !result.hasFlag(flag.longName)) {
        if (!flag.missingFlagError.empty()) {
          return std::unexpected(std::string(flag.missingFlagError));
        }
        return std::unexpected("missing " + std::string(flag.longName));
      }
    }

    return result;
  }

} // namespace noctalia::cli_schema
