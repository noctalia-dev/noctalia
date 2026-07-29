#pragma once

#include <string>
#include <variant>
#include <vector>

namespace scripting {

  // One argument passed to a script callback. Each alternative maps directly
  // onto a Luau value: boolean, number, string.
  using ScriptArg = std::variant<bool, double, std::string>;
  using ScriptArgs = std::vector<ScriptArg>;

} // namespace scripting
