#include "theme/firefox_theme/manifest.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace noctalia::theme::firefox_theme::manifest {
  namespace {

    constexpr std::string_view kLegacyDescription = "Noctalia Pywalfox native messaging host";

    [[nodiscard]] bool isNoctaliaDescription(std::string_view description) {
      return description == kDescription || description == kLegacyDescription;
    }

  } // namespace

  Inspection inspect(const std::filesystem::path& manifestPath) {
    std::error_code ec;
    if (!std::filesystem::exists(manifestPath, ec) && !ec) {
      return {};
    }

    std::ifstream in(manifestPath);
    if (!in) {
      return {.ownership = Ownership::Foreign};
    }

    try {
      nlohmann::json root;
      in >> root;

      Inspection result{.ownership = Ownership::Foreign};
      if (const auto path = root.find("path"); path != root.end() && path->is_string()) {
        result.hostPath = path->get<std::string>();
      }
      if (const auto description = root.find("description"); description != root.end()
          && description->is_string()
          && isNoctaliaDescription(description->get_ref<const std::string&>())) {
        result.ownership = Ownership::Noctalia;
      }
      return result;
    } catch (...) {
      return {.ownership = Ownership::Foreign};
    }
  }

  bool
  install(const std::filesystem::path& manifestPath, const std::filesystem::path& hostExecutable, std::string* error) {
    if (hostExecutable.empty() || !std::filesystem::is_regular_file(hostExecutable)) {
      if (error != nullptr) {
        *error = "noctalia executable not found";
      }
      return false;
    }

    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(hostExecutable, ec);
    const auto path = ec ? hostExecutable : canonical;

    std::filesystem::create_directories(manifestPath.parent_path(), ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create native-messaging-hosts directory: " + ec.message();
      }
      return false;
    }

    const nlohmann::json body = {
        {"name", std::string(kName)},
        {"description", std::string(kDescription)},
        {"path", path.string()},
        {"type", "stdio"},
        {"allowed_extensions", nlohmann::json::array({std::string(kExtensionId)})},
    };

    std::ofstream out(manifestPath, std::ios::trunc);
    if (!out) {
      if (error != nullptr) {
        *error = "failed to write " + manifestPath.string();
      }
      return false;
    }
    out << body.dump(2) << '\n';
    return true;
  }

} // namespace noctalia::theme::firefox_theme::manifest
