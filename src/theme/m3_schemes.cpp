#include "cpp/cam/cam.h"
#include "cpp/cam/hct.h"
#include "cpp/dynamiccolor/dynamic_scheme.h"
#include "cpp/dynamiccolor/material_dynamic_colors.h"
#include "cpp/quantize/lab.h"
#include "cpp/quantize/wu.h"
#include "cpp/scheme/scheme_content.h"
#include "cpp/scheme/scheme_fruit_salad.h"
#include "cpp/scheme/scheme_monochrome.h"
#include "cpp/scheme/scheme_rainbow.h"
#include "cpp/scheme/scheme_tonal_spot.h"
#include "cpp/utils/utils.h"
#include "theme/fixed_palette.h"
#include "theme/palette.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Material Design 3 scheme path. Quantize the resized pixels, score for the
// dominant colourful seed, build a DynamicScheme, then pull each token via
// MaterialDynamicColors. Matches matugen's pipeline (Rust material_colors
// crate) one-for-one.

namespace mcu = material_color_utilities;

namespace noctalia::theme {

  namespace {

    // ─── Matugen-faithful quant + score ────────────────────────────────
    //
    // MCU's QuantizeCelebi → RankedSuggestions diverges from the Rust
    // material_colors crate (matugen's underlying lib) in two subtle places
    // that swap seed picks on certain images:
    //
    //   1. MCU's wsmeans.cc sorts swatches by population descending and then
    //      stores them in std::map<Argb,uint32_t>, so iteration order is
    //      key-sorted (alpha by argb). The Rust version skips the sort and
    //      uses IndexMap (insertion order = cluster index order from wu).
    //   2. MCU's score.cc uses unstable std::sort to rank scored HCTs;
    //      the Rust version uses stable sort_by.
    //
    // Same constants, same math otherwise. We fork wsmeans-result-build and
    // score here so the seed matches matugen byte-for-byte. Wu can stay as
    // MCU's — it's purely numeric and produces identical output.

    struct ClusterEntry {
      mcu::Argb argb;
      uint32_t population;
    };

    struct DistanceToIndex {
      double distance = 0.0;
      std::size_t index = 0;
      bool operator<(const DistanceToIndex& a) const { return distance < a.distance; }
    };

