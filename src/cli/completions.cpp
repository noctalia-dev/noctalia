#include "cli/completions.h"

#include "cli/parse.h"
#include "cli/schema_root.h"

#include <print>
#include <span>
#include <string_view>
#include <vector>

namespace noctalia::cli {

  namespace {

    struct CommandState {
      const Command* command;
      std::vector<std::string_view> path;
    };

    void collectStates(const Command& command, std::vector<std::string_view> path, std::vector<CommandState>& states) {
      path.push_back(command.name);
      states.push_back(CommandState{.command = &command, .path = path});
      for (const Command& child : command.subcommands) {
        if (!child.hidden)
          collectStates(child, path, states);
      }
    }

    [[nodiscard]] std::vector<CommandState> collectStates(const Command& root) {
      std::vector<CommandState> states;
      collectStates(root, {}, states);
      return states;
    }

    [[nodiscard]] std::string stateId(std::span<const std::string_view> path) {
      std::string id;
      for (const std::string_view segment : path) {
        if (!id.empty())
          id.push_back('_');
        for (const char c : segment)
          id.push_back(c == '-' ? '_' : c);
      }
      return id;
    }

    [[nodiscard]] std::string shellSingleQuote(std::string_view value) {
      std::string quoted{"'"};
      for (const char c : value) {
        if (c == '\'')
          quoted.append("'\\''");
        else
          quoted.push_back(c);
      }
      quoted.push_back('\'');
      return quoted;
    }

    [[nodiscard]] std::string bashAnsiQuote(std::string_view value) {
      std::string quoted{"$'"};
      for (const char c : value) {
        if (c == '\\' || c == '\'')
          quoted.push_back('\\');
        if (c == '\n') {
          quoted.append("\\n");
        } else {
          quoted.push_back(c);
        }
      }
      quoted.push_back('\'');
      return quoted;
    }

    [[nodiscard]] std::string newlineChoices(std::span<const std::string_view> choices) {
      std::string words;
      for (const std::string_view choice : choices) {
        if (!words.empty())
          words.push_back('\n');
        words.append(choice);
      }
      return words;
    }

    [[nodiscard]] std::string spaceWords(const Command& command) {
      std::string words;
      const auto append = [&words](std::string_view word) {
        if (!words.empty())
          words.push_back('\n');
        words.append(word);
      };
      for (const Command& child : command.subcommands) {
        if (!child.hidden)
          append(child.name);
      }
      for (const Flag& flag : command.flags) {
        if (!flag.shortName.empty())
          append(flag.shortName);
        if (!flag.longName.empty())
          append(flag.longName);
      }
      append("-h");
      append("--help");
      return words;
    }

    [[nodiscard]] std::string pathArgs(std::span<const std::string_view> path) {
      std::string result;
      for (std::size_t i = 1; i < path.size(); ++i) {
        if (!result.empty())
          result.push_back(' ');
        result.append(path[i]);
      }
      return result;
    }

    [[nodiscard]] std::string fishChoiceList(std::span<const std::string_view> choices) {
      std::string result;
      for (const std::string_view choice : choices) {
        if (!result.empty())
          result.push_back(' ');
        if (choice.find_first_of(" \t") == std::string_view::npos) {
          result.append(choice);
        } else {
          result.push_back('"');
          for (const char c : choice) {
            if (c == '"' || c == '\\')
              result.push_back('\\');
            result.push_back(c);
          }
          result.push_back('"');
        }
      }
      return result;
    }

    [[nodiscard]] std::string zshChoiceList(std::span<const std::string_view> choices) {
      std::string result{"("};
      for (const std::string_view choice : choices) {
        if (result.size() > 1)
          result.push_back(' ');
        result.append(shellSingleQuote(choice));
      }
      result.push_back(')');
      return result;
    }

    void appendBashWords(std::string& output, std::string_view words, unsigned indent) {
      output.append(indent, ' ');
      output.append("_noctalia_complete_words ");
      output.append(bashAnsiQuote(words));
      output.push_back('\n');
    }

    void appendBashCompletion(std::string& output, std::span<const std::string_view> choices, unsigned indent) {
      appendBashWords(output, newlineChoices(choices), indent);
    }

    void appendBashFiles(std::string& output, unsigned indent) {
      output.append(indent, ' ');
      output.append("compopt -o filenames\n");
      output.append(indent, ' ');
      output.append("COMPREPLY=( $(compgen -f -- \"$cur\") )\n");
    }

