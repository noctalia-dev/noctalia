#pragma once

#include <cctype>
#include <cstddef>
#include <string>

// Text preparation shared by the polkit and secret prompt panels.
namespace ui {

  // Inserts breaks into unbroken runs (long paths, URLs, base64) so they wrap
  // instead of overflowing the panel.
  inline std::string wrapLongRuns(const std::string& text, std::size_t maxRun = 48) {
    std::string out;
    out.reserve(text.size() + text.size() / maxRun);
    std::size_t run = 0;
    for (char ch : text) {
      const bool breakable = std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '/' || ch == ':' || ch == '-';
      out.push_back(ch);
      if (breakable) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= maxRun) {
        out.push_back('\n');
        run = 0;
      }
    }
    return out;
  }

} // namespace ui
