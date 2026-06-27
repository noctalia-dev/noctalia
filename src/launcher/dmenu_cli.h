#pragma once

namespace noctalia::launcher {

  // `noctalia dmenu` — stdin/stdout drop-in. Reads newline-separated candidates from
  // stdin, asks the running shell to present them in the launcher, and prints the
  // selected line to stdout (exit 0). Cancel (Esc / panel closed) prints nothing and
  // exits 1. Options: -p <prompt> (optional label, unused by the shell UI today).
  int runDmenuCli(int argc, char** argv);

} // namespace noctalia::launcher
