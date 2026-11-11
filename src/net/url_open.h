#pragma once

#include <string>

namespace net {

  // Queue a URL launch through the system default handler. Launch failures are logged asynchronously.
  bool openInBrowser(const std::string& url, const std::string& activationToken = {});

} // namespace net
