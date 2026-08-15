#include "cli/help.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace noctalia::cli {

  namespace {

    using HelpEntry = std::pair<std::string, std::string>;

    [[nodiscard]] std::string positionalSpec(const Positional& positional) {
      if (positional.joinRemaining)
        return "<" + std::string(positional.name) + "...>";
      if (positional.variadic)
        return "[" + std::string(positional.name) + " ...]";
      if (positional.required)
        return "<" + std::string(positional.name) + ">";
      return "[" + std::string(positional.name) + "]";
    }

    [[nodiscard]] std::string joinChoices(std::span<const std::string_view> choices) {
      std::string result;
      for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i != 0)
          result.append(", ");
        result.append(choices[i]);
      }
      return result;
    }

    void appendSection(std::string& output, std::string_view heading, const std::vector<HelpEntry>& entries) {
      if (entries.empty())
        return;

      output.push_back('\n');
      output.append(heading);
      output.append(":\n");
      std::size_t width = 0;
      for (const auto& [left, right] : entries) {
        (void)right;
        width = std::max(width, left.size());
      }
      for (const auto& [left, right] : entries) {
        output.append("  ");
        output.append(left);
        if (!right.empty()) {
          output.append(width - left.size() + 2, ' ');
          output.append(right);
        }
        output.push_back('\n');
      }
    }

    [[nodiscard]] bool commandTakesArgs(const Command& command) {
      if (!command.positionals.empty() || !command.flags.empty())
        return true;
      return std::ranges::any_of(command.subcommands, [](const Command& child) {
        return !child.hidden && commandTakesArgs(child);
      });
    }

  } // namespace

  std::string renderArgsSpec(const Command& command) {
    if (!command.subcommands.empty()) {
      std::string result = "<{";
      bool first = true;
      bool childTakesArgs = false;
      for (const Command& child : command.subcommands) {
        if (child.hidden)
          continue;
        if (!first)
          result.push_back('|');
        result.append(child.name);
        first = false;
        childTakesArgs = childTakesArgs || commandTakesArgs(child);
      }
      result.append("}>");
      if (childTakesArgs)
        result.append(" ...");
      return result;
    }

    std::string result;
    const auto appendToken = [&result](std::string_view token) {
      if (!result.empty())
        result.push_back(' ');
      result.append(token);
    };

    for (const Positional& positional : command.positionals)
      appendToken(positionalSpec(positional));

    bool hasOptionalFlag = false;
    for (const Flag& flag : command.flags) {
      if (!flag.required) {
        hasOptionalFlag = true;
        continue;
      }
      appendToken(canonicalName(flag));
      appendToken(flag.valueName);
    }
    if (hasOptionalFlag)
      appendToken("[options]");
    return result;
  }

  std::string renderHelp(const Command& command, std::string_view path) {
    std::string output = "Usage: ";
    output.append(path);
    if (!command.subcommands.empty()) {
      output.append(" <command>");
    } else {
      for (const Positional& positional : command.positionals) {
        output.push_back(' ');
        output.append(positionalSpec(positional));
      }
    }
    if (!command.flags.empty())
      output.append(" [options]");
    output.push_back('\n');

    if (!command.details.empty()) {
      output.push_back('\n');
      output.append(command.details);
      if (output.back() != '\n')
        output.push_back('\n');
    }

    if (!command.subcommands.empty()) {
      std::vector<const Command*> children;
      for (const Command& child : command.subcommands) {
        if (!child.hidden)
          children.push_back(&child);
      }
      std::ranges::sort(children, {}, [](const Command* child) { return child->name; });

      std::vector<HelpEntry> entries;
      entries.reserve(children.size());
      for (const Command* child : children) {
        std::string left{child->name};
        const std::string args =
            command.name == "noctalia" && !child->subcommands.empty() ? "<command>" : renderArgsSpec(*child);
        if (!args.empty()) {
          left.push_back(' ');
          left.append(args);
        }
        entries.emplace_back(std::move(left), child->summary);
      }
      appendSection(output, "Commands", entries);
    } else {
      std::vector<HelpEntry> entries;
      for (const Positional& positional : command.positionals) {
        if (positional.description.empty() && positional.choices.empty())
          continue;
        std::string description{positional.description};
        if (description.empty())
          description = "one of: " + joinChoices(positional.choices);
        entries.emplace_back(positionalSpec(positional), std::move(description));
      }
      appendSection(output, "Arguments", entries);
    }

    std::vector<HelpEntry> options;
    options.reserve(command.flags.size() + 1);
    for (const Flag& flag : command.flags) {
      std::string spelling;
      if (!flag.shortName.empty() && !flag.longName.empty()) {
        spelling.append(flag.shortName);
        spelling.append(", ");
        spelling.append(flag.longName);
      } else if (!flag.shortName.empty()) {
        spelling.append(flag.shortName);
      } else {
        spelling.append("    ");
        spelling.append(flag.longName);
      }
      if (!flag.valueName.empty()) {
        spelling.push_back(' ');
        spelling.append(flag.valueName);
      }

      std::string description{flag.description};
      if (!flag.defaultValue.empty()) {
        if (!description.empty())
          description.push_back(' ');
        description.append("(default: ");
        description.append(flag.defaultValue);
        description.push_back(')');
      }
      options.emplace_back(std::move(spelling), std::move(description));
    }
    options.emplace_back("-h, --help", "Show this help message");
    appendSection(output, "Options", options);

    if (!command.epilog.empty()) {
      output.push_back('\n');
      output.append(command.epilog);
      if (output.back() != '\n')
        output.push_back('\n');
    }

    return output;
  }

} // namespace noctalia::cli
