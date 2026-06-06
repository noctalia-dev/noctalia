#include "net/url_open.h"

#include "core/log.h"
#include "core/process.h"

#include <vector>

namespace {
  constexpr Logger kLog("url-open");
}

namespace net {

  bool openInBrowser(const std::string& url, const std::string& activationToken) {
    if (!process::commandExists("xdg-open")) {
      kLog.warn("browser opener command not found: xdg-open");
      return false;
    }

    return process::runAsync(std::vector<std::string>{"xdg-open", url}, activationToken);
  }

} // namespace net
