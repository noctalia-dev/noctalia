#include "config/config_service.h"

#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <format>
#include <sstream>
#include <string>
#include <utility>

void ConfigService::registerIpc(IpcService& ipc) {
  ipc.bind(nythera::cli::msg::configReload, [this](const std::string&) -> std::string {
    forceReload();
    return "ok\n";
  });

  ipc.bind(nythera::cli::msg::configGet, [this](const std::string& args) -> std::string {
    auto tokens = StringUtils::splitShellWords(args);
    if (!tokens || tokens->size() != 1) {
      return "error: expected exactly one argument: <path>\n";
    }
    const auto& path = (*tokens)[0];
    const auto node = m_configTable->at_path(path);
    if (!node) {
      return std::string("error: path not found: ") + path + "\n";
    }
    std::ostringstream oss;
    oss << node << "\n";
    return oss.str();
  });

  ipc.bind(nythera::cli::msg::configSet, [this](const std::string& args) -> std::string {
    auto tokens = StringUtils::splitShellWords(args);
    if (!tokens || tokens->size() < 2) {
      return "error: expected arguments: <path> <value>\n";
    }
    const std::string path = (*tokens)[0];
    const std::string value = StringUtils::trim(args.substr(path.size()));

    const std::string deltaString = path + "=" + value;
    toml::table delta;
    toml::table candidateOverrides = m_overridesTable;
    try {
      delta = toml::parse(deltaString);
    } catch (const toml::parse_error& e) {
      return std::format("error: invalid TOML value: {}: {}\n", deltaString, e.description());
    }
    deepMerge(candidateOverrides, delta);

    if (candidateOverrides == m_overridesTable) {
      return "ok\n";
    }

    const auto baselineDiagnostics = diagnosticsForOverrides(m_persistedOverridesTable);
    const auto candidateDiagnostics = diagnosticsForOverrides(candidateOverrides);
    const auto introduced = candidateDiagnostics.introducedDiagnosticsComparedTo(baselineDiagnostics);
    if (!introduced.entries.empty()) {
      std::ostringstream oss;
      oss << "error: ";
      bool first = true;
      for (const auto& entry : introduced.entries) {
        if (!first) {
          oss << "; ";
        }
        first = false;
        oss << entry.describe();
      }
      oss << "\n";
      return oss.str();
    }
    if (!commitOverrideTable(
            std::move(candidateOverrides), nullptr,
            {.baseline = &baselineDiagnostics, .candidate = &candidateDiagnostics}
        )) {
      return std::format(
          "error: {}\n", m_lastMutationError.empty() ? "failed to write config overrides" : m_lastMutationError
      );
    }
    return "ok\n";
  });
}
