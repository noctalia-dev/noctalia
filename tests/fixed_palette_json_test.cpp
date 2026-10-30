#include "render/core/color.h"
#include "test_check.h"
#include "theme/fixed_palette_json.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

namespace {

  using noctalia::theme::parsePaletteFile;

  class TempDir {
  public:
    TempDir() {
      std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-fixed-palette-json-XXXXXX").string();
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

  // A format 1 dark mode with every role except the two whose names changed meaning between formats.
  nlohmann::json version1Mode() {
    return {
        {"mPrimary", "#010101"},
        {"mOnPrimary", "#020202"},
        {"mSecondary", "#030303"},
        {"mOnSecondary", "#040404"},
        {"mTertiary", "#050505"},
        {"mOnTertiary", "#060606"},
        {"mError", "#070707"},
        {"mOnError", "#080808"},
        {"mSurface", "#090909"},
        {"mOnSurface", "#0a0a0a"},
        {"mOnSurfaceVariant", "#0b0b0b"},
        {"mShadow", "#0c0c0c"},
        {"mHover", "#0d0d0d"},
        {"mOnHover", "#0e0e0e"},
        {"terminal",
         {{"foreground", "#0f0f0f"},
          {"selectionFg", "#101010"},
          {"cursorText", "#121212"},
          {"normal", {{"black", "#131313"}}}}},
    };
  }

  // The same colors in format 2.
  nlohmann::json version2Mode() {
    return {
        {"primary", "#010101"},
        {"on_primary", "#020202"},
        {"secondary", "#030303"},
        {"on_secondary", "#040404"},
        {"tertiary", "#050505"},
        {"on_tertiary", "#060606"},
        {"error", "#070707"},
        {"on_error", "#080808"},
        {"surface", "#090909"},
        {"on_surface", "#0a0a0a"},
        {"on_surface_variant", "#0b0b0b"},
        {"shadow", "#0c0c0c"},
        {"hover", "#0d0d0d"},
        {"on_hover", "#0e0e0e"},
        {"terminal",
         {{"foreground", "#0f0f0f"},
          {"selection_fg", "#101010"},
          {"cursor_text", "#121212"},
          {"normal", {{"black", "#131313"}}}}},
    };
  }

  void checkSharedColors(const noctalia::theme::FixedPaletteMode& mode) {
    TEST_CHECK(mode.palette.primary == hex("#010101"));
    TEST_CHECK(mode.palette.onPrimary == hex("#020202"));
    TEST_CHECK(mode.palette.onSurfaceVariant == hex("#0b0b0b"));
    TEST_CHECK(mode.palette.onHover == hex("#0e0e0e"));
    TEST_CHECK(mode.terminal.foreground == hex("#0f0f0f"));
    TEST_CHECK(mode.terminal.selectionFg == hex("#101010"));
    TEST_CHECK(mode.terminal.cursorText == hex("#121212"));
    TEST_CHECK(mode.terminal.normal.black == hex("#131313"));
  }

  std::filesystem::path writePalette(const TempDir& dir, std::string_view name, const nlohmann::json& root) {
    const auto path = dir.path() / (std::string(name) + ".json");
    std::ofstream out(path);
    TEST_CHECK(out.good());
    out << root.dump(2) << '\n';
    return path;
  }

} // namespace

int main() {
  TempDir dir;

  // Format 1 (no "version"): `m`-prefixed camelCase keys, with mSurfaceVariant and mOutline feeding the container
  // and outline variant roles. An mOutlineVariant that meant nothing in format 1 is ignored.
  {
    nlohmann::json dark = version1Mode();
    dark["mSurfaceVariant"] = "#111111";
    dark["mOutline"] = "#222222";
    dark["mOutlineVariant"] = "#333333";
    const auto parsed = parsePaletteFile(writePalette(dir, "v1", {{"dark", dark}}));
    TEST_CHECK(parsed.has_value());
    checkSharedColors(parsed->dark);
    TEST_CHECK(parsed->dark.palette.surfaceContainer == hex("#111111"));
    TEST_CHECK(parsed->dark.palette.outlineVariant == hex("#222222"));
    TEST_CHECK(parsed->light.palette.surfaceContainer == hex("#111111"));
  }

  // Format 1 readers also accepted the names without the `m` prefix.
  {
    nlohmann::json dark = version1Mode();
    dark.erase("mOnPrimary");
    dark["onPrimary"] = "#020202";
    dark["surfaceVariant"] = "#121212";
    dark["outline"] = "#232323";
    const auto parsed = parsePaletteFile(writePalette(dir, "v1-unprefixed", {{"dark", dark}}));
    TEST_CHECK(parsed.has_value());
    checkSharedColors(parsed->dark);
    TEST_CHECK(parsed->dark.palette.surfaceContainer == hex("#121212"));
    TEST_CHECK(parsed->dark.palette.outlineVariant == hex("#232323"));
  }

  // Format 2 reads snake_case role names and ignores format 1 names.
  {
    nlohmann::json dark = version2Mode();
    dark["surface_container"] = "#444444";
    dark["outline_variant"] = "#555555";
    dark["mSurfaceVariant"] = "#999999";
    dark["mPrimary"] = "#888888";
    const auto parsed = parsePaletteFile(writePalette(dir, "v2", {{"version", 2}, {"dark", dark}}));
    TEST_CHECK(parsed.has_value());
    checkSharedColors(parsed->dark);
    TEST_CHECK(parsed->dark.palette.surfaceContainer == hex("#444444"));
    TEST_CHECK(parsed->dark.palette.outlineVariant == hex("#555555"));
  }

  // Newer and malformed versions are rejected rather than read with the wrong key names.
  {
    const nlohmann::json dark = version2Mode();
    TEST_CHECK(!parsePaletteFile(writePalette(dir, "v3", {{"version", 3}, {"dark", dark}})).has_value());
    TEST_CHECK(!parsePaletteFile(writePalette(dir, "string", {{"version", "2"}, {"dark", dark}})).has_value());
    TEST_CHECK(!parsePaletteFile(writePalette(dir, "zero", {{"version", 0}, {"dark", dark}})).has_value());
  }
}