    // Hand-port of material_colors Rust QuantizerWsmeans::quantize. Differs
    // from MCU's wsmeans.cc in five places that all matter for output parity:
    //
    //   • 10 iterations max, not 100.
    //   • cluster_indices initialised to i % cluster_count (deterministic),
    //     not rand() % cluster_count.
    //   • Point reassignment is unconditional when a closer cluster exists,
    //     no kMinDeltaE = 3.0 threshold.
    //   • Termination uses points_moved count, not a "did anything change" bool.
    //   • cluster_count is NOT additionally clamped to starting_clusters.len()
    //     (matugen will index OOB if you give it fewer seeds than requested).
    //
    // The final assembly also preserves cluster index order (matches Rust
    // IndexMap insertion order) instead of population-sorting + std::map.
    std::vector<ClusterEntry> wsmeansMatugenOrder(
        const std::vector<mcu::Argb>& input_pixels, const std::vector<mcu::Argb>& starting_clusters, uint16_t max_colors
    ) {
      if (max_colors == 0 || input_pixels.empty())
        return {};
      max_colors = std::min<uint16_t>(max_colors, 256);

      // Dedupe input pixels in insertion order, building parallel
      // pixels/points/counts arrays. Matches matugen's IndexMap loop.
      std::unordered_map<mcu::Argb, std::uint32_t> pixel_to_count;
      std::vector<mcu::Argb> pixels;
      std::vector<mcu::Lab> points;
      pixels.reserve(input_pixels.size());
      points.reserve(input_pixels.size());
      for (mcu::Argb pixel : input_pixels) {
        auto it = pixel_to_count.find(pixel);
        if (it != pixel_to_count.end()) {
          it->second++;
        } else {
          pixels.push_back(pixel);
          points.push_back(mcu::LabFromInt(pixel));
          pixel_to_count[pixel] = 1;
        }
      }

      std::size_t cluster_count = std::min(static_cast<std::size_t>(max_colors), points.size());

      std::vector<mcu::Lab> clusters;
      clusters.reserve(starting_clusters.size());
      for (mcu::Argb argb : starting_clusters)
        clusters.push_back(mcu::LabFromInt(argb));
      // matugen relies on Wu returning max_colors entries and would index
      // OOB otherwise. Clamp here (matches MCU's wsmeans.cc) so we don't
      // crash on low-cardinality images where Wu returns fewer cubes; the
      // result still matches matugen for the common case.
      if (!starting_clusters.empty())
        cluster_count = std::min(cluster_count, starting_clusters.size());

      // Deterministic init, no rand.
      std::vector<std::size_t> cluster_indices;
      cluster_indices.reserve(points.size());
      for (std::size_t i = 0; i < points.size(); i++)
        cluster_indices.push_back(i % cluster_count);

      std::vector<std::vector<DistanceToIndex>> dmat(cluster_count, std::vector<DistanceToIndex>(cluster_count));
      std::array<std::uint32_t, 256> pixel_count_sums{};

      constexpr std::size_t kMaxIterations = 10;

      for (std::size_t iter = 0; iter < kMaxIterations; iter++) {
        // Build pairwise cluster distance matrix, then sort each row IN
        // PLACE by distance ascending. NB: matugen does the same in-place
        // sort, which has a curious side effect — see the reassign loop.
        for (std::size_t i = 0; i < cluster_count; i++) {
          for (std::size_t j = i + 1; j < cluster_count; j++) {
            double d = clusters[i].DeltaE(clusters[j]);
            dmat[j][i] = {d, i};
            dmat[i][j] = {d, j};
          }
        }
        for (std::size_t i = 0; i < cluster_count; i++) {
          std::sort(dmat[i].begin(), dmat[i].end());
        }

        int points_moved = 0;
        for (std::size_t i = 0; i < points.size(); i++) {
          mcu::Lab point = points[i];
          std::size_t prev_idx = cluster_indices[i];
          double prev_d = point.DeltaE(clusters[prev_idx]);
          double min_d = prev_d;
          std::size_t new_idx = cluster_count;
          // Quirk match: matugen's reassign loop reads dmat[prev_idx][j]
          // (the j-th SMALLEST distance from prev_idx, after the in-place
          // sort) but compares against clusters[j] (the j-th cluster by
          // original index). The j on each side refers to a different
          // thing; the early-out cutoff is monotone in j, so the loop ends
          // up only checking the first N clusters (in original order) where
          // N is the count of cluster pairs within 4× of prev_d. We replicate
          // the same indexing for byte-for-byte parity.
          for (std::size_t j = 0; j < cluster_count; j++) {
            if (dmat[prev_idx][j].distance >= 4.0 * prev_d)
              continue;
            double d = point.DeltaE(clusters[j]);
            if (d < min_d) {
              min_d = d;
              new_idx = j;
            }
          }
          if (new_idx != cluster_count) {
            points_moved++;
            cluster_indices[i] = new_idx;
          }
        }
        if (points_moved == 0 && iter > 0)
          break;

        std::array<double, 256> sa{}, sb{}, sc{};
        for (std::size_t i = 0; i < cluster_count; i++)
          pixel_count_sums[i] = 0;
        for (std::size_t i = 0; i < points.size(); i++) {
          const std::size_t ci = cluster_indices[i];
          const std::uint32_t cnt = pixel_to_count[pixels[i]];
          pixel_count_sums[ci] += cnt;
          sa[ci] += points[i].l * static_cast<double>(cnt);
          sb[ci] += points[i].a * static_cast<double>(cnt);
          sc[ci] += points[i].b * static_cast<double>(cnt);
        }
        for (std::size_t i = 0; i < cluster_count; i++) {
          const std::uint32_t cnt = pixel_count_sums[i];
          if (cnt == 0) {
            clusters[i] = {0, 0, 0};
            continue;
          }
          clusters[i] = {
              sa[i] / static_cast<double>(cnt), sb[i] / static_cast<double>(cnt), sc[i] / static_cast<double>(cnt)
          };
        }
      }

      // Cluster index order, dedupe by argb, drop empties. Matches matugen
      // wsmeans.rs:275-294 exactly (no population sort, no std::map rekey).
      std::vector<ClusterEntry> result;
      for (std::size_t i = 0; i < cluster_count; i++) {
        const std::uint32_t cnt = pixel_count_sums[i];
        if (cnt == 0)
          continue;
        mcu::Argb argb = mcu::IntFromLab(clusters[i]);
        bool dup = false;
        for (auto& e : result) {
          if (e.argb == argb) {
            dup = true;
            break;
          }
        }
        if (dup)
          continue;
        result.push_back({argb, cnt});
      }
      return result;
    }

