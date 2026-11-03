#include "util/string_utils.h"

#include <glib.h>
#include <memory>

namespace StringUtils {

  namespace {

    struct GFreeDeleter {
      void operator()(gchar* p) const { g_free(p); }
    };
    using OwnedGStr = std::unique_ptr<gchar, GFreeDeleter>;

    // Combining Diacritical Marks: the accents of Latin, Greek and Cyrillic letters once decomposed.
    // Marks of other scripts (e.g. Japanese dakuten) distinguish letters and are kept.
    bool isCombiningAccent(gunichar c) { return c >= 0x0300 && c <= 0x036F; }

  } // namespace

  std::string normalizeForSearch(std::string_view s) {
    if (!g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr)) {
      return toLower(s);
    }
    // Dotless ı has no decomposition, so map it to the "i" people type. Dotted İ needs no special
    // case: it folds to "i" + U+0307, whose dot is dropped below with the other accents.
    const std::string text = replaceAll(s, "ı", "i");
    const OwnedGStr folded(g_utf8_casefold(text.data(), static_cast<gssize>(text.size())));
    const OwnedGStr decomposed(g_utf8_normalize(folded.get(), -1, G_NORMALIZE_DEFAULT));
    std::string stripped;
    stripped.reserve(text.size());
    for (const gchar* p = decomposed.get(); *p != '\0';) {
      const gchar* next = g_utf8_next_char(p);
      if (!isCombiningAccent(g_utf8_get_char(p))) {
        stripped.append(p, next);
      }
      p = next;
    }
    // Recompose what remains (e.g. Hangul syllables) so the key stays in canonical form.
    const OwnedGStr composed(
        g_utf8_normalize(stripped.data(), static_cast<gssize>(stripped.size()), G_NORMALIZE_DEFAULT_COMPOSE)
    );
    return composed.get();
  }

} // namespace StringUtils
