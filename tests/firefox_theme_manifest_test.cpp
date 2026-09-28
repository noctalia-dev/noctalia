#include "test_check.h"
#include "theme/firefox_theme/manifest.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

namespace {

  class TempDir {
  public:
    TempDir() {
      std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-firefox-manifest-XXXXXX").string();
      std::array<char, 4096> buffer{};
      TEST_CHECK(pattern.size() < buffer.size());
      std::ranges::copy(pattern, buffer.begin());
      const char* result = ::mkdtemp(buffer.data());
      TEST_CHECK(result != nullptr);
      m_path = result;
    }

    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  void writeManifest(
      const std::filesystem::path& path, std::string_view description, const std::filesystem::path& hostPath
  ) {
    const nlohmann::json body = {
        {"name", "pywalfox"},
        {"description", description},
        {"path", hostPath.string()},
        {"type", "stdio"},
        {"allowed_extensions", nlohmann::json::array({"pywalfox@frewacom.org"})},
    };
    std::ofstream out(path);
    TEST_CHECK(out.good());
    out << body.dump(2) << '\n';
  }

} // namespace

int main() {
  namespace manifest = noctalia::theme::firefox_theme::manifest;

  TempDir temp;
  const auto manifestPath = temp.path() / "pywalfox.json";
  TEST_CHECK(manifest::inspect(manifestPath).ownership == manifest::Ownership::Missing);

  const std::filesystem::path staleNixHost = "/nix/store/old-noctalia-5.1.0/bin/.noctalia-wrapped";
  writeManifest(manifestPath, manifest::kDescription, staleNixHost);
  auto inspection = manifest::inspect(manifestPath);
  TEST_CHECK(inspection.ownership == manifest::Ownership::Noctalia);
  TEST_CHECK(inspection.hostPath == staleNixHost);

  const auto currentNixHost = temp.path() / ".noctalia-wrapped";
  {
    std::ofstream out(currentNixHost);
    TEST_CHECK(out.good());
  }
  std::string error;
  TEST_CHECK(manifest::install(manifestPath, currentNixHost, &error));
  inspection = manifest::inspect(manifestPath);
  TEST_CHECK(inspection.ownership == manifest::Ownership::Noctalia);
  TEST_CHECK(inspection.hostPath == std::filesystem::weakly_canonical(currentNixHost));

  writeManifest(manifestPath, "Noctalia Pywalfox native messaging host", "/usr/bin/noctalia-pywalfox");
  TEST_CHECK(manifest::inspect(manifestPath).ownership == manifest::Ownership::Noctalia);

  writeManifest(manifestPath, "Automatically theme Firefox with Pywal", "/usr/bin/pywalfox");
  inspection = manifest::inspect(manifestPath);
  TEST_CHECK(inspection.ownership == manifest::Ownership::Foreign);
  TEST_CHECK(inspection.hostPath == "/usr/bin/pywalfox");

  {
    std::ofstream out(manifestPath, std::ios::trunc);
    TEST_CHECK(out.good());
    out << "not json\n";
  }
  TEST_CHECK(manifest::inspect(manifestPath).ownership == manifest::Ownership::Foreign);
}
