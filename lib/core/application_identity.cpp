#include "core/application_identity.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {

  ApplicationIdentity gIdentity;
  bool gIdentitySet = false;

} // namespace

void setApplicationIdentity(ApplicationIdentity identity) {
  gIdentity = std::move(identity);
  gIdentitySet = true;
}

const ApplicationIdentity& applicationIdentity() {
  if (!gIdentitySet) {
    std::fputs("noctalia-ui: setApplicationIdentity() must be called before using the library\n", stderr);
    std::abort();
  }
  return gIdentity;
}

std::string applicationEnvVar(std::string_view suffix) {
  std::string name = applicationIdentity().envPrefix;
  name += '_';
  name += suffix;
  return name;
}