    // Hand-port of material_colors Rust Score::score (score.rs). Identical
    // math to MCU's RankedSuggestions, but iterates clusters in input order
    // and uses std::stable_sort.
    std::vector<mcu::Argb> scoreMatugen(const std::vector<ClusterEntry>& clusters, int desired, bool filter) {
      constexpr double kTargetChroma = 48.0;
      constexpr double kWeightProportion = 0.7;
      constexpr double kWeightChromaAbove = 0.3;
      constexpr double kWeightChromaBelow = 0.1;
      constexpr double kCutoffChroma = 5.0;
      constexpr double kCutoffExcitedProportion = 0.01;

      std::vector<mcu::Hct> colors_hct;
      colors_hct.reserve(clusters.size());
      constexpr std::size_t kHueCount = 360;
      std::vector<uint32_t> hue_population(kHueCount, 0);
      double population_sum = 0.0;
      for (const auto& c : clusters) {
        mcu::Hct hct(c.argb);
        colors_hct.push_back(hct);
        const int hue = mcu::SanitizeDegreesInt(static_cast<int>(std::floor(hct.get_hue())));
        hue_population[static_cast<std::size_t>(hue)] += c.population;
        population_sum += c.population;
      }

      std::vector<double> hue_excited(kHueCount, 0.0);
      for (std::size_t hue = 0; hue < kHueCount; hue++) {
        double prop = hue_population[hue] / population_sum;
        const int hueInt = static_cast<int>(hue);
        for (int i = hueInt - 14; i < hueInt + 16; i++) {
          int nh = mcu::SanitizeDegreesInt(i);
          hue_excited[static_cast<std::size_t>(nh)] += prop;
        }
      }

      struct Scored {
        mcu::Hct hct;
        double score;
      };
      std::vector<Scored> scored;
      scored.reserve(colors_hct.size());
      for (mcu::Hct hct : colors_hct) {
        int hue = mcu::SanitizeDegreesInt(static_cast<int>(std::round(hct.get_hue())));
        double prop = hue_excited[static_cast<std::size_t>(hue)];
        if (filter && (hct.get_chroma() < kCutoffChroma || prop <= kCutoffExcitedProportion))
          continue;
        double prop_score = prop * 100.0 * kWeightProportion;
        double cw = hct.get_chroma() < kTargetChroma ? kWeightChromaBelow : kWeightChromaAbove;
        double chroma_score = (hct.get_chroma() - kTargetChroma) * cw;
        scored.push_back({hct, prop_score + chroma_score});
      }
      // STABLE sort: matches Rust sort_by, preserves cluster insertion order
      // when scores tie.
      std::ranges::stable_sort(scored, std::ranges::greater{}, &Scored::score);

      std::vector<mcu::Hct> chosen;
      for (int diff = 90; diff >= 15; diff--) {
        chosen.clear();
        for (const auto& e : scored) {
          mcu::Hct hct = e.hct;
          bool dup = false;
          for (const auto& ch : chosen) {
            if (mcu::DiffDegrees(hct.get_hue(), ch.get_hue()) < diff) {
              dup = true;
              break;
            }
          }
          if (!dup) {
            chosen.push_back(hct);
            if (static_cast<int>(chosen.size()) >= desired)
              break;
          }
        }
        if (static_cast<int>(chosen.size()) >= desired)
          break;
      }

      std::vector<mcu::Argb> out;
      if (chosen.empty())
        out.push_back(0xff4285f4u);
      for (auto& h : chosen)
        out.push_back(h.ToInt());
      return out;
    }