    void appendZshFlagSpecs(std::string& output, const Command& command) {
      for (const Flag& flag : command.flags) {
        std::string action;
        if (!flag.valueName.empty()) {
          if (flag.choices.empty())
            action = ":" + std::string(flag.valueName) + ":_files";
          else
            action = ":" + std::string(flag.valueName) + ":" + zshChoiceList(flag.choices);
        }
        const auto append = [&](std::string_view spelling, std::string_view other) {
          if (spelling.empty())
            return;
          std::string spec;
          if (!other.empty()) {
            spec.push_back('(');
            spec.append(spelling);
            spec.push_back(' ');
            spec.append(other);
            spec.push_back(')');
          }
          spec.append(spelling);
          if (!flag.description.empty()) {
            spec.push_back('[');
            spec.append(flag.description);
            spec.push_back(']');
          }
          spec.append(action);
          output.append("    ");
          output.append(shellSingleQuote(spec));
          output.append(" \\\n");
        };
        append(flag.shortName, flag.longName);
        append(flag.longName, flag.shortName);
      }
      output.append("    '(-h --help)-h[Show this help message]' \\\n");
      output.append("    '(-h --help)--help[Show this help message]' \\\n");
    }

  } // namespace

  std::string generateBash(const Command& root) {
    const auto states = collectStates(root);
    std::string output = "# bash completion for noctalia; generated from the live CLI schema\n"
                         "_noctalia_complete_words() {\n"
                         "  local words=\"$1\"\n"
                         "  local IFS=$'\\n'\n"
                         "  COMPREPLY=( $(compgen -W \"$words\" -- \"$cur\") )\n"
                         "}\n\n"
                         "_noctalia_completions() {\n"
                         "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                         "  local path=noctalia\n"
                         "  local depth=1\n"
                         "  local i token\n"
                         "  for ((i=1; i<COMP_CWORD; ++i)); do\n"
                         "    token=\"${COMP_WORDS[i]}\"\n"
                         "    case \"$path:$token\" in\n";

    for (const CommandState& state : states) {
      for (const Command& child : state.command->subcommands) {
        if (child.hidden)
          continue;
        auto childPath = state.path;
        childPath.push_back(child.name);
        output.append("      ");
        output.append(stateId(state.path));
        output.push_back(':');
        output.append(child.name);
        output.append(") path=");
        output.append(stateId(childPath));
        output.append("; depth=$((i + 1)) ;;\n");
      }
    }
    output.append("    esac\n  done\n\n  case \"$path\" in\n");

    for (const CommandState& state : states) {
      const Command& command = *state.command;
      output.append("    ");
      output.append(stateId(state.path));
      output.append(")\n");

      for (const Flag& flag : command.flags) {
        if (flag.valueName.empty())
          continue;
        output.append("      case \"${COMP_WORDS[COMP_CWORD-1]}\" in\n        ");
        if (!flag.shortName.empty())
          output.append(flag.shortName);
        if (!flag.shortName.empty() && !flag.longName.empty())
          output.push_back('|');
        if (!flag.longName.empty())
          output.append(flag.longName);
        output.append(")\n");
        if (flag.choices.empty())
          appendBashFiles(output, 10);
        else
          appendBashCompletion(output, flag.choices, 10);
        output.append("          return ;;\n      esac\n");
      }

      output.append("      if [[ \"$cur\" == -* ]]; then\n");
      appendBashWords(output, spaceWords(command), 8);
      output.append("        return\n      fi\n");

      if (!command.subcommands.empty()) {
        std::vector<std::string_view> names;
        for (const Command& child : command.subcommands) {
          if (!child.hidden)
            names.push_back(child.name);
        }
        appendBashCompletion(output, names, 6);
        output.append("      ;;\n");
        continue;
      }

      output.append(
          "      local positional=0 skip_next=0\n"
          "      for ((i=depth; i<COMP_CWORD; ++i)); do\n"
          "        token=\"${COMP_WORDS[i]}\"\n"
          "        if ((skip_next)); then skip_next=0; continue; fi\n"
      );
      std::vector<std::string_view> valueSpellings;
      for (const Flag& flag : command.flags) {
        if (flag.valueName.empty())
          continue;
        if (!flag.shortName.empty())
          valueSpellings.push_back(flag.shortName);
        if (!flag.longName.empty())
          valueSpellings.push_back(flag.longName);
      }
      if (!valueSpellings.empty()) {
        output.append("        case \"$token\" in\n          ");
        for (std::size_t i = 0; i < valueSpellings.size(); ++i) {
          if (i != 0)
            output.push_back('|');
          output.append(valueSpellings[i]);
        }
        output.append(") skip_next=1; continue ;;\n        esac\n");
      }
      output.append(
          "        [[ \"$token\" == --*=* || \"$token\" == -* ]] && continue\n"
          "        ((positional++))\n"
          "      done\n"
          "      case $positional in\n"
      );
      for (std::size_t i = 0; i < command.positionals.size(); ++i) {
        const Positional& positional = command.positionals[i];
        output.append("        ");
        output.append(std::to_string(i));
        output.append(")\n");
        if (positional.choices.empty())
          appendBashFiles(output, 10);
        else
          appendBashCompletion(output, positional.choices, 10);
        output.append("          ;;\n");
        if (positional.variadic)
          break;
      }
      output.append("        *)\n");
      if (!command.positionals.empty() && command.positionals.back().variadic) {
        const Positional& positional = command.positionals.back();
        if (positional.choices.empty())
          appendBashFiles(output, 10);
        else
          appendBashCompletion(output, positional.choices, 10);
      }
      output.append("          ;;\n      esac\n      ;;\n");
    }
    output.append("  esac\n}\n\ncomplete -F _noctalia_completions noctalia\n");
    return output;
  }

