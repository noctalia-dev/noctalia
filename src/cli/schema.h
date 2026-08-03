#pragma once

#include <span>
#include <string_view>

namespace noctalia::cli_schema {

  // A single named flag. `--flag <value>` if takesValue is true, `--flag` (boolean) otherwise.
  struct CliFlag {
    std::string_view longName;  // "--target"
    std::string_view shortName; // "" if none
    std::string_view description;
    bool takesValue = false;
    bool required = false;
    // Error text when takesValue and the flag is given with nothing after it,
    // e.g. "--target requires a directory". Empty falls back to
    // "<longName> requires a value".
    std::string_view missingValueError;
    // Error text when required and never given at all, e.g. "missing --target <dir>".
    // Empty falls back to "missing <longName>". This is a distinct case from
    // missingValueError: the flag was never passed vs. passed with no value.
    std::string_view missingFlagError;
  };

  // A positional argument. `choices` empty means free-form (e.g. a file path);
  // non-empty means the value must be one of these (e.g. export's merged|full).
  struct CliPositional {
    std::string_view name; // shown in usage, e.g. "path" or "mode"
    std::string_view description;
    std::span<const std::string_view> choices; // empty = free-form
    bool required = false;
    // Error text when required and missing, e.g. "missing report path".
    // Empty falls back to "missing <name>". Existing wording is preserved
    // exactly by setting this explicitly wherever it doesn't match that default.
    std::string_view missingError;
    // Error text when a value is given but doesn't match `choices`, e.g.
    // "expected merged or full". Empty falls back to a generic message.
    std::string_view invalidChoiceError;
  };

  // One CLI command (a leaf like `validate`, or a group like `config` itself).
  // `helpText` is the full pre-rendered --help body for this exact command —
  // the schema does not attempt to auto-format help; existing kXxxHelpText
  // constants stay put and are referenced here, so wording never changes.
  struct CliCommand {
    std::string_view name;
    std::string_view summary; // one-line, used in the parent's command list
    std::string_view helpText;
    std::span<const CliFlag> flags;
    std::span<const CliPositional> positionals;
    std::span<const CliCommand> subcommands;
    // Error text when more positionals are given than declared, e.g.
    // "error: usage: bar-show [bar-name] [monitor-selector]". Empty falls
    // back to the generic "unexpected argument: <value>" from parseArgs.
    std::string_view extraArgError;
  };

} // namespace noctalia::cli_schema
