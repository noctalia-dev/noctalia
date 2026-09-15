#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace compositors::kde {

  struct KwinTrackedWindow {
    std::string uuid;
    std::string appId;
    std::string title;
    std::string outputName;
    std::vector<std::string> desktopIds;
    bool minimized = false;
  };

  [[nodiscard]] std::vector<KwinTrackedWindow> parseWindowListPayload(std::string_view payload);

} // namespace compositors::kde
