#pragma once

#include "i18n/i18n.h"
#include "util/file_utils.h"

#include <string>
#include <vector>

namespace noctalia::theme {

  struct BuiltinTemplateInfo {
    std::string id;
    std::string name;
    std::string category;
    std::string documentationUrl;
    std::vector<std::string> outputPaths;
    bool outputDynamic = false;
    std::string outputPathDynamicCommand;
  };

  struct AvailableTemplate {
    std::string id;          // canonical TOML value (what gets written to config)
    std::string displayName; // friendly label for the GUI; falls back to id when not provided
    std::string category;
    std::string documentationUrl;
    std::vector<std::string> outputPaths;
    bool outputDynamic = false;
  };

  // Loads the built-in template catalog from the shipped assets/templates/builtin.toml.
  // On parse failure, sets `err` (when non-null) and returns an empty list.
  // Entries are sorted by category, then id.
  [[nodiscard]] std::vector<BuiltinTemplateInfo> loadBuiltinTemplateInfo(std::string* err = nullptr);

  // Every built-in template the user can opt into.
  [[nodiscard]] std::vector<AvailableTemplate> availableTemplates();

  // Build a user-facing tooltip string describing the files a template will overwrite.
  [[nodiscard]] inline std::string formatTemplateTooltip(const AvailableTemplate& t) {
    if (t.outputPaths.empty() && !t.outputDynamic) {
      return {};
    }
    std::string tip;
    tip.reserve(256);
    const auto appendPath = [&](const std::string& path) {
      if (!tip.empty()) {
        tip += '\n';
      }
      tip += FileUtils::xdgPathForDisplay(path);
    };
    for (const auto& path : t.outputPaths) {
      if (FileUtils::isOutputPathApplicable(path)) {
        appendPath(path);
      }
    }
    if (tip.empty()) {
      for (const auto& path : t.outputPaths) {
        appendPath(path);
      }
    }
    if (t.outputDynamic) {
      if (!tip.empty()) {
        tip += '\n';
      }
      tip += i18n::tr("settings.schema.templates.dynamic-output");
    }
    return tip;
  }

} // namespace noctalia::theme
