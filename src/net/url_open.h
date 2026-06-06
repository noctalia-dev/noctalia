#pragma once

#include <string>

namespace net {

  // Open a URL in the user's default browser through xdg-open. Returns true if xdg-open was launched.
  bool openInBrowser(const std::string& url, const std::string& activationToken = {});

} // namespace net
