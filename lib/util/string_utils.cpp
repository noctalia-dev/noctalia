#include "util/string_utils.h"

#include <glib.h>

namespace StringUtils {

  std::string foldForSearch(std::string_view s) {
    if (!g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr)) {
      return toLower(s);
    }
    // Turkish dotted capital I and dotless small i fold to "i̇" and "ı" by default, so neither
    // matches the "i" or "I" people type. Treat all four as plain "i" for searching.
    std::string text = replaceAll(replaceAll(s, "İ", "i"), "ı", "i");
    gchar* folded = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
    // Locale-unaware lowercasing of "İ" (e.g. g_utf8_strdown) gives "i" + U+0307 combining dot above.
    text = replaceAll(folded, "i̇", "i");
    g_free(folded);
    // Compose, so precomposed and decomposed spellings of the same text compare equal.
    gchar* normalized = g_utf8_normalize(text.data(), static_cast<gssize>(text.size()), G_NORMALIZE_DEFAULT_COMPOSE);
    if (normalized == nullptr) {
      return text;
    }
    std::string out(normalized);
    g_free(normalized);
    return out;
  }

} // namespace StringUtils
