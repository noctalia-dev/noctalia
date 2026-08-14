#pragma once

#include <span>
#include <string_view>

namespace noctalia::cli {

  struct Flag {
    std::string_view longName;
    std::string_view shortName;
    std::string_view valueName;
    std::string_view description;
    std::span<const std::string_view> choices;
    std::string_view defaultValue;
    bool repeatable = false;
    bool required = false;
  };

  struct Positional {
    std::string_view name;
    std::string_view description;
    std::span<const std::string_view> choices;
    bool required = false;
    bool variadic = false;
    bool joinRemaining = false;
  };

  struct Command {
    std::string_view name;
    std::string_view summary;
    std::string_view details;
    std::string_view epilog;
    std::span<const Flag> flags;
    std::span<const Positional> positionals;
    std::span<const Command> subcommands;
    bool hidden = false;
  };

  [[nodiscard]] constexpr std::string_view canonicalName(const Flag& flag) {
    return flag.longName.empty() ? flag.shortName : flag.longName;
  }

  [[nodiscard]] constexpr bool validateCommand(const Command& command) {
    for (std::size_t i = 0; i < command.flags.size(); ++i) {
      const Flag& flag = command.flags[i];
      if (flag.longName.empty() && flag.shortName.empty())
        return false;
      if (flag.valueName.empty() && (!flag.choices.empty() || !flag.defaultValue.empty() || flag.required))
        return false;
      if (!flag.longName.empty() && flag.longName == flag.shortName)
        return false;

      for (std::size_t j = i + 1; j < command.flags.size(); ++j) {
        const Flag& other = command.flags[j];
        if ((!flag.longName.empty() && (flag.longName == other.longName || flag.longName == other.shortName))
            || (!flag.shortName.empty() && (flag.shortName == other.longName || flag.shortName == other.shortName))) {
          return false;
        }
      }
    }

    bool foundCollector = false;
    for (std::size_t i = 0; i < command.positionals.size(); ++i) {
      const Positional& positional = command.positionals[i];
      if (positional.variadic && positional.joinRemaining)
        return false;
      if (positional.variadic || positional.joinRemaining) {
        if (foundCollector || i + 1 != command.positionals.size())
          return false;
        foundCollector = true;
      }
    }

    for (std::size_t i = 0; i < command.subcommands.size(); ++i) {
      if (!validateCommand(command.subcommands[i]))
        return false;
      for (std::size_t j = i + 1; j < command.subcommands.size(); ++j) {
        if (command.subcommands[i].name == command.subcommands[j].name)
          return false;
      }
    }

    return true;
  }

} // namespace noctalia::cli
