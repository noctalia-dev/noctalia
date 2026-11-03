#pragma once

#include <filesystem>
#include <string>

namespace noctalia::theme {

  struct [[nodiscard]] KdeColorSchemeApplyResult {
    bool success = false;
    std::string error;
    std::string notificationError;
  };

  [[nodiscard]] bool mergeKdeColorScheme(
      const std::filesystem::path& schemePath, const std::filesystem::path& kdeGlobalsPath, std::string* error
  );

  KdeColorSchemeApplyResult applyKdeColorScheme(const std::filesystem::path& schemePath);

} // namespace noctalia::theme
