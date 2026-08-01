#pragma once

namespace noctalia::completions {

  // Entry point for `noctalia completions <shell>`.
  // argv[2] selects the shell, remaining args are only ever --help.
  int runCli(int argc, char* argv[]);

} // namespace noctalia::completions
