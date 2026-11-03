#include "i18n/i18n.h"

#include <cstddef>

namespace i18n::detail {

  // Single-pass scanner: copy tmpl into out, and on '{' look for the matching
  // '}' and emit the substituted value if the name is in args. Missing names
  // are left as "{name}" so mismatches are visible on screen. Linear search
  // over args beats a hash map since args almost always has ≤ 3 entries.
  std::string interpolate(std::string_view tmpl, std::span<const Pair> args) {
    std::string out;
    out.reserve(tmpl.size());

    const std::size_t n = tmpl.size();
    std::size_t i = 0;
    while (i < n) {
      const char c = tmpl[i];
      if (c != '{') {
        out.push_back(c);
        ++i;
        continue;
      }

      const std::size_t end = tmpl.find('}', i + 1);
      if (end == std::string_view::npos) {
        out.append(tmpl.substr(i));
        break;
      }

      const std::string_view name = tmpl.substr(i + 1, end - i - 1);
      bool matched = false;
      for (const auto& p : args) {
        if (p.first == name) {
          out.append(p.second);
          matched = true;
          break;
        }
      }
      if (!matched) {
        out.append(tmpl.substr(i, end - i + 1));
      }
      i = end + 1;
    }
    return out;
  }

} // namespace i18n::detail
