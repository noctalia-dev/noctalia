#include "shell/greeter/greeter_profile_store.h"

#include "config/atomic_file.h"
#include "config/schema/greeter_schema.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace greeter {
  namespace {
    constexpr std::size_t kProfileLimit = 256U * 1024U;
    std::string format(const GreeterProfile& profile) {
      std::ostringstream out;
      out << toml::toml_formatter{noctalia::config::schema::writeGreeterConfig(profile)};
      return out.str();
    }
  } // namespace

  std::expected<std::filesystem::path, std::string> localProfilePath() {
    std::filesystem::path base;
    if (const auto* state = std::getenv("XDG_STATE_HOME"); state && *state) {
      base = state;
    } else if (const auto* home = std::getenv("HOME"); home && *home) {
      base = std::filesystem::path(home) / ".local/state";
    }
    if (base.empty() || !base.is_absolute()) {
      return std::unexpected("Greeter profile requires an absolute XDG_STATE_HOME or HOME.");
    }
    return base / "noctalia-greeter/greeter.toml";
  }

  std::expected<GreeterProfile, std::string> readLocalProfile(const std::filesystem::path& path) {
    if (path.empty()) {
      return std::unexpected("Greeter profile path is unavailable.");
    }
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !std::filesystem::exists(status))) {
      return GreeterProfile{};
    }
    if (ec || !std::filesystem::is_regular_file(status)) {
      return std::unexpected("Cannot read greeter profile: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return std::unexpected("Cannot open greeter profile: " + path.string());
    }
    std::string text(kProfileLimit + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad()) {
      return std::unexpected("Cannot read greeter profile: " + path.string());
    }
    if (count > kProfileLimit) {
      return std::unexpected("Greeter profile exceeds 256 KiB: " + path.string());
    }
    text.resize(count);
    try {
      const auto table = toml::parse(text, path.string());
      noctalia::config::schema::Diagnostics diagnostics;
      auto profile = noctalia::config::schema::readGreeterConfig(table, {}, diagnostics);
      if (!profile) {
        return std::unexpected(path.string() + ": " + diagnostics.entries.front().describe());
      }
      return *profile;
    } catch (const toml::parse_error& error) {
      return std::unexpected(path.string() + ": " + std::string(error.description()));
    }
  }

  std::expected<void, std::string> writeLocalProfile(const std::filesystem::path& path, const GreeterProfile& profile) {
    const auto existing = readLocalProfile(path);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    noctalia::config::schema::Diagnostics diagnostics;
    if (!noctalia::config::schema::readGreeterConfig(
            noctalia::config::schema::writeGreeterConfig(profile), {}, diagnostics
        )) {
      return std::unexpected(diagnostics.entries.front().describe());
    }
    const auto text = format(profile);
    if (text.size() > kProfileLimit) {
      return std::unexpected("Greeter profile exceeds 256 KiB.");
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    const bool created = std::filesystem::create_directories(parent, ec);
    if (!ec && created) {
      std::filesystem::permissions(parent, std::filesystem::perms::owner_all, ec);
    }
    if (ec || !writeTextFileAtomic(path, text, FileUtils::privateFileMode())) {
      return std::unexpected("Cannot save greeter profile: " + path.string());
    }
    return {};
  }
} // namespace greeter
