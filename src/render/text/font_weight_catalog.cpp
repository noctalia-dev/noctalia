#include "render/text/font_weight_catalog.h"

#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <fontconfig/fontconfig.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace text {
  namespace {

    constexpr Logger kLog("font-weight-catalog");
    constexpr int kOpenTypeWeightMatchTolerance = 50;

    constexpr FontWeight kPresetLabelFontWeights[] = {
        FontWeight::Thin, FontWeight::UltraLight, FontWeight::Light,  FontWeight::SemiLight,
        FontWeight::Book, FontWeight::Normal,     FontWeight::Medium, FontWeight::SemiBold,
        FontWeight::Bold, FontWeight::UltraBold,  FontWeight::Heavy,  FontWeight::UltraHeavy,
    };

    struct CatalogEntry {
      std::vector<FontWeight> weights;
      bool variable = false;
    };

    std::unordered_map<std::string, CatalogEntry> g_cache;

    [[nodiscard]] std::string normalizeFamilyKey(std::string_view fontFamily) {
      std::string key = StringUtils::trim(std::string(fontFamily));
      if (key.empty()) {
        key = "sans-serif";
      }
      return StringUtils::toLower(std::move(key));
    }

    [[nodiscard]] std::vector<FontWeight> allPresetLabelFontWeights() {
      return {std::begin(kPresetLabelFontWeights), std::end(kPresetLabelFontWeights)};
    }

    [[nodiscard]] bool ensureFontConfigReady() {
      if (!FcInit()) {
        kLog.warn("fontconfig initialization failed; exposing all preset font weights");
        return false;
      }
      return true;
    }

    [[nodiscard]] std::optional<FontWeight> nearestPresetForOpenTypeWeight(int openTypeWeight) {
      std::optional<FontWeight> best;
      int bestDistance = std::numeric_limits<int>::max();
      for (FontWeight candidateWeight : kPresetLabelFontWeights) {
        const int candidate = static_cast<int>(candidateWeight);
        const int distance = std::abs(candidate - openTypeWeight);
        if (distance < bestDistance) {
          bestDistance = distance;
          best = candidateWeight;
        }
      }
      if (!best.has_value() || bestDistance > kOpenTypeWeightMatchTolerance) {
        return std::nullopt;
      }
      return best;
    }

    [[nodiscard]] FcFontSet* listFontsForFamilyPattern(FcPattern* pattern, FcObjectSet* objectSet) {
      if (pattern == nullptr || objectSet == nullptr) {
        return nullptr;
      }
      return FcFontList(nullptr, pattern, objectSet);
    }

    [[nodiscard]] FcFontSet* listFontsForFamilyName(const std::string& family, FcObjectSet* objectSet) {
      if (family.empty() || objectSet == nullptr) {
        return nullptr;
      }

      FcPattern* pattern = FcPatternBuild(nullptr, FC_FAMILY, FcTypeString, family.c_str(), nullptr);
      if (pattern == nullptr) {
        return nullptr;
      }

      FcFontSet* fonts = listFontsForFamilyPattern(pattern, objectSet);
      if (fonts != nullptr && fonts->nfont > 0) {
        FcPatternDestroy(pattern);
        return fonts;
      }
      if (fonts != nullptr) {
        FcFontSetDestroy(fonts);
      }

      // Generic aliases such as sans-serif need pattern substitution, but FcMatchFont
      // collapses the pattern to a single face and must not run before FcFontList.
      FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
      FcResult matchResult;
      FcPattern* matched = FcFontMatch(nullptr, pattern, &matchResult);
      if (matched != nullptr) {
        FcChar8* resolvedFamily = nullptr;
        if (FcPatternGetString(matched, FC_FAMILY, 0, &resolvedFamily) == FcResultMatch
            && resolvedFamily != nullptr
            && resolvedFamily[0] != '\0') {
          FcPattern* resolvedPattern =
              FcPatternBuild(nullptr, FC_FAMILY, FcTypeString, reinterpret_cast<const char*>(resolvedFamily), nullptr);
          if (resolvedPattern != nullptr) {
            fonts = listFontsForFamilyPattern(resolvedPattern, objectSet);
            FcPatternDestroy(resolvedPattern);
          }
        }
        FcPatternDestroy(matched);
      }

      FcPatternDestroy(pattern);
      return fonts;
    }

    [[nodiscard]] CatalogEntry discoverCatalogEntry(std::string_view fontFamily) {
      CatalogEntry entry{};
      entry.weights = allPresetLabelFontWeights();

      if (!ensureFontConfigReady()) {
        entry.variable = true;
        return entry;
      }

      const std::string family = StringUtils::trim(std::string(fontFamily));
      const std::string queryFamily = family.empty() ? "sans-serif" : family;

      FcObjectSet* objectSet = FcObjectSetBuild(FC_WEIGHT, FC_VARIABLE, nullptr);
      if (objectSet == nullptr) {
        entry.variable = true;
        return entry;
      }

      FcFontSet* fonts = listFontsForFamilyName(queryFamily, objectSet);
      FcObjectSetDestroy(objectSet);

      if (fonts == nullptr || fonts->nfont == 0) {
        if (fonts != nullptr) {
          FcFontSetDestroy(fonts);
        }
        kLog.debug("no fontconfig faces for family '{}'; exposing regular weight only", queryFamily);
        entry.weights = {FontWeight::Normal};
        return entry;
      }

      std::unordered_set<int> openTypeWeights;
      openTypeWeights.reserve(static_cast<std::size_t>(fonts->nfont));

      for (int i = 0; i < fonts->nfont; ++i) {
        FcPattern* font = fonts->fonts[i];
        if (font == nullptr) {
          continue;
        }

        FcBool isVariable = FcFalse;
        if (FcPatternGetBool(font, FC_VARIABLE, 0, &isVariable) == FcResultMatch && isVariable) {
          entry.variable = true;
          break;
        }

        int fcWeight = FC_WEIGHT_REGULAR;
        if (FcPatternGetInteger(font, FC_WEIGHT, 0, &fcWeight) == FcResultMatch) {
          openTypeWeights.insert(FcWeightToOpenType(fcWeight));
        }
      }
      FcFontSetDestroy(fonts);

      if (entry.variable) {
        entry.weights = allPresetLabelFontWeights();
        return entry;
      }

      std::unordered_set<int> seenPresetWeights;
      std::vector<FontWeight> matched;
      matched.reserve(std::size(kPresetLabelFontWeights));

      for (int openTypeWeight : openTypeWeights) {
        if (const auto preset = nearestPresetForOpenTypeWeight(openTypeWeight); preset.has_value()) {
          const int presetValue = static_cast<int>(*preset);
          if (seenPresetWeights.insert(presetValue).second) {
            matched.push_back(*preset);
          }
        }
      }

      std::ranges::sort(matched, [](FontWeight a, FontWeight b) { return static_cast<int>(a) < static_cast<int>(b); });

      if (matched.empty()) {
        matched.push_back(FontWeight::Normal);
      }

      entry.weights = std::move(matched);
      return entry;
    }

    [[nodiscard]] const CatalogEntry& catalogEntryForFamily(std::string_view fontFamily) {
      const std::string cacheKey = normalizeFamilyKey(fontFamily);
      const auto cached = g_cache.find(cacheKey);
      if (cached != g_cache.end()) {
        return cached->second;
      }

      CatalogEntry discovered = discoverCatalogEntry(fontFamily);
      const auto [it, _] = g_cache.emplace(cacheKey, std::move(discovered));
      return it->second;
    }

  } // namespace

  void invalidateFontWeightCatalogCache() { g_cache.clear(); }

  bool fontFamilySupportsVariableWeight(std::string_view fontFamily) {
    return catalogEntryForFamily(fontFamily).variable;
  }

  std::vector<FontWeight> availableLabelFontWeights(std::string_view fontFamily) {
    return catalogEntryForFamily(fontFamily).weights;
  }

} // namespace text