    // Build pixels → wu seeds → wsmeans (matugen order) → chroma filter →
    // matugen-style score → first ranked.
    mcu::Argb extractSeed(const std::vector<uint8_t>& rgb112) {
      std::vector<mcu::Argb> pixels;
      pixels.reserve(rgb112.size() / 3);
      for (size_t i = 0; i + 2 < rgb112.size(); i += 3) {
        const uint32_t r = rgb112[i + 0];
        const uint32_t g = rgb112[i + 1];
        const uint32_t b = rgb112[i + 2];
        pixels.push_back(0xff000000u | (r << 16) | (g << 8) | b);
      }

      // Matches QuantizeCelebi: drop transparent, run wu for seeds, then
      // wsmeans. Wu is unchanged from MCU.
      std::vector<mcu::Argb> opaque;
      opaque.reserve(pixels.size());
      for (mcu::Argb p : pixels) {
        if (mcu::IsOpaque(p))
          opaque.push_back(p);
      }
      auto wu_seeds = mcu::QuantizeWu(opaque, 128);
      auto clusters = wsmeansMatugenOrder(opaque, wu_seeds, 128);

      // Drop low-chroma clusters before scoring (matches matugen's
      // get_source_color_from_image, which calls IndexMap::retain on the
      // quantizer output). This DOES skew the proportions in score, but
      // matugen ships it that way and we need bug-for-bug parity.
      std::erase_if(clusters, [](const ClusterEntry& c) { return mcu::CamFromInt(c.argb).chroma < 5.0; });

      auto ranked = scoreMatugen(clusters, 4, true);
      return ranked.empty() ? 0xff4285f4u : ranked.front();
    }

    std::unique_ptr<mcu::DynamicScheme> makeScheme(const mcu::Hct& source, Scheme scheme, bool isDark) {
      switch (scheme) {
      case Scheme::TonalSpot:
        return std::make_unique<mcu::SchemeTonalSpot>(source, isDark, 0.0);
      case Scheme::Content:
        return std::make_unique<mcu::SchemeContent>(source, isDark, 0.0);
      case Scheme::FruitSalad:
        return std::make_unique<mcu::SchemeFruitSalad>(source, isDark, 0.0);
      case Scheme::Rainbow:
        return std::make_unique<mcu::SchemeRainbow>(source, isDark, 0.0);
      case Scheme::Monochrome:
        return std::make_unique<mcu::SchemeMonochrome>(source, isDark, 0.0);
      default:
        // Custom schemes are not handled here.
        return std::make_unique<mcu::SchemeTonalSpot>(source, isDark, 0.0);
      }
    }

