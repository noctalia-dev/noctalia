#include "app/shell_library.h"

// Linked whole into every test of shell code: configures the shared UI library exactly as the
// shell's main() does, before main() runs.
namespace {

  const bool kConfigured = [] {
    configureSharedLibrary();
    return true;
  }();

} // namespace
