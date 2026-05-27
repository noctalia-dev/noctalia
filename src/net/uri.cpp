#include "net/uri.h"

#include <cctype>

namespace {

  int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
      return 10 + (ch - 'a');
    }
    return -1;
  }

} // namespace

namespace uri {

  std::string decodeComponent(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size()) {
        const int hi = hexValue(text[i + 1]);
        const int lo = hexValue(text[i + 2]);
        if (hi >= 0 && lo >= 0) {
          decoded.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      decoded.push_back(text[i]);
    }
    return decoded;
  }

  bool isRemoteUrl(std::string_view url) { return url.starts_with("https://") || url.starts_with("http://"); }

  std::string normalizeFileUrl(std::string_view url) {
    if (url.empty() || isRemoteUrl(url)) {
      return {};
    }
    std::string path(url);
    constexpr std::string_view prefix = "file://";
    if (path.starts_with(prefix)) {
      path.erase(0, prefix.size());
      if (path.starts_with("localhost/")) {
        path.erase(0, std::string_view("localhost").size());
      } else if (!path.empty() && path.front() != '/') {
        const auto firstSlash = path.find('/');
        path = firstSlash == std::string::npos ? std::string{} : path.substr(firstSlash);
      }
    }
    return decodeComponent(path);
  }

} // namespace uri
