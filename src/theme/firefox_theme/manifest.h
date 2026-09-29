#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace noctalia::theme::firefox_theme::manifest {

  inline constexpr std::string_view kExtensionId = "pywalfox@frewacom.org";
  inline constexpr std::string_view kName = "pywalfox";
  inline constexpr std::string_view kDescription = "Noctalia Firefox theme native messaging host";

  enum class Ownership : std::uint8_t {
    Missing,
    Noctalia,
    Foreign,
  };

  struct Inspection {
    Ownership ownership = Ownership::Missing;
    std::filesystem::path hostPath;
  };

  [[nodiscard]] Inspection inspect(const std::filesystem::path& manifestPath);
  bool install(
      const std::filesystem::path& manifestPath, const std::filesystem::path& hostExecutable,
      std::string* error = nullptr
  );

} // namespace noctalia::theme::firefox_theme::manifest
