#include "util/clamp.h"

#include "core/log.h"

#include <mutex>
#include <set>
#include <utility>

namespace util::detail {

  void reportInvertedClamp(double lo, double hi, const std::source_location& loc) {
    // file_name() is a stable string-literal pointer per site; (pointer, line)
    // keys the site so we warn once and never spam a per-frame layout path.
    static std::mutex mutex;
    static std::set<std::pair<const char*, std::uint_least32_t>> seen;

    {
      const std::scoped_lock lock(mutex);
      if (!seen.emplace(loc.file_name(), loc.line()).second) {
        return;
      }
    }

    logWarn(
        "clampOrdered: inverted range at {}:{} (lo={}, hi={}); returning lo. Fix the bounds math at this site.",
        loc.file_name(), loc.line(), lo, hi
    );
  }

} // namespace util::detail