    std::unordered_map<std::string, uint32_t> buildTokenMap(mcu::DynamicScheme& s) {
      std::unordered_map<std::string, uint32_t> m;
      auto set = [&](const char* k, mcu::DynamicColor dc) { m[k] = dc.GetArgb(s); };

      // Primary group
      set("primary", mcu::MaterialDynamicColors::Primary());
      set("on_primary", mcu::MaterialDynamicColors::OnPrimary());
      set("primary_container", mcu::MaterialDynamicColors::PrimaryContainer());
      set("on_primary_container", mcu::MaterialDynamicColors::OnPrimaryContainer());
      set("inverse_primary", mcu::MaterialDynamicColors::InversePrimary());
      set("surface_tint", mcu::MaterialDynamicColors::SurfaceTint());

      // Secondary group
      set("secondary", mcu::MaterialDynamicColors::Secondary());
      set("on_secondary", mcu::MaterialDynamicColors::OnSecondary());
      set("secondary_container", mcu::MaterialDynamicColors::SecondaryContainer());
      set("on_secondary_container", mcu::MaterialDynamicColors::OnSecondaryContainer());

      // Tertiary group
      set("tertiary", mcu::MaterialDynamicColors::Tertiary());
      set("on_tertiary", mcu::MaterialDynamicColors::OnTertiary());
      set("tertiary_container", mcu::MaterialDynamicColors::TertiaryContainer());
      set("on_tertiary_container", mcu::MaterialDynamicColors::OnTertiaryContainer());

      // Error group
      set("error", mcu::MaterialDynamicColors::Error());
      set("on_error", mcu::MaterialDynamicColors::OnError());
      set("error_container", mcu::MaterialDynamicColors::ErrorContainer());
      set("on_error_container", mcu::MaterialDynamicColors::OnErrorContainer());

      // Surface
      set("surface", mcu::MaterialDynamicColors::Surface());
      set("on_surface", mcu::MaterialDynamicColors::OnSurface());
      set("surface_variant", mcu::MaterialDynamicColors::SurfaceVariant());
      set("on_surface_variant", mcu::MaterialDynamicColors::OnSurfaceVariant());
      set("surface_dim", mcu::MaterialDynamicColors::SurfaceDim());
      set("surface_bright", mcu::MaterialDynamicColors::SurfaceBright());

      // Surface containers
      set("surface_container_lowest", mcu::MaterialDynamicColors::SurfaceContainerLowest());
      set("surface_container_low", mcu::MaterialDynamicColors::SurfaceContainerLow());
      set("surface_container", mcu::MaterialDynamicColors::SurfaceContainer());
      set("surface_container_high", mcu::MaterialDynamicColors::SurfaceContainerHigh());
      set("surface_container_highest", mcu::MaterialDynamicColors::SurfaceContainerHighest());

      // Outline + shadow/scrim
      set("outline", mcu::MaterialDynamicColors::Outline());
      set("outline_variant", mcu::MaterialDynamicColors::OutlineVariant());
      set("shadow", mcu::MaterialDynamicColors::Shadow());
      set("scrim", mcu::MaterialDynamicColors::Scrim());

      // Inverse
      set("inverse_surface", mcu::MaterialDynamicColors::InverseSurface());
      set("inverse_on_surface", mcu::MaterialDynamicColors::InverseOnSurface());

      // Background (alias of surface per MD3 spec)
      set("background", mcu::MaterialDynamicColors::Background());
      set("on_background", mcu::MaterialDynamicColors::OnBackground());

      // Fixed colors — identical across light/dark.
      set("primary_fixed", mcu::MaterialDynamicColors::PrimaryFixed());
      set("primary_fixed_dim", mcu::MaterialDynamicColors::PrimaryFixedDim());
      set("on_primary_fixed", mcu::MaterialDynamicColors::OnPrimaryFixed());
      set("on_primary_fixed_variant", mcu::MaterialDynamicColors::OnPrimaryFixedVariant());
      set("secondary_fixed", mcu::MaterialDynamicColors::SecondaryFixed());
      set("secondary_fixed_dim", mcu::MaterialDynamicColors::SecondaryFixedDim());
      set("on_secondary_fixed", mcu::MaterialDynamicColors::OnSecondaryFixed());
      set("on_secondary_fixed_variant", mcu::MaterialDynamicColors::OnSecondaryFixedVariant());
      set("tertiary_fixed", mcu::MaterialDynamicColors::TertiaryFixed());
      set("tertiary_fixed_dim", mcu::MaterialDynamicColors::TertiaryFixedDim());
      set("on_tertiary_fixed", mcu::MaterialDynamicColors::OnTertiaryFixed());
      set("on_tertiary_fixed_variant", mcu::MaterialDynamicColors::OnTertiaryFixedVariant());

      return m;
    }

  } // namespace

  GeneratedPalette generateMaterial(const std::vector<uint8_t>& rgb112, Scheme scheme) {
    const mcu::Argb seed = extractSeed(rgb112);
    const mcu::Hct source(seed);

    auto darkScheme = makeScheme(source, scheme, true);
    auto lightScheme = makeScheme(source, scheme, false);

    GeneratedPalette out;
    out.dark = buildTokenMap(*darkScheme);
    out.light = buildTokenMap(*lightScheme);
    out.dark["source_color"] = seed;
    out.light["source_color"] = seed;
    synthesizeTerminalPaletteTokens(out);
    return out;
  }

} // namespace noctalia::theme
