#pragma once

#include <filesystem>
#include <string>

namespace noctalia::theme {

  struct KdeColorSchemeApplyResult {
    bool success = false;
    std::string error;
    std::string notificationError;
  };

  [[nodiscard]] bool mergeKdeColorScheme(
      const std::filesystem::path& schemePath, const std::filesystem::path& kdeGlobalsPath, std::string* error
  );

  [[nodiscard]] KdeColorSchemeApplyResult applyKdeColorScheme(const std::filesystem::path& schemePath);

} // namespace noctalia::theme
