#pragma once

#include "filemanager/folder_size.h"

#include <cstdint>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace filemanager {
  struct PropertyNamePresentation {
    std::string text;
    bool editable = true;
  };

  [[nodiscard]] inline PropertyNamePresentation propertyNamePresentation(bool root, const std::string& name) {
    return root ? PropertyNamePresentation{"/", false} : PropertyNamePresentation{name, true};
  }

  struct PropertyScanPresentation {
    bool showDisclosure = false;
    std::string summary;
    std::vector<std::pair<std::string, std::string>> details;
  };

  [[nodiscard]] inline std::string propertyScanCountText(std::uint64_t count) {
    std::ostringstream out;
    try {
      out.imbue(std::locale(""));
    } catch (const std::runtime_error&) {
      // Retain an ungrouped count if the active locale is unavailable.
    }
    out << count;
    return out.str();
  }

  [[nodiscard]] inline PropertyScanPresentation propertyScanPresentation(const FolderSizeResult& result) {
    PropertyScanPresentation presentation;
    presentation.showDisclosure = result.partial();
    if (!presentation.showDisclosure)
      return presentation;
    const auto append = [&](std::uint64_t count, std::string_view singular, std::string_view plural) {
      if (!count)
        return;
      if (!presentation.summary.empty())
        presentation.summary += " · ";
      presentation.summary += propertyScanCountText(count) + " ";
      presentation.summary += count == 1 ? singular : plural;
    };
    if (result.failed)
      presentation.summary = "Scan failed";
    else if (result.cancelled)
      presentation.summary = "Scan cancelled";
    append(result.permissionErrors, "inaccessible path", "inaccessible paths");
    append(result.vanishedEntries, "missing or stale entry", "missing or stale entries");
    append(result.ioErrors, "read error", "read errors");
    const auto categorizedErrors = result.permissionErrors + result.vanishedEntries + result.ioErrors;
    append(
        result.errors > categorizedErrors ? result.errors - categorizedErrors : 0, "other scan error", "other scan errors"
    );
    append(result.excludedMounts, "child mount excluded", "child mounts excluded");
    if (presentation.summary.empty())
      presentation.summary = "Some content was not counted";

    if (!result.reason.empty())
      presentation.details.emplace_back(result.failed ? "Root error" : "Scan note", result.reason);
    if (result.permissionErrors)
      presentation.details.emplace_back("Permission failures", propertyScanCountText(result.permissionErrors));
    if (result.vanishedEntries)
      presentation.details.emplace_back("Missing or stale", propertyScanCountText(result.vanishedEntries));
    if (result.ioErrors)
      presentation.details.emplace_back("Read errors", propertyScanCountText(result.ioErrors));
    if (result.errors > categorizedErrors)
      presentation.details.emplace_back("Other scan errors", propertyScanCountText(result.errors - categorizedErrors));
    if (result.unreadableDirectories)
      presentation.details.emplace_back("Incomplete subtrees", propertyScanCountText(result.unreadableDirectories));
    if (result.excludedMounts)
      presentation.details.emplace_back("Excluded child mounts", propertyScanCountText(result.excludedMounts));
    for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
      const auto& diagnostic = result.diagnostics[i];
      std::string value = diagnostic.path.empty() ? "." : diagnostic.path;
      if (!diagnostic.operation.empty())
        value += "\n" + diagnostic.operation;
      if (!diagnostic.description.empty())
        value += (diagnostic.operation.empty() ? "\n" : ": ") + diagnostic.description;
      if (diagnostic.subtree)
        value += "\nDescendants may be omitted";
      presentation.details.emplace_back("Path " + propertyScanCountText(i + 1), std::move(value));
    }
    return presentation;
  }
} // namespace filemanager
