#pragma once

#include "filemanager/folder_size.h"

#include <string>

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
    std::string status;
    std::string details;
  };

  [[nodiscard]] inline PropertyScanPresentation propertyScanPresentation(const FolderSizeResult& result) {
    PropertyScanPresentation presentation;
    presentation.showDisclosure = result.partial();
    if (!presentation.showDisclosure)
      return presentation;
    if (result.failed)
      presentation.status = "Failed.";
    else if (result.cancelled)
      presentation.status = "Cancelled.";
    else
      presentation.status = "Partial.";
    if (!result.reason.empty())
      presentation.details = result.reason;
    for (const auto& diagnostic : result.diagnostics) {
      if (!presentation.details.empty())
        presentation.details += '\n';
      presentation.details += diagnostic.path.empty() ? "." : diagnostic.path;
      if (!diagnostic.operation.empty())
        presentation.details += " — " + diagnostic.operation;
      if (!diagnostic.description.empty())
        presentation.details += ": " + diagnostic.description;
    }
    return presentation;
  }
} // namespace filemanager
