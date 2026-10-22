#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

// Cheap pre-layout text metrics. Panels that must report a preferredHeight before
// the renderer has laid anything out estimate their line count with these; they
// are deliberately approximate and are not a substitute for real text measurement.
namespace ui {

  // Number of lines `text` occupies when wrapped at `charsPerLine`, capped at `maxLines`.
  inline int wrappedLineCount(std::string_view text, int charsPerLine, int maxLines) {
    if (text.empty()) {
      return 0;
    }
    int lines = 0;
    int col = 0;
    for (char ch : text) {
      if (ch == '\n') {
        ++lines;
        col = 0;
        if (lines >= maxLines) {
          return maxLines;
        }
        continue;
      }
      ++col;
      if (charsPerLine > 0 && col > charsPerLine) {
        ++lines;
        col = 1;
        if (lines >= maxLines) {
          return maxLines;
        }
      }
    }
    if (col > 0 || lines == 0) {
      ++lines;
    }
    return std::min(lines, maxLines);
  }

  // Inserts breaks into unbroken runs (long paths, URLs, base64) so they wrap
  // instead of overflowing the panel.
  inline std::string wrapLongRuns(std::string text, std::size_t maxRun = 48) {
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
