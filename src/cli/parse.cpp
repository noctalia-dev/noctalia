#include "cli/parse.h"

#include "cli/help.h"

#include <algorithm>
#include <print>

namespace noctalia::cli {

  namespace {

    [[nodiscard]] const Flag* findFlag(const Command& command, std::string_view spelling) {
      const auto it = std::ranges::find_if(command.flags, [spelling](const Flag& flag) {
        return spelling == flag.longName || spelling == flag.shortName;
      });
      return it == command.flags.end() ? nullptr : &*it;
    }

    [[nodiscard]] bool isChoice(std::span<const std::string_view> choices, std::string_view value) {
      return choices.empty() || std::ranges::find(choices, value) != choices.end();
    }

    [[nodiscard]] std::string
    choiceError(std::string_view value, std::string_view valueName, std::span<const std::string_view> choices) {
      std::string message = "error: invalid value '";
      message.append(value);
      message.append("' for ");
      message.append(valueName);
      message.append(" (expected ");
      for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i != 0)
          message.append(", ");
        message.append(choices[i]);
      }
      message.push_back(')');
      return message;
    }

  } // namespace

  bool ParsedArgs::has(std::string_view name) const {
    return std::ranges::any_of(flagValues, [name](const auto& entry) { return canonicalName(*entry.first) == name; });
  }

  std::string_view ParsedArgs::value(std::string_view name) const {
    const auto it = std::ranges::find_if(flagValues.rbegin(), flagValues.rend(), [name](const auto& entry) {
      return canonicalName(*entry.first) == name;
    });
    return it == flagValues.rend() ? std::string_view{} : it->second;
  }

  std::string_view ParsedArgs::valueOr(std::string_view name, std::string_view defaultValue) const {
    const auto it = std::ranges::find_if(flagValues.rbegin(), flagValues.rend(), [name](const auto& entry) {
      return canonicalName(*entry.first) == name;
    });
    return it == flagValues.rend() ? defaultValue : it->second;
  }

  std::vector<std::string_view> ParsedArgs::values(std::string_view name) const {
    std::vector<std::string_view> result;
    for (const auto& [flag, flagValue] : flagValues) {
      if (canonicalName(*flag) == name)
        result.push_back(flagValue);
    }
    return result;
  }

  std::expected<ParsedArgs, std::string> parseArgs(const Command& command, std::span<char* const> args) {
    ParsedArgs result;
    std::vector<bool> assigned(command.positionals.size(), false);
    std::size_t positionalIndex = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
      const std::string_view token{args[i]};
      if (token == "-h" || token == "--help") {
        result.helpRequested = true;
        return result;
      }

      if (positionalIndex < command.positionals.size() && command.positionals[positionalIndex].joinRemaining) {
        assigned[positionalIndex] = true;
        for (std::size_t remaining = i; remaining < args.size(); ++remaining) {
          if (!result.joinedRemaining.empty())
            result.joinedRemaining.push_back(' ');
          result.joinedRemaining.append(args[remaining]);
        }
        break;
      }

      const Flag* flag = nullptr;
      std::string_view inlineValue;
      bool hasInlineValue = false;
      if (token.size() > 1 && token.front() == '-') {
        flag = findFlag(command, token);
        if (flag == nullptr && token.starts_with("--")) {
          const std::size_t equals = token.find('=');
          if (equals != std::string_view::npos) {
            const std::string_view spelling = token.substr(0, equals);
            const Flag* candidate = findFlag(command, spelling);
            if (candidate != nullptr && !candidate->valueName.empty()) {
              flag = candidate;
              inlineValue = token.substr(equals + 1);
              hasInlineValue = true;
            }
          }
        }

        if (flag == nullptr)
          return std::unexpected("error: unknown argument: " + std::string(token));

        std::string_view flagValue = "1";
        if (!flag->valueName.empty()) {
          if (hasInlineValue) {
            flagValue = inlineValue;
          } else {
            if (i + 1 >= args.size())
              return std::unexpected("error: " + std::string(token) + " requires a value");
            flagValue = args[++i];
          }
          if (!isChoice(flag->choices, flagValue))
            return std::unexpected(choiceError(flagValue, flag->valueName, flag->choices));
        }

        if (!flag->repeatable) {
          std::erase_if(result.flagValues, [flag](const auto& entry) { return entry.first == flag; });
        }
        result.flagValues.emplace_back(flag, flagValue);
        continue;
      }

      if (positionalIndex >= command.positionals.size())
        return std::unexpected("error: unexpected argument: " + std::string(token));

      const Positional& positional = command.positionals[positionalIndex];
      if (!isChoice(positional.choices, token))
        return std::unexpected(choiceError(token, "<" + std::string(positional.name) + ">", positional.choices));

      assigned[positionalIndex] = true;
      result.positionals.push_back(token);
      if (!positional.variadic)
        ++positionalIndex;
    }

    for (std::size_t i = 0; i < command.positionals.size(); ++i) {
      if (command.positionals[i].required && !assigned[i]) {
        return std::unexpected("error: missing required argument <" + std::string(command.positionals[i].name) + ">");
      }
    }
    for (const Flag& flag : command.flags) {
      if (flag.required && !result.has(canonicalName(flag)))
        return std::unexpected("error: missing required flag " + std::string(canonicalName(flag)));
    }

    return result;
  }

  std::optional<ParsedArgs> parseOrReport(const Command& command, std::string_view path, std::span<char* const> args) {
    auto parsed = parseArgs(command, args);
    if (!parsed) {
      std::println(stderr, "{}", parsed.error());
      std::println(stderr, "Run '{} --help' for usage.", path);
      return std::nullopt;
    }
    if (parsed->helpRequested)
      std::print("{}", renderHelp(command, path));
    return std::move(*parsed);
  }

} // namespace noctalia::cli
