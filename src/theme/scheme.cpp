#include "theme/scheme.h"

namespace noctalia::theme {

  std::optional<Scheme> schemeFromString(std::string_view s) {
    if (s == "m3-tonal-spot")
      return Scheme::TonalSpot;
    if (s == "m3-content")
      return Scheme::Content;
    if (s == "m3-fruit-salad")
      return Scheme::FruitSalad;
    if (s == "m3-rainbow")
      return Scheme::Rainbow;
    if (s == "m3-monochrome")
      return Scheme::Monochrome;
    if (s == "vibrant")
      return Scheme::Vibrant;
    if (s == "faithful")
      return Scheme::Faithful;
    if (s == "soft")
      return Scheme::Soft;
    if (s == "dysfunctional")
      return Scheme::Dysfunctional;
    if (s == "muted")
      return Scheme::Muted;
    return std::nullopt;
  }

  std::string_view schemeToString(Scheme s) {
    switch (s) {
    case Scheme::TonalSpot:
      return "m3-tonal-spot";
    case Scheme::Content:
      return "m3-content";
    case Scheme::FruitSalad:
      return "m3-fruit-salad";
    case Scheme::Rainbow:
      return "m3-rainbow";
    case Scheme::Monochrome:
      return "m3-monochrome";
    case Scheme::Vibrant:
      return "vibrant";
    case Scheme::Faithful:
      return "faithful";
    case Scheme::Soft:
      return "soft";
    case Scheme::Dysfunctional:
      return "dysfunctional";
    case Scheme::Muted:
      return "muted";
    }
    return "m3-tonal-spot";
  }

} // namespace noctalia::theme