  std::string generateZsh(const Command& root) {
    const auto states = collectStates(root);
    std::string output = "#compdef noctalia\n# generated from the live CLI schema\n\n";

    for (const CommandState& state : states) {
      const Command& command = *state.command;
      output.append("_noctalia_");
      output.append(stateId(state.path));
      output.append("() {\n  local context state state_descr line\n  typeset -A opt_args\n");

      if (!command.subcommands.empty()) {
        output.append("  local -a commands\n  commands=(\n");
        for (const Command& child : command.subcommands) {
          if (child.hidden)
            continue;
          output.append("    ");
          output.append(shellSingleQuote(std::string(child.name) + ":" + std::string(child.summary)));
          output.push_back('\n');
        }
        output.append("  )\n  _arguments -C \\\n");
        appendZshFlagSpecs(output, command);
        output.append(
            "    '1:command:->command' \\\n    '*::argument:->args'\n"
            "  case $state in\n"
            "    command) _describe 'command' commands ;;\n"
            "    args)\n"
            "      case $line[1] in\n"
        );
        for (const Command& child : command.subcommands) {
          if (child.hidden)
            continue;
          auto childPath = state.path;
          childPath.push_back(child.name);
          output.append("        ");
          output.append(child.name);
          output.append(") _noctalia_");
          output.append(stateId(childPath));
          output.append(" ;;\n");
        }
        output.append("      esac\n      ;;\n  esac\n");
      } else {
        output.append("  _arguments \\\n");
        appendZshFlagSpecs(output, command);
        for (std::size_t i = 0; i < command.positionals.size(); ++i) {
          const Positional& positional = command.positionals[i];
          std::string spec = positional.variadic ? "*" : std::to_string(i + 1);
          spec.push_back(':');
          spec.append(positional.name);
          spec.push_back(':');
          spec.append(positional.choices.empty() ? "_files" : zshChoiceList(positional.choices));
          output.append("    ");
          output.append(shellSingleQuote(spec));
          output.append(i + 1 == command.positionals.size() ? "\n" : " \\\n");
        }
        if (command.positionals.empty())
          output.append("    ':argument:_files'\n");
      }
      output.append("}\n\n");
    }

    output.append("_noctalia() { _noctalia_");
    output.append(stateId(states.front().path));
    output.append(" }\n\n_noctalia \"$@\"\n");
    return output;
  }

