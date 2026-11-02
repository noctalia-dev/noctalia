#include "app/build_info.h"

#include "noctalia_git_revision.h"

namespace noctalia::build_info {

  std::string_view version() noexcept { return NOCTALIA_VERSION; }

  std::string_view revision() noexcept { return NOCTALIA_GIT_REVISION; }

  std::string displayVersion() {
    std::string label = "v";
    label += version();

    constexpr std::string_view unknownRevision = "unknown";
    const std::string_view rev = revision();
    if (!rev.empty() && rev != unknownRevision) {
      label += " (";
      label += rev;
      label += ')';
    }
    return label;
  }

} // namespace noctalia::build_info
