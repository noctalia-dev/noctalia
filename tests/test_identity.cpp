#include "app/shell_identity.h"

// Linked whole into every test executable: library code reads the application identity, which the
// shell sets in main() and the tests set here before main() runs.
namespace {

  const bool kIdentitySet = [] {
    setApplicationIdentity(shellApplicationIdentity());
    return true;
  }();

} // namespace