  std::string generateFish(const Command& root) {
    const auto states = collectStates(root);
    std::string output = "# fish completion for noctalia; generated from the live CLI schema\n"
                         "function __noctalia_path_prefix\n"
                         "    set -l tokens (commandline -opc)\n"
                         "    if test (count $tokens) -gt 0\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    for segment in $argv\n"
                         "        if test (count $tokens) -eq 0; or test $tokens[1] != $segment\n"
                         "            return 1\n"
                         "        end\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    return 0\n"
                         "end\n\n"
                         "function __noctalia_exact_path\n"
                         "    set -l tokens (commandline -opc)\n"
                         "    if test (count $tokens) -gt 0\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    if test (count $tokens) -ne (count $argv)\n"
                         "        return 1\n"
                         "    end\n"
                         "    for segment in $argv\n"
                         "        if test $tokens[1] != $segment\n"
                         "            return 1\n"
                         "        end\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    return 0\n"
                         "end\n\n"
                         "function __noctalia_at_pos\n"
                         "    set -l wanted $argv[1]\n"
                         "    set -e argv[1]\n"
                         "    set -l tokens (commandline -opc)\n"
                         "    if test (count $tokens) -gt 0\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    for segment in $argv\n"
                         "        if test (count $tokens) -eq 0; or test $tokens[1] != $segment\n"
                         "            return 1\n"
                         "        end\n"
                         "        set -e tokens[1]\n"
                         "    end\n"
                         "    set -l seen 0\n"
                         "    for token in $tokens\n"
                         "        if not string match -qr '^-' -- $token\n"
                         "            set seen (math $seen + 1)\n"
                         "        end\n"
                         "    end\n"
                         "    test $seen -eq $wanted\n"
                         "end\n\n";

    for (const CommandState& state : states) {
      const Command& command = *state.command;
      const std::string path = pathArgs(state.path);
      const std::string exactCondition = path.empty() ? "__noctalia_exact_path" : "__noctalia_exact_path " + path;
      const std::string prefixCondition = path.empty() ? "__noctalia_path_prefix" : "__noctalia_path_prefix " + path;

      for (const Command& child : command.subcommands) {
        if (child.hidden)
          continue;
        output.append("complete -c noctalia -n ");
        output.append(shellSingleQuote(exactCondition));
        output.append(" -f -a ");
        output.append(shellSingleQuote(child.name));
        if (!child.summary.empty()) {
          output.append(" -d ");
          output.append(shellSingleQuote(child.summary));
        }
        output.push_back('\n');
      }

      for (const Flag& flag : command.flags) {
        output.append("complete -c noctalia -n ");
        output.append(shellSingleQuote(prefixCondition));
        if (!flag.shortName.empty()) {
          output.append(" -s ");
          output.append(flag.shortName.substr(1));
        }
        if (!flag.longName.empty()) {
          output.append(" -l ");
          output.append(flag.longName.substr(2));
        }
        if (!flag.valueName.empty()) {
          output.append(" -r");
          if (flag.choices.empty()) {
            output.append(" -F");
          } else {
            output.append(" -a ");
            output.append(shellSingleQuote(fishChoiceList(flag.choices)));
          }
        }
        if (!flag.description.empty()) {
          output.append(" -d ");
          output.append(shellSingleQuote(flag.description));
        }
        output.push_back('\n');
      }
      output.append("complete -c noctalia -n ");
      output.append(shellSingleQuote(prefixCondition));
      output.append(" -s h -l help -d 'Show this help message'\n");

      for (std::size_t i = 0; i < command.positionals.size(); ++i) {
        const Positional& positional = command.positionals[i];
        if (positional.choices.empty())
          continue;
        std::string condition = "__noctalia_at_pos " + std::to_string(i);
        if (!path.empty()) {
          condition.push_back(' ');
          condition.append(path);
        }
        output.append("complete -c noctalia -n ");
        output.append(shellSingleQuote(condition));
        output.append(" -f -a ");
        output.append(shellSingleQuote(fishChoiceList(positional.choices)));
        if (!positional.description.empty()) {
          output.append(" -d ");
          output.append(shellSingleQuote(positional.description));
        }
        output.push_back('\n');
      }
    }
    return output;
  }

  int runCompletionsCli(int argc, char* argv[]) {
    auto parsed = parseOrReport(
        kCompletionsCmd, "noctalia completions", std::span<char* const>{argv + 2, static_cast<std::size_t>(argc - 2)}
    );
    if (!parsed)
      return 1;
    if (parsed->helpRequested)
      return 0;

    const std::string_view shell = parsed->positionals.front();
    if (shell == "bash")
      std::print("{}", generateBash(kRootCmd));
    else if (shell == "zsh")
      std::print("{}", generateZsh(kRootCmd));
    else
      std::print("{}", generateFish(kRootCmd));
    return 0;
  }

} // namespace noctalia::cli
