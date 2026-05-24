// Custom (non-M3) schemes: vibrant, faithful, dysfunctional, muted.
//
// These do NOT use TonalPalette. Pipeline:
//   downsample → k-means in Lab space (deterministic init by sort-by-L) →
//   scheme-specific scoring to pick primary/secondary/tertiary seeds →
//   generate the built-in token map in HSL space using shiftHue/adjustSurface/
//   ensureContrast helpers from theme/color.h + theme/contrast.h.

#include "cpp/cam/hct.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/fixed_palette.h"
#include "theme/palette.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mcu = material_color_utilities;

namespace noctalia::theme {

  namespace {

    // ────────────────────────────────────────────────────────────────────────────
    // Lab (D65) colour-space conversion.
    // ────────────────────────────────────────────────────────────────────────────

    constexpr double kWhiteX = 95.047;
    constexpr double kWhiteY = 100.0;
    constexpr double kWhiteZ = 108.883;

    struct Lab {
      double L, a, b;
    };

    double linearize(int c) {
      const double n = c / 255.0;
      if (n <= 0.04045)
        return n / 12.92;
      return std::pow((n + 0.055) / 1.055, 2.4);
    }

    int delinearize(double v) {
      double n;
      if (v <= 0.0031308)
        n = v * 12.92;
      else
        n = 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
      long r = std::lround(n * 255.0);
      if (r < 0)
        r = 0;
      if (r > 255)
        r = 255;
      return static_cast<int>(r);
    }

    double labF(double t) {
      if (t > 0.008856)
        return std::cbrt(t);
      return (903.3 * t + 16.0) / 116.0;
    }

    double labFInv(double t) {
      if (t > 0.206893)
        return t * t * t;
      return (116.0 * t - 16.0) / 903.3;
    }

    Lab rgbToLab(const Color& c) {
      const double lr = linearize(c.r);
      const double lg = linearize(c.g);
      const double lb = linearize(c.b);
      double x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) * 100.0;
      double y = (0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb) * 100.0;
      double z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) * 100.0;
      const double fx = labF(x / kWhiteX);
      const double fy = labF(y / kWhiteY);
      const double fz = labF(z / kWhiteZ);
      return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

    Color labToRgb(const Lab& lab) {
      const double fy = (lab.L + 16.0) / 116.0;
      const double fx = lab.a / 500.0 + fy;
      const double fz = fy - lab.b / 200.0;
      double x = kWhiteX * labFInv(fx) / 100.0;
      double y = kWhiteY * labFInv(fy) / 100.0;
      double z = kWhiteZ * labFInv(fz) / 100.0;
      double lr = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
      double lg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
      double lb = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
      lr = std::clamp(lr, 0.0, 1.0);
      lg = std::clamp(lg, 0.0, 1.0);
      lb = std::clamp(lb, 0.0, 1.0);
      return Color(delinearize(lr), delinearize(lg), delinearize(lb));
    }

    double labDistance(const Lab& a, const Lab& b) {
      const double dL = a.L - b.L;
      const double da = a.a - b.a;
      const double db = a.b - b.b;
      return std::sqrt(dL * dL + da * da + db * db);
    }

    // ────────────────────────────────────────────────────────────────────────────
    // HCT lookup via MCU
    // ────────────────────────────────────────────────────────────────────────────

    struct Hct {
      double hue, chroma, tone;
    };

    Hct colorToHct(const Color& c) {
      mcu::Hct h(static_cast<mcu::Argb>(c.toArgb()));
      return {h.get_hue(), h.get_chroma(), h.get_tone()};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Downsample + k-means in Lab space
    // ────────────────────────────────────────────────────────────────────────────

    std::vector<Color> downsamplePixels(const std::vector<uint8_t>& rgb, int factor = 4) {
      std::vector<Color> out;
      const int step = factor * factor;
      const size_t npix = rgb.size() / 3;
      out.reserve(npix / static_cast<size_t>(step) + 1);
      for (size_t i = 0; i < npix; i += static_cast<size_t>(step)) {
        out.emplace_back(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
      }
      return out;
    }

    struct Cluster {
      Color centroid;       // averaged (Lab → RGB)
      Color representative; // actual input pixel closest to centroid
      int count;
    };

    // Lab-space k-means with deterministic init (indices sorted by L, even
    // stride). 10 iterations by default. Returns clusters sorted by size desc,
    // each with its averaged centroid, the closest input pixel (representative),
    // and the cluster count.
    std::vector<Cluster> kmeansCluster(const std::vector<Color>& colors, int k, int iterations = 10) {
      const int n = static_cast<int>(colors.size());
      if (n < k) {
        std::vector<Cluster> out;
        std::unordered_map<uint32_t, int> counts;
        for (const auto& c : colors)
          counts[c.toArgb()]++;
        for (const auto& kv : counts) {
          const Color c = Color::fromArgb(kv.first);
          out.push_back({c, c, kv.second});
          if (static_cast<int>(out.size()) >= k)
            break;
        }
        return out;
      }

      std::vector<Lab> labs(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i)
        labs[static_cast<size_t>(i)] = rgbToLab(colors[static_cast<size_t>(i)]);

      // Deterministic init: sort indices by L, pick evenly spaced.
      std::vector<int> sortedIdx(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i)
        sortedIdx[static_cast<size_t>(i)] = i;
      // Stable sort so tied L values preserve their original order.
      std::stable_sort(sortedIdx.begin(), sortedIdx.end(), [&](int a, int b) {
        return labs[static_cast<size_t>(a)].L < labs[static_cast<size_t>(b)].L;
      });

      std::vector<Lab> centroids(static_cast<size_t>(k));
      const int step = n / k;
      for (int i = 0; i < k; ++i) {
        centroids[static_cast<size_t>(i)] = labs[static_cast<size_t>(sortedIdx[static_cast<size_t>(i * step)])];
      }

      std::vector<int> assignments(static_cast<size_t>(n), 0);
      for (int iter = 0; iter < iterations; ++iter) {
        // Assign each color to the nearest centroid.
        for (int idx = 0; idx < n; ++idx) {
          double minDist = std::numeric_limits<double>::infinity();
          int minCluster = 0;
          for (int ci = 0; ci < k; ++ci) {
            const double d = labDistance(labs[static_cast<size_t>(idx)], centroids[static_cast<size_t>(ci)]);
            if (d < minDist) {
              minDist = d;
              minCluster = ci;
            }
          }
          assignments[static_cast<size_t>(idx)] = minCluster;
        }

        // Recompute centroids as mean of assigned Lab points.
        std::vector<Lab> acc(static_cast<size_t>(k), {0.0, 0.0, 0.0});
        std::vector<int> cnt(static_cast<size_t>(k), 0);
        for (int idx = 0; idx < n; ++idx) {
          const int ci = assignments[static_cast<size_t>(idx)];
          acc[static_cast<size_t>(ci)].L += labs[static_cast<size_t>(idx)].L;
          acc[static_cast<size_t>(ci)].a += labs[static_cast<size_t>(idx)].a;
          acc[static_cast<size_t>(ci)].b += labs[static_cast<size_t>(idx)].b;
          cnt[static_cast<size_t>(ci)]++;
        }
        for (int ci = 0; ci < k; ++ci) {
          if (cnt[static_cast<size_t>(ci)] > 0) {
            const double nd = static_cast<double>(cnt[static_cast<size_t>(ci)]);
            centroids[static_cast<size_t>(ci)] = {
                acc[static_cast<size_t>(ci)].L / nd, acc[static_cast<size_t>(ci)].a / nd,
                acc[static_cast<size_t>(ci)].b / nd
            };
          }
        }
      }

      // Final pass: count + find representative pixel (closest to centroid).
      std::vector<int> clusterCounts(static_cast<size_t>(k), 0);
      std::vector<std::pair<Color, double>> reps(
          static_cast<size_t>(k), {Color(0, 0, 0), std::numeric_limits<double>::infinity()}
      );
      for (int idx = 0; idx < n; ++idx) {
        const int ci = assignments[static_cast<size_t>(idx)];
        clusterCounts[static_cast<size_t>(ci)]++;
        const double d = labDistance(labs[static_cast<size_t>(idx)], centroids[static_cast<size_t>(ci)]);
        if (d < reps[static_cast<size_t>(ci)].second) {
          reps[static_cast<size_t>(ci)] = {colors[static_cast<size_t>(idx)], d};
        }
      }

      std::vector<Cluster> results;
      for (int ci = 0; ci < k; ++ci) {
        if (clusterCounts[static_cast<size_t>(ci)] > 0) {
          results.push_back(
              {labToRgb(centroids[static_cast<size_t>(ci)]), reps[static_cast<size_t>(ci)].first,
               clusterCounts[static_cast<size_t>(ci)]}
          );
        }
      }
      std::stable_sort(results.begin(), results.end(), [](const Cluster& a, const Cluster& b) {
        return a.count > b.count;
      });
      return results;
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Hue families: 6 non-uniform buckets used by the scoring functions.
    // ────────────────────────────────────────────────────────────────────────────

    int hueToFamily(double hue) {
      if (hue >= 330.0 || hue < 30.0)
        return 0; // RED
      if (hue < 60.0)
        return 1; // ORANGE
      if (hue < 105.0)
        return 2; // YELLOW
      if (hue < 190.0)
        return 3; // GREEN
      if (hue < 270.0)
        return 4; // BLUE
      return 5;   // PURPLE
    }

    double familyCenterHue(int family) {
      static const double centers[6] = {0.0, 45.0, 82.5, 147.5, 230.0, 300.0};
      return centers[family];
    }

    double circularHueDiff(double h1, double h2) {
      const double diff = std::fabs(h1 - h2);
      return std::min(diff, 360.0 - diff);
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Scheme-specific scoring over quantized clusters.
    // ────────────────────────────────────────────────────────────────────────────

    struct Scored {
      Color color;
      double score;
    };

    // Vibrant: count^0.3 weighting, chroma-prioritized with tone/hue penalties.
    std::vector<Scored> scoreChroma(const std::vector<std::pair<Color, int>>& in) {
      std::vector<Scored> out;
      out.reserve(in.size());
      for (const auto& [color, count] : in) {
        const auto hct = colorToHct(color);
        const double chromaScore = hct.chroma;
        double tonePenalty = 0.0;
        if (hct.tone < 20.0)
          tonePenalty = (20.0 - hct.tone) * 2.0;
        else if (hct.tone > 80.0)
          tonePenalty = (hct.tone - 80.0) * 1.5;
        else if (hct.tone < 40.0)
          tonePenalty = (40.0 - hct.tone) * 0.5;
        else if (hct.tone > 60.0)
          tonePenalty = (hct.tone - 60.0) * 0.3;
        double huePenalty = 0.0;
        if (hct.hue > 80.0 && hct.hue < 110.0)
          huePenalty = 5.0;
        const double score = (chromaScore - tonePenalty - huePenalty) * std::pow(static_cast<double>(count), 0.3);
        out.push_back({color, score});
      }
      std::stable_sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
      return out;
    }

    // Faithful: count-by-hue-family scoring.
    std::vector<Scored> scoreCount(const std::vector<std::pair<Color, int>>& in) {
      constexpr double MIN_CHROMA = 10.0;
      struct Entry {
        Color color;
        double hue;
        double chroma;
        int count;
      };
      std::unordered_map<int, std::vector<Entry>> families;

      for (const auto& [color, count] : in) {
        const auto hct = colorToHct(color);
        if (hct.chroma >= MIN_CHROMA) {
          families[hueToFamily(hct.hue)].push_back({color, hct.hue, hct.chroma, count});
        }
      }

      if (families.empty()) {
        std::vector<Scored> result;
        for (const auto& [color, count] : in)
          result.push_back({color, static_cast<double>(count)});
        std::stable_sort(result.begin(), result.end(), [](const Scored& a, const Scored& b) {
          return a.score > b.score;
        });
        return result;
      }

      std::vector<std::pair<int, int>> familyTotals;
      for (auto& [fam, entries] : families) {
        int tot = 0;
        for (const auto& e : entries)
          tot += e.count;
        familyTotals.push_back({fam, tot});
      }
      std::stable_sort(familyTotals.begin(), familyTotals.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
      });

      std::vector<Scored> result;
      for (size_t rank = 0; rank < familyTotals.size(); ++rank) {
        const int fam = familyTotals[rank].first;
        auto& entries = families[fam];
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
          if (a.count != b.count)
            return a.count > b.count;
          return a.chroma > b.chroma;
        });
        for (const auto& e : entries) {
          const double score = static_cast<double>(familyTotals.size() - rank) * 1'000'000.0 +
                               static_cast<double>(e.count) * 1000.0 + e.chroma;
          result.push_back({e.color, score});
        }
      }
      std::stable_sort(result.begin(), result.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
      });
      return result;
    }

    // Dysfunctional: prefer the 2nd most dominant hue family.
    std::vector<Scored> scoreDysfunctional(const std::vector<std::pair<Color, int>>& in) {
      constexpr double MIN_CHROMA = 10.0;
      constexpr double MIN_HUE_DISTANCE = 45.0;
      constexpr double MIN_COUNT_RATIO = 0.02;

      struct Entry {
        Color color;
        double hue;
        double chroma;
        int count;
      };
      std::unordered_map<int, std::vector<Entry>> families;

      for (const auto& [color, count] : in) {
        const auto hct = colorToHct(color);
        if (hct.chroma >= MIN_CHROMA) {
          families[hueToFamily(hct.hue)].push_back({color, hct.hue, hct.chroma, count});
        }
      }

      if (families.empty()) {
        std::vector<Scored> result;
        for (const auto& [color, count] : in)
          result.push_back({color, static_cast<double>(count)});
        std::stable_sort(result.begin(), result.end(), [](const Scored& a, const Scored& b) {
          return a.score > b.score;
        });
        return result;
      }

      std::vector<std::pair<int, int>> familyTotals;
      for (auto& [fam, entries] : families) {
        int tot = 0;
        for (const auto& e : entries)
          tot += e.count;
        familyTotals.push_back({fam, tot});
      }
      std::stable_sort(familyTotals.begin(), familyTotals.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
      });

      const int dominantFamily = familyTotals[0].first;
      const double dominantCenter = familyCenterHue(dominantFamily);
      int totalColorful = 0;
      for (const auto& [f, c] : familyTotals)
        totalColorful += c;
      const double minCount = static_cast<double>(totalColorful) * MIN_COUNT_RATIO;

      struct Distant {
        int family;
        int count;
        double hueDiff;
        double maxChroma;
      };
      std::vector<Distant> distant;
      std::vector<int> close = {dominantFamily};
      for (size_t i = 1; i < familyTotals.size(); ++i) {
        const int fam = familyTotals[i].first;
        const int cnt = familyTotals[i].second;
        const double famCenter = familyCenterHue(fam);
        const double hueDiff = circularHueDiff(dominantCenter, famCenter);
        if (hueDiff >= MIN_HUE_DISTANCE && static_cast<double>(cnt) >= minCount) {
          double maxChroma = 0.0;
          for (const auto& e : families[fam])
            maxChroma = std::max(maxChroma, e.chroma);
          distant.push_back({fam, cnt, hueDiff, maxChroma});
        } else {
          close.push_back(fam);
        }
      }

      std::stable_sort(distant.begin(), distant.end(), [](const Distant& a, const Distant& b) {
        return a.hueDiff * a.maxChroma > b.hueDiff * b.maxChroma;
      });

      std::vector<Scored> result;
      for (size_t rank = 0; rank < distant.size(); ++rank) {
        const int fam = distant[rank].family;
        auto& entries = families[fam];
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
          if (a.chroma != b.chroma)
            return a.chroma > b.chroma;
          return a.count > b.count;
        });
        for (const auto& e : entries) {
          const double score = static_cast<double>(distant.size() - rank) * 1'000'000.0 + e.chroma * 1000.0 +
                               static_cast<double>(e.count);
          result.push_back({e.color, score});
        }
      }
      for (int fam : close) {
        auto& entries = families[fam];
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
          if (a.count != b.count)
            return a.count > b.count;
          return a.chroma > b.chroma;
        });
        for (const auto& e : entries) {
          const double score = static_cast<double>(e.count) * 1000.0 + e.chroma;
          result.push_back({e.color, score});
        }
      }
      std::stable_sort(result.begin(), result.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
      });
      return result;
    }

    // Muted: pure count, no chroma filter.
    std::vector<Scored> scoreMuted(const std::vector<std::pair<Color, int>>& in) {
      std::vector<Scored> out;
      out.reserve(in.size());
      for (const auto& [color, count] : in)
        out.push_back({color, static_cast<double>(count)});
      std::stable_sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
      return out;
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Palette extraction: downsample → cluster → score → pick top k seeds.
    // ────────────────────────────────────────────────────────────────────────────

    std::vector<Color> extractPalette(const std::vector<uint8_t>& rgb112, Scheme scheme, int k = 5) {
      auto sampled = downsamplePixels(rgb112, 4);

      int clusterCount = 20;
      std::vector<Color> filtered = sampled;

      if (scheme == Scheme::Vibrant) {
        clusterCount = 20;
        // Pre-filter to chroma >= 5.0 in Cam16; fall back to sampled if too sparse.
        std::vector<Color> kept;
        for (const auto& c : sampled) {
          const auto hct = colorToHct(c);
          if (hct.chroma >= 5.0)
            kept.push_back(c);
        }
        filtered = (static_cast<int>(kept.size()) >= clusterCount * 2) ? kept : sampled;
      } else if (scheme == Scheme::Faithful || scheme == Scheme::Dysfunctional) {
        clusterCount = 48;
      } else if (scheme == Scheme::Muted) {
        clusterCount = 24;
      }

      auto clusters = kmeansCluster(filtered, clusterCount);

      // Vibrant is scored from averaged centroids for smoother output; the
      // other schemes score from representative pixels (actual image colours).
      std::vector<std::pair<Color, int>> scoringInput;
      scoringInput.reserve(clusters.size());
      if (scheme == Scheme::Vibrant) {
        for (const auto& c : clusters)
          scoringInput.push_back({c.centroid, c.count});
      } else {
        for (const auto& c : clusters)
          scoringInput.push_back({c.representative, c.count});
      }

      std::vector<Scored> scored;
      switch (scheme) {
      case Scheme::Vibrant:
        scored = scoreChroma(scoringInput);
        break;
      case Scheme::Faithful:
        scored = scoreCount(scoringInput);
        break;
      case Scheme::Dysfunctional:
        scored = scoreDysfunctional(scoringInput);
        break;
      case Scheme::Muted:
        scored = scoreMuted(scoringInput);
        break;
      default:
        scored = scoreMuted(scoringInput);
        break;
      }

      std::vector<Color> finalColors;
      for (const auto& s : scored)
        finalColors.push_back(s.color);

      // If scoring returned fewer than k colours, synthesise the rest by
      // rotating the primary in 60° steps.
      while (static_cast<int>(finalColors.size()) < k) {
        if (finalColors.empty()) {
          finalColors.push_back(Color::fromHex("#6750A4"));
          continue;
        }
        const Color base = finalColors.front();
        const double offset = static_cast<double>(finalColors.size()) * 60.0;
        finalColors.push_back(shiftHue(base, offset));
      }

      finalColors.resize(static_cast<size_t>(k));
      return finalColors;
    }

    // ────────────────────────────────────────────────────────────────────────────
    // find_error_color
    // ────────────────────────────────────────────────────────────────────────────

    Color findErrorColor(const std::vector<Color>& palette) {
      for (const auto& c : palette) {
        auto [h, s, l] = c.toHsl();
        if ((h <= 30.0 || h >= 330.0) && s > 0.4 && l > 0.3 && l < 0.7)
          return c;
      }
      return Color::fromHex("#FD4663");
    }

    // ────────────────────────────────────────────────────────────────────────────
    // HSL-space token map generators (vibrant / faithful / dysfunctional).
    // ────────────────────────────────────────────────────────────────────────────

    using TokenMap = std::unordered_map<std::string, uint32_t>;

    Color fromHsl(double h, double s, double l) { return Color::fromHsl(h, s, l); }

    TokenMap generateNormalDark(const std::vector<Color>& palette) {
      const Color primary = palette.empty() ? Color(255, 245, 155) : palette[0];
      auto [primary_h, primary_s, primary_l] = primary.toHsl();
      (void)primary_l;

      constexpr double MIN_HUE_DISTANCE = 30.0;
      Color secondary = shiftHue(primary, 30.0);
      if (palette.size() > 1) {
        const auto [sh, ss, sl] = palette[1].toHsl();
        (void)ss;
        (void)sl;
        if (hueDistance(primary_h, sh) > MIN_HUE_DISTANCE)
          secondary = palette[1];
      }
      Color tertiary = shiftHue(primary, 60.0);
      if (palette.size() > 2) {
        const auto [th, ts, tl] = palette[2].toHsl();
        const auto [sh, ss, sl] = secondary.toHsl();
        (void)ts;
        (void)tl;
        (void)ss;
        (void)sl;
        if (hueDistance(primary_h, th) > MIN_HUE_DISTANCE && hueDistance(sh, th) > MIN_HUE_DISTANCE) {
          tertiary = palette[2];
        }
      }
      const Color error = findErrorColor(palette);

      auto [ph, ps, pl] = primary.toHsl();
      const Color primary_adjusted = fromHsl(ph, std::max(ps, 0.7), std::max(pl, 0.65));
      auto [sh2, ss2, sl2] = secondary.toHsl();
      const Color secondary_adjusted = fromHsl(sh2, std::max(ss2, 0.6), std::max(sl2, 0.60));
      auto [th2, ts2, tl2] = tertiary.toHsl();
      const Color tertiary_adjusted = fromHsl(th2, std::max(ts2, 0.5), std::max(tl2, 0.60));

      auto makeContainerDark = [](const Color& base) {
        auto [h, s, l] = base.toHsl();
        return fromHsl(h, std::min(s + 0.15, 1.0), std::max(l - 0.35, 0.15));
      };
      const Color primary_container = makeContainerDark(primary_adjusted);
      const Color secondary_container = makeContainerDark(secondary_adjusted);
      const Color tertiary_container = makeContainerDark(tertiary_adjusted);
      const Color error_container = makeContainerDark(error);

      // Surface heuristic — shift cyan toward blue, cap saturation by hue band.
      auto [surface_hue_tmp, ssurf, _ignored] = palette[0].toHsl();
      (void)_ignored;
      double surface_hue = surface_hue_tmp;
      if (surface_hue >= 160.0 && surface_hue <= 200.0) {
        surface_hue = std::fmod(surface_hue + 10.0, 360.0);
      }
      double surface_sat_cap;
      if (surface_hue < 60.0 || surface_hue > 300.0)
        surface_sat_cap = 0.35;
      else if (surface_hue < 120.0)
        surface_sat_cap = 0.50;
      else
        surface_sat_cap = 0.90;

      const Color base_surface = fromHsl(surface_hue, std::min(ssurf, surface_sat_cap), 0.5);
      const Color surface = adjustSurface(base_surface, surface_sat_cap, 0.12);
      const Color surface_variant = adjustSurface(base_surface, std::min(0.80, surface_sat_cap), 0.16);

      const Color surface_container_lowest = adjustSurface(base_surface, 0.85, 0.06);
      const Color surface_container_low = adjustSurface(base_surface, 0.85, 0.10);
      const Color surface_container = adjustSurface(base_surface, 0.70, 0.20);
      const Color surface_container_high = adjustSurface(base_surface, 0.75, 0.18);
      const Color surface_container_highest = adjustSurface(base_surface, 0.70, 0.22);

      auto [text_h, _tsh, _tsl] = palette[0].toHsl();
      (void)_tsh;
      (void)_tsl;
      const Color base_on_surface = fromHsl(text_h, 0.05, 0.95);
      const Color on_surface = ensureContrast(base_on_surface, surface, 4.5);
      const Color base_on_surface_variant = fromHsl(text_h, 0.05, 0.70);
      const Color on_surface_variant = ensureContrast(base_on_surface_variant, surface_variant, 4.5);

      const Color outline = ensureContrast(adjustSurface(palette[0], 0.10, 0.30), surface, 3.0);
      const Color outline_variant = ensureContrast(adjustSurface(palette[0], 0.10, 0.40), surface, 3.0);

      const Color dark_fg = fromHsl(std::get<0>(palette[0].toHsl()), 0.20, 0.12);
      const Color on_primary = ensureContrast(dark_fg, primary_adjusted, 7.0);
      const Color on_secondary = ensureContrast(dark_fg, secondary_adjusted, 7.0);
      const Color on_tertiary = ensureContrast(dark_fg, tertiary_adjusted, 7.0);
      const Color on_error = ensureContrast(dark_fg, error, 7.0);

      const Color on_primary_container = ensureContrast(fromHsl(primary_h, primary_s, 0.90), primary_container, 4.5, 1);
      auto [sec_h, sec_s, _sl] = secondary.toHsl();
      (void)_sl;
      const Color on_secondary_container = ensureContrast(fromHsl(sec_h, sec_s, 0.90), secondary_container, 4.5, 1);
      auto [ter_h, ter_s, _tl] = tertiary.toHsl();
      (void)_tl;
      const Color on_tertiary_container = ensureContrast(fromHsl(ter_h, ter_s, 0.90), tertiary_container, 4.5, 1);
      auto [err_h, err_s, _el] = error.toHsl();
      (void)_el;
      const Color on_error_container = ensureContrast(fromHsl(err_h, err_s, 0.90), error_container, 4.5, 1);

      const Color shadow = surface;
      const Color scrim = Color(0, 0, 0);

      const double inv_h = std::get<0>(palette[0].toHsl());
      const Color inverse_surface = fromHsl(inv_h, 0.08, 0.90);
      const Color inverse_on_surface = fromHsl(inv_h, 0.05, 0.15);
      const Color inverse_primary = fromHsl(primary_h, std::max(primary_s * 0.8, 0.5), 0.40);

      const Color background = surface;
      const Color on_background = on_surface;

      auto makeFixedDark = [](const Color& base) {
        auto [h, s, l] = base.toHsl();
        (void)l;
        return std::pair<Color, Color>{fromHsl(h, std::max(s, 0.70), 0.85), fromHsl(h, std::max(s, 0.65), 0.75)};
      };
      auto [primary_fixed, primary_fixed_dim] = makeFixedDark(primary_adjusted);
      auto [secondary_fixed, secondary_fixed_dim] = makeFixedDark(secondary_adjusted);
      auto [tertiary_fixed, tertiary_fixed_dim] = makeFixedDark(tertiary_adjusted);

      const Color on_primary_fixed = ensureContrast(fromHsl(primary_h, 0.15, 0.15), primary_fixed, 4.5);
      const Color on_primary_fixed_variant = ensureContrast(fromHsl(primary_h, 0.15, 0.20), primary_fixed_dim, 4.5);
      const Color on_secondary_fixed = ensureContrast(fromHsl(sec_h, 0.15, 0.15), secondary_fixed, 4.5);
      const Color on_secondary_fixed_variant = ensureContrast(fromHsl(sec_h, 0.15, 0.20), secondary_fixed_dim, 4.5);
      const Color on_tertiary_fixed = ensureContrast(fromHsl(ter_h, 0.15, 0.15), tertiary_fixed, 4.5);
      const Color on_tertiary_fixed_variant = ensureContrast(fromHsl(ter_h, 0.15, 0.20), tertiary_fixed_dim, 4.5);

      const Color surface_dim = adjustSurface(base_surface, 0.85, 0.08);
      const Color surface_bright = adjustSurface(base_surface, 0.75, 0.24);

      TokenMap m;
      m["primary"] = primary_adjusted.toArgb();
      m["on_primary"] = on_primary.toArgb();
      m["primary_container"] = primary_container.toArgb();
      m["on_primary_container"] = on_primary_container.toArgb();
      m["primary_fixed"] = primary_fixed.toArgb();
      m["primary_fixed_dim"] = primary_fixed_dim.toArgb();
      m["on_primary_fixed"] = on_primary_fixed.toArgb();
      m["on_primary_fixed_variant"] = on_primary_fixed_variant.toArgb();
      m["surface_tint"] = primary_adjusted.toArgb();
      m["secondary"] = secondary_adjusted.toArgb();
      m["on_secondary"] = on_secondary.toArgb();
      m["secondary_container"] = secondary_container.toArgb();
      m["on_secondary_container"] = on_secondary_container.toArgb();
      m["secondary_fixed"] = secondary_fixed.toArgb();
      m["secondary_fixed_dim"] = secondary_fixed_dim.toArgb();
      m["on_secondary_fixed"] = on_secondary_fixed.toArgb();
      m["on_secondary_fixed_variant"] = on_secondary_fixed_variant.toArgb();
      m["tertiary"] = tertiary_adjusted.toArgb();
      m["on_tertiary"] = on_tertiary.toArgb();
      m["tertiary_container"] = tertiary_container.toArgb();
      m["on_tertiary_container"] = on_tertiary_container.toArgb();
      m["tertiary_fixed"] = tertiary_fixed.toArgb();
      m["tertiary_fixed_dim"] = tertiary_fixed_dim.toArgb();
      m["on_tertiary_fixed"] = on_tertiary_fixed.toArgb();
      m["on_tertiary_fixed_variant"] = on_tertiary_fixed_variant.toArgb();
      m["error"] = error.toArgb();
      m["on_error"] = on_error.toArgb();
      m["error_container"] = error_container.toArgb();
      m["on_error_container"] = on_error_container.toArgb();
      m["surface"] = surface.toArgb();
      m["on_surface"] = on_surface.toArgb();
      m["surface_variant"] = surface_variant.toArgb();
      m["on_surface_variant"] = on_surface_variant.toArgb();
      m["surface_dim"] = surface_dim.toArgb();
      m["surface_bright"] = surface_bright.toArgb();
      m["surface_container_lowest"] = surface_container_lowest.toArgb();
      m["surface_container_low"] = surface_container_low.toArgb();
      m["surface_container"] = surface_container.toArgb();
      m["surface_container_high"] = surface_container_high.toArgb();
      m["surface_container_highest"] = surface_container_highest.toArgb();
      m["outline"] = outline.toArgb();
      m["outline_variant"] = outline_variant.toArgb();
      m["shadow"] = shadow.toArgb();
      m["scrim"] = scrim.toArgb();
      m["inverse_surface"] = inverse_surface.toArgb();
      m["inverse_on_surface"] = inverse_on_surface.toArgb();
      m["inverse_primary"] = inverse_primary.toArgb();
      m["background"] = background.toArgb();
      m["on_background"] = on_background.toArgb();
      return m;
    }

    TokenMap generateNormalLight(const std::vector<Color>& palette) {
      const Color primary = palette.empty() ? Color(93, 101, 245) : palette[0];
      auto [primary_h, primary_s, _pl] = primary.toHsl();
      (void)_pl;

      constexpr double MIN_HUE_DISTANCE = 30.0;
      Color secondary = shiftHue(primary, 30.0);
      if (palette.size() > 1) {
        const auto [sh, ss, sl] = palette[1].toHsl();
        (void)ss;
        (void)sl;
        if (hueDistance(primary_h, sh) > MIN_HUE_DISTANCE)
          secondary = palette[1];
      }
      Color tertiary = shiftHue(primary, 60.0);
      if (palette.size() > 2) {
        const auto [th, ts, tl] = palette[2].toHsl();
        (void)ts;
        (void)tl;
        const auto [sh, ss, sl] = secondary.toHsl();
        (void)ss;
        (void)sl;
        if (hueDistance(primary_h, th) > MIN_HUE_DISTANCE && hueDistance(sh, th) > MIN_HUE_DISTANCE) {
          tertiary = palette[2];
        }
      }
      const Color error = findErrorColor(palette);

      auto clamp = [](double v, double lo, double hi) { return std::max(lo, std::min(v, hi)); };

      auto [ph, ps, pl] = primary.toHsl();
      const Color primary_adjusted = fromHsl(ph, std::max(ps, 0.7), clamp(pl, 0.25, 0.45));
      auto [sh2, ss2, sl2] = secondary.toHsl();
      const Color secondary_adjusted = fromHsl(sh2, std::max(ss2, 0.6), clamp(sl2, 0.22, 0.40));
      auto [th2, ts2, tl2] = tertiary.toHsl();
      const Color tertiary_adjusted = fromHsl(th2, std::max(ts2, 0.5), clamp(tl2, 0.20, 0.35));

      auto makeContainerLight = [](const Color& base) {
        auto [h, s, l] = base.toHsl();
        return fromHsl(h, std::max(s - 0.20, 0.30), std::min(l + 0.35, 0.85));
      };
      const Color primary_container = makeContainerLight(primary_adjusted);
      const Color secondary_container = makeContainerLight(secondary_adjusted);
      const Color tertiary_container = makeContainerLight(tertiary_adjusted);
      const Color error_container = makeContainerLight(error);

      const Color surface = adjustSurface(palette[0], 0.90, 0.90);
      const Color surface_variant = adjustSurface(palette[0], 0.80, 0.78);
      const Color surface_container_lowest = adjustSurface(palette[0], 0.85, 0.96);
      const Color surface_container_low = adjustSurface(palette[0], 0.85, 0.92);
      const Color surface_container = adjustSurface(palette[0], 0.80, 0.86);
      const Color surface_container_high = adjustSurface(palette[0], 0.75, 0.84);
      const Color surface_container_highest = adjustSurface(palette[0], 0.70, 0.80);

      auto [text_h, surface_s_tmp, _tl3] = palette[0].toHsl();
      const double surface_s = surface_s_tmp;
      (void)_tl3;
      const Color base_on_surface = fromHsl(text_h, 0.05, 0.10);
      const Color on_surface = ensureContrast(base_on_surface, surface, 4.5);
      const Color base_on_surface_variant = fromHsl(text_h, 0.05, 0.35);
      const Color on_surface_variant = ensureContrast(base_on_surface_variant, surface_variant, 4.5);

      const Color light_fg = fromHsl(text_h, 0.1, 0.98);
      const Color on_primary = ensureContrast(light_fg, primary_adjusted, 7.0);
      const Color on_secondary = ensureContrast(light_fg, secondary_adjusted, 7.0);
      const Color on_tertiary = ensureContrast(light_fg, tertiary_adjusted, 7.0);
      const Color on_error = ensureContrast(light_fg, error, 7.0);

      const Color on_primary_container =
          ensureContrast(fromHsl(primary_h, primary_s, 0.15), primary_container, 4.5, -1);
      auto [sec_h, sec_s, _sl] = secondary.toHsl();
      (void)_sl;
      const Color on_secondary_container = ensureContrast(fromHsl(sec_h, sec_s, 0.15), secondary_container, 4.5, -1);
      auto [ter_h, ter_s, _tl] = tertiary.toHsl();
      (void)_tl;
      const Color on_tertiary_container = ensureContrast(fromHsl(ter_h, ter_s, 0.15), tertiary_container, 4.5, -1);
      auto [err_h, err_s, _el] = error.toHsl();
      (void)_el;
      const Color on_error_container = ensureContrast(fromHsl(err_h, err_s, 0.15), error_container, 4.5, -1);

      auto makeFixedLight = [](const Color& base) {
        auto [h, s, l] = base.toHsl();
        (void)l;
        return std::pair<Color, Color>{fromHsl(h, std::max(s, 0.70), 0.40), fromHsl(h, std::max(s, 0.65), 0.30)};
      };
      auto [primary_fixed, primary_fixed_dim] = makeFixedLight(primary_adjusted);
      auto [secondary_fixed, secondary_fixed_dim] = makeFixedLight(secondary_adjusted);
      auto [tertiary_fixed, tertiary_fixed_dim] = makeFixedLight(tertiary_adjusted);

      const Color on_primary_fixed = ensureContrast(fromHsl(primary_h, 0.15, 0.90), primary_fixed, 4.5);
      const Color on_primary_fixed_variant = ensureContrast(fromHsl(primary_h, 0.15, 0.85), primary_fixed_dim, 4.5);
      const Color on_secondary_fixed = ensureContrast(fromHsl(sec_h, 0.15, 0.90), secondary_fixed, 4.5);
      const Color on_secondary_fixed_variant = ensureContrast(fromHsl(sec_h, 0.15, 0.85), secondary_fixed_dim, 4.5);
      const Color on_tertiary_fixed = ensureContrast(fromHsl(ter_h, 0.15, 0.90), tertiary_fixed, 4.5);
      const Color on_tertiary_fixed_variant = ensureContrast(fromHsl(ter_h, 0.15, 0.85), tertiary_fixed_dim, 4.5);

      const Color surface_dim = adjustSurface(palette[0], 0.85, 0.82);
      const Color surface_bright = adjustSurface(palette[0], 0.90, 0.95);

      const Color outline = ensureContrast(fromHsl(text_h, std::max(surface_s * 0.4, 0.25), 0.65), surface, 3.0);
      const Color outline_variant =
          ensureContrast(fromHsl(text_h, std::max(surface_s * 0.3, 0.20), 0.75), surface, 3.0);
      const Color shadow = fromHsl(text_h, std::max(surface_s * 0.3, 0.15), 0.80);
      const Color scrim = Color(0, 0, 0);

      const Color inverse_surface = fromHsl(text_h, 0.08, 0.15);
      const Color inverse_on_surface = fromHsl(text_h, 0.05, 0.90);
      const Color inverse_primary = fromHsl(primary_h, std::max(primary_s * 0.8, 0.5), 0.70);

      const Color background = surface;
      const Color on_background = on_surface;

      TokenMap m;
      m["primary"] = primary_adjusted.toArgb();
      m["on_primary"] = on_primary.toArgb();
      m["primary_container"] = primary_container.toArgb();
      m["on_primary_container"] = on_primary_container.toArgb();
      m["primary_fixed"] = primary_fixed.toArgb();
      m["primary_fixed_dim"] = primary_fixed_dim.toArgb();
      m["on_primary_fixed"] = on_primary_fixed.toArgb();
      m["on_primary_fixed_variant"] = on_primary_fixed_variant.toArgb();
      m["surface_tint"] = primary_adjusted.toArgb();
      m["secondary"] = secondary_adjusted.toArgb();
      m["on_secondary"] = on_secondary.toArgb();
      m["secondary_container"] = secondary_container.toArgb();
      m["on_secondary_container"] = on_secondary_container.toArgb();
      m["secondary_fixed"] = secondary_fixed.toArgb();
      m["secondary_fixed_dim"] = secondary_fixed_dim.toArgb();
      m["on_secondary_fixed"] = on_secondary_fixed.toArgb();
      m["on_secondary_fixed_variant"] = on_secondary_fixed_variant.toArgb();
      m["tertiary"] = tertiary_adjusted.toArgb();
      m["on_tertiary"] = on_tertiary.toArgb();
      m["tertiary_container"] = tertiary_container.toArgb();
      m["on_tertiary_container"] = on_tertiary_container.toArgb();
      m["tertiary_fixed"] = tertiary_fixed.toArgb();
      m["tertiary_fixed_dim"] = tertiary_fixed_dim.toArgb();
      m["on_tertiary_fixed"] = on_tertiary_fixed.toArgb();
      m["on_tertiary_fixed_variant"] = on_tertiary_fixed_variant.toArgb();
      m["error"] = error.toArgb();
      m["on_error"] = on_error.toArgb();
      m["error_container"] = error_container.toArgb();
      m["on_error_container"] = on_error_container.toArgb();
      m["surface"] = surface.toArgb();
      m["on_surface"] = on_surface.toArgb();
      m["surface_variant"] = surface_variant.toArgb();
      m["on_surface_variant"] = on_surface_variant.toArgb();
      m["surface_dim"] = surface_dim.toArgb();
      m["surface_bright"] = surface_bright.toArgb();
      m["surface_container_lowest"] = surface_container_lowest.toArgb();
      m["surface_container_low"] = surface_container_low.toArgb();
      m["surface_container"] = surface_container.toArgb();
      m["surface_container_high"] = surface_container_high.toArgb();
      m["surface_container_highest"] = surface_container_highest.toArgb();
      m["outline"] = outline.toArgb();
      m["outline_variant"] = outline_variant.toArgb();
      m["shadow"] = shadow.toArgb();
      m["scrim"] = scrim.toArgb();
      m["inverse_surface"] = inverse_surface.toArgb();
      m["inverse_on_surface"] = inverse_on_surface.toArgb();
      m["inverse_primary"] = inverse_primary.toArgb();
      m["background"] = background.toArgb();
      m["on_background"] = on_background.toArgb();
      return m;
    }

    TokenMap generateMutedDark(const std::vector<Color>& palette) {
      const Color primary = palette.empty() ? Color(128, 128, 128) : palette[0];
      auto [primary_h, primary_s, _pl] = primary.toHsl();
      (void)_pl;

      const Color secondary = shiftHue(primary, 15.0);
      const Color tertiary = shiftHue(primary, 30.0);
      const Color error = findErrorColor(palette);

      constexpr double MUTED_SAT_PRIMARY = 0.15;
      constexpr double MUTED_SAT_SECONDARY = 0.12;
      constexpr double MUTED_SAT_TERTIARY = 0.10;
      constexpr double MUTED_SAT_SURFACE = 0.08;

      auto [ph, ps, pl] = primary.toHsl();
      const Color primary_adjusted = fromHsl(ph, std::min(ps, MUTED_SAT_PRIMARY), std::max(pl, 0.65));
      auto [sh2, ss2, sl2] = secondary.toHsl();
      const Color secondary_adjusted = fromHsl(sh2, std::min(ss2, MUTED_SAT_SECONDARY), std::max(sl2, 0.60));
      auto [th2, ts2, tl2] = tertiary.toHsl();
      const Color tertiary_adjusted = fromHsl(th2, std::min(ts2, MUTED_SAT_TERTIARY), std::max(tl2, 0.60));

      auto makeContainerDark = [&](const Color& base) {
        auto [h, s, l] = base.toHsl();
        return fromHsl(h, std::min(s + 0.05, MUTED_SAT_PRIMARY), std::max(l - 0.35, 0.15));
      };
      const Color primary_container = makeContainerDark(primary_adjusted);
      const Color secondary_container = makeContainerDark(secondary_adjusted);
      const Color tertiary_container = makeContainerDark(tertiary_adjusted);
      const Color error_container = makeContainerDark(error);

      const Color base_surface = fromHsl(primary_h, MUTED_SAT_SURFACE, 0.5);
      const Color surface = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.12);
      const Color surface_variant = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.16);
      const Color surface_container_lowest = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.06);
      const Color surface_container_low = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.10);
      const Color surface_container = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.20);
      const Color surface_container_high = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.18);
      const Color surface_container_highest = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.22);

      const Color base_on_surface = fromHsl(primary_h, 0.03, 0.95);
      const Color on_surface = ensureContrast(base_on_surface, surface, 4.5);
      const Color base_on_surface_variant = fromHsl(primary_h, 0.03, 0.70);
      const Color on_surface_variant = ensureContrast(base_on_surface_variant, surface_variant, 4.5);

      const Color outline = ensureContrast(fromHsl(primary_h, 0.05, 0.30), surface, 3.0);
      const Color outline_variant = ensureContrast(fromHsl(primary_h, 0.05, 0.40), surface, 3.0);

      const Color dark_fg = fromHsl(primary_h, 0.10, 0.12);
      const Color on_primary = ensureContrast(dark_fg, primary_adjusted, 7.0);
      const Color on_secondary = ensureContrast(dark_fg, secondary_adjusted, 7.0);
      const Color on_tertiary = ensureContrast(dark_fg, tertiary_adjusted, 7.0);
      const Color on_error = ensureContrast(dark_fg, error, 7.0);

      const Color on_primary_container = ensureContrast(fromHsl(primary_h, 0.05, 0.90), primary_container, 4.5, 1);
      auto [sec_h, _ss, _sl] = secondary.toHsl();
      (void)_ss;
      (void)_sl;
      const Color on_secondary_container = ensureContrast(fromHsl(sec_h, 0.05, 0.90), secondary_container, 4.5, 1);
      auto [ter_h, _ts, _tl] = tertiary.toHsl();
      (void)_ts;
      (void)_tl;
      const Color on_tertiary_container = ensureContrast(fromHsl(ter_h, 0.05, 0.90), tertiary_container, 4.5, 1);
      auto [err_h, _es, _el] = error.toHsl();
      (void)_es;
      (void)_el;
      const Color on_error_container = ensureContrast(fromHsl(err_h, 0.05, 0.90), error_container, 4.5, 1);

      const Color shadow = surface;
      const Color scrim = Color(0, 0, 0);

      const Color inverse_surface = fromHsl(primary_h, 0.05, 0.90);
      const Color inverse_on_surface = fromHsl(primary_h, 0.03, 0.15);
      const Color inverse_primary = fromHsl(primary_h, std::min(primary_s * 0.5, MUTED_SAT_PRIMARY), 0.40);

      const Color background = surface;
      const Color on_background = on_surface;

      auto makeFixedDark = [&](const Color& base) {
        auto [h, s, l] = base.toHsl();
        (void)l;
        return std::pair<Color, Color>{
            fromHsl(h, std::min(s, MUTED_SAT_PRIMARY), 0.85), fromHsl(h, std::min(s, MUTED_SAT_PRIMARY), 0.75)
        };
      };
      auto [primary_fixed, primary_fixed_dim] = makeFixedDark(primary_adjusted);
      auto [secondary_fixed, secondary_fixed_dim] = makeFixedDark(secondary_adjusted);
      auto [tertiary_fixed, tertiary_fixed_dim] = makeFixedDark(tertiary_adjusted);

      const Color on_primary_fixed = ensureContrast(fromHsl(primary_h, 0.05, 0.15), primary_fixed, 4.5);
      const Color on_primary_fixed_variant = ensureContrast(fromHsl(primary_h, 0.05, 0.20), primary_fixed_dim, 4.5);
      const Color on_secondary_fixed = ensureContrast(fromHsl(sec_h, 0.05, 0.15), secondary_fixed, 4.5);
      const Color on_secondary_fixed_variant = ensureContrast(fromHsl(sec_h, 0.05, 0.20), secondary_fixed_dim, 4.5);
      const Color on_tertiary_fixed = ensureContrast(fromHsl(ter_h, 0.05, 0.15), tertiary_fixed, 4.5);
      const Color on_tertiary_fixed_variant = ensureContrast(fromHsl(ter_h, 0.05, 0.20), tertiary_fixed_dim, 4.5);

      const Color surface_dim = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.08);
      const Color surface_bright = adjustSurface(base_surface, MUTED_SAT_SURFACE, 0.24);

      TokenMap m;
      m["primary"] = primary_adjusted.toArgb();
      m["on_primary"] = on_primary.toArgb();
      m["primary_container"] = primary_container.toArgb();
      m["on_primary_container"] = on_primary_container.toArgb();
      m["primary_fixed"] = primary_fixed.toArgb();
      m["primary_fixed_dim"] = primary_fixed_dim.toArgb();
      m["on_primary_fixed"] = on_primary_fixed.toArgb();
      m["on_primary_fixed_variant"] = on_primary_fixed_variant.toArgb();
      m["surface_tint"] = primary_adjusted.toArgb();
      m["secondary"] = secondary_adjusted.toArgb();
      m["on_secondary"] = on_secondary.toArgb();
      m["secondary_container"] = secondary_container.toArgb();
      m["on_secondary_container"] = on_secondary_container.toArgb();
      m["secondary_fixed"] = secondary_fixed.toArgb();
      m["secondary_fixed_dim"] = secondary_fixed_dim.toArgb();
      m["on_secondary_fixed"] = on_secondary_fixed.toArgb();
      m["on_secondary_fixed_variant"] = on_secondary_fixed_variant.toArgb();
      m["tertiary"] = tertiary_adjusted.toArgb();
      m["on_tertiary"] = on_tertiary.toArgb();
      m["tertiary_container"] = tertiary_container.toArgb();
      m["on_tertiary_container"] = on_tertiary_container.toArgb();
      m["tertiary_fixed"] = tertiary_fixed.toArgb();
      m["tertiary_fixed_dim"] = tertiary_fixed_dim.toArgb();
      m["on_tertiary_fixed"] = on_tertiary_fixed.toArgb();
      m["on_tertiary_fixed_variant"] = on_tertiary_fixed_variant.toArgb();
      m["error"] = error.toArgb();
      m["on_error"] = on_error.toArgb();
      m["error_container"] = error_container.toArgb();
      m["on_error_container"] = on_error_container.toArgb();
      m["surface"] = surface.toArgb();
      m["on_surface"] = on_surface.toArgb();
      m["surface_variant"] = surface_variant.toArgb();
      m["on_surface_variant"] = on_surface_variant.toArgb();
      m["surface_dim"] = surface_dim.toArgb();
      m["surface_bright"] = surface_bright.toArgb();
      m["surface_container_lowest"] = surface_container_lowest.toArgb();
      m["surface_container_low"] = surface_container_low.toArgb();
      m["surface_container"] = surface_container.toArgb();
      m["surface_container_high"] = surface_container_high.toArgb();
      m["surface_container_highest"] = surface_container_highest.toArgb();
      m["outline"] = outline.toArgb();
      m["outline_variant"] = outline_variant.toArgb();
      m["shadow"] = shadow.toArgb();
      m["scrim"] = scrim.toArgb();
      m["inverse_surface"] = inverse_surface.toArgb();
      m["inverse_on_surface"] = inverse_on_surface.toArgb();
      m["inverse_primary"] = inverse_primary.toArgb();
      m["background"] = background.toArgb();
      m["on_background"] = on_background.toArgb();
      return m;
    }

    TokenMap generateMutedLight(const std::vector<Color>& palette) {
      const Color primary = palette.empty() ? Color(128, 128, 128) : palette[0];
      auto [primary_h, primary_s, _pl] = primary.toHsl();
      (void)_pl;

      const Color secondary = shiftHue(primary, 15.0);
      const Color tertiary = shiftHue(primary, 30.0);
      const Color error = findErrorColor(palette);

      constexpr double MUTED_SAT_PRIMARY = 0.15;
      constexpr double MUTED_SAT_SECONDARY = 0.12;
      constexpr double MUTED_SAT_TERTIARY = 0.10;
      constexpr double MUTED_SAT_SURFACE = 0.08;

      auto [ph, ps, pl] = primary.toHsl();
      const Color primary_adjusted = fromHsl(ph, std::min(ps, MUTED_SAT_PRIMARY), std::min(pl, 0.45));
      auto [sh2, ss2, sl2] = secondary.toHsl();
      const Color secondary_adjusted = fromHsl(sh2, std::min(ss2, MUTED_SAT_SECONDARY), std::min(sl2, 0.40));
      auto [th2, ts2, tl2] = tertiary.toHsl();
      const Color tertiary_adjusted = fromHsl(th2, std::min(ts2, MUTED_SAT_TERTIARY), std::min(tl2, 0.35));

      auto makeContainerLight = [](const Color& base) {
        auto [h, s, l] = base.toHsl();
        return fromHsl(h, std::max(s - 0.05, 0.05), std::min(l + 0.35, 0.85));
      };
      const Color primary_container = makeContainerLight(primary_adjusted);
      const Color secondary_container = makeContainerLight(secondary_adjusted);
      const Color tertiary_container = makeContainerLight(tertiary_adjusted);
      const Color error_container = makeContainerLight(error);

      const Color surface = adjustSurface(primary, MUTED_SAT_SURFACE, 0.90);
      const Color surface_variant = adjustSurface(primary, MUTED_SAT_SURFACE, 0.78);
      const Color surface_container_lowest = adjustSurface(primary, MUTED_SAT_SURFACE, 0.96);
      const Color surface_container_low = adjustSurface(primary, MUTED_SAT_SURFACE, 0.92);
      const Color surface_container = adjustSurface(primary, MUTED_SAT_SURFACE, 0.86);
      const Color surface_container_high = adjustSurface(primary, MUTED_SAT_SURFACE, 0.84);
      const Color surface_container_highest = adjustSurface(primary, MUTED_SAT_SURFACE, 0.80);

      const Color base_on_surface = fromHsl(primary_h, 0.03, 0.10);
      const Color on_surface = ensureContrast(base_on_surface, surface, 4.5);
      const Color base_on_surface_variant = fromHsl(primary_h, 0.03, 0.35);
      const Color on_surface_variant = ensureContrast(base_on_surface_variant, surface_variant, 4.5);

      const Color light_fg = fromHsl(primary_h, 0.05, 0.98);
      const Color on_primary = ensureContrast(light_fg, primary_adjusted, 7.0);
      const Color on_secondary = ensureContrast(light_fg, secondary_adjusted, 7.0);
      const Color on_tertiary = ensureContrast(light_fg, tertiary_adjusted, 7.0);
      const Color on_error = ensureContrast(light_fg, error, 7.0);

      const Color on_primary_container = ensureContrast(fromHsl(primary_h, 0.05, 0.15), primary_container, 4.5, -1);
      auto [sec_h, _ss, _sl] = secondary.toHsl();
      (void)_ss;
      (void)_sl;
      const Color on_secondary_container = ensureContrast(fromHsl(sec_h, 0.05, 0.15), secondary_container, 4.5, -1);
      auto [ter_h, _ts, _tl] = tertiary.toHsl();
      (void)_ts;
      (void)_tl;
      const Color on_tertiary_container = ensureContrast(fromHsl(ter_h, 0.05, 0.15), tertiary_container, 4.5, -1);
      auto [err_h, _es, _el] = error.toHsl();
      (void)_es;
      (void)_el;
      const Color on_error_container = ensureContrast(fromHsl(err_h, 0.05, 0.15), error_container, 4.5, -1);

      auto makeFixedLight = [&](const Color& base) {
        auto [h, s, l] = base.toHsl();
        (void)l;
        (void)s;
        return std::pair<Color, Color>{
            fromHsl(h, std::min(s, MUTED_SAT_PRIMARY), 0.40), fromHsl(h, std::min(s, MUTED_SAT_PRIMARY), 0.30)
        };
      };
      auto [primary_fixed, primary_fixed_dim] = makeFixedLight(primary_adjusted);
      auto [secondary_fixed, secondary_fixed_dim] = makeFixedLight(secondary_adjusted);
      auto [tertiary_fixed, tertiary_fixed_dim] = makeFixedLight(tertiary_adjusted);

      const Color on_primary_fixed = ensureContrast(fromHsl(primary_h, 0.05, 0.90), primary_fixed, 4.5);
      const Color on_primary_fixed_variant = ensureContrast(fromHsl(primary_h, 0.05, 0.85), primary_fixed_dim, 4.5);
      const Color on_secondary_fixed = ensureContrast(fromHsl(sec_h, 0.05, 0.90), secondary_fixed, 4.5);
      const Color on_secondary_fixed_variant = ensureContrast(fromHsl(sec_h, 0.05, 0.85), secondary_fixed_dim, 4.5);
      const Color on_tertiary_fixed = ensureContrast(fromHsl(ter_h, 0.05, 0.90), tertiary_fixed, 4.5);
      const Color on_tertiary_fixed_variant = ensureContrast(fromHsl(ter_h, 0.05, 0.85), tertiary_fixed_dim, 4.5);

      const Color surface_dim = adjustSurface(primary, MUTED_SAT_SURFACE, 0.82);
      const Color surface_bright = adjustSurface(primary, MUTED_SAT_SURFACE, 0.95);

      const Color outline = ensureContrast(fromHsl(primary_h, 0.05, 0.65), surface, 3.0);
      const Color outline_variant = ensureContrast(fromHsl(primary_h, 0.05, 0.75), surface, 3.0);
      const Color shadow = fromHsl(primary_h, 0.05, 0.80);
      const Color scrim = Color(0, 0, 0);

      const Color inverse_surface = fromHsl(primary_h, 0.05, 0.15);
      const Color inverse_on_surface = fromHsl(primary_h, 0.03, 0.90);
      const Color inverse_primary = fromHsl(primary_h, std::min(primary_s * 0.5, MUTED_SAT_PRIMARY), 0.70);

      const Color background = surface;
      const Color on_background = on_surface;

      TokenMap m;
      m["primary"] = primary_adjusted.toArgb();
      m["on_primary"] = on_primary.toArgb();
      m["primary_container"] = primary_container.toArgb();
      m["on_primary_container"] = on_primary_container.toArgb();
      m["primary_fixed"] = primary_fixed.toArgb();
      m["primary_fixed_dim"] = primary_fixed_dim.toArgb();
      m["on_primary_fixed"] = on_primary_fixed.toArgb();
      m["on_primary_fixed_variant"] = on_primary_fixed_variant.toArgb();
      m["surface_tint"] = primary_adjusted.toArgb();
      m["secondary"] = secondary_adjusted.toArgb();
      m["on_secondary"] = on_secondary.toArgb();
      m["secondary_container"] = secondary_container.toArgb();
      m["on_secondary_container"] = on_secondary_container.toArgb();
      m["secondary_fixed"] = secondary_fixed.toArgb();
      m["secondary_fixed_dim"] = secondary_fixed_dim.toArgb();
      m["on_secondary_fixed"] = on_secondary_fixed.toArgb();
      m["on_secondary_fixed_variant"] = on_secondary_fixed_variant.toArgb();
      m["tertiary"] = tertiary_adjusted.toArgb();
      m["on_tertiary"] = on_tertiary.toArgb();
      m["tertiary_container"] = tertiary_container.toArgb();
      m["on_tertiary_container"] = on_tertiary_container.toArgb();
      m["tertiary_fixed"] = tertiary_fixed.toArgb();
      m["tertiary_fixed_dim"] = tertiary_fixed_dim.toArgb();
      m["on_tertiary_fixed"] = on_tertiary_fixed.toArgb();
      m["on_tertiary_fixed_variant"] = on_tertiary_fixed_variant.toArgb();
      m["error"] = error.toArgb();
      m["on_error"] = on_error.toArgb();
      m["error_container"] = error_container.toArgb();
      m["on_error_container"] = on_error_container.toArgb();
      m["surface"] = surface.toArgb();
      m["on_surface"] = on_surface.toArgb();
      m["surface_variant"] = surface_variant.toArgb();
      m["on_surface_variant"] = on_surface_variant.toArgb();
      m["surface_dim"] = surface_dim.toArgb();
      m["surface_bright"] = surface_bright.toArgb();
      m["surface_container_lowest"] = surface_container_lowest.toArgb();
      m["surface_container_low"] = surface_container_low.toArgb();
      m["surface_container"] = surface_container.toArgb();
      m["surface_container_high"] = surface_container_high.toArgb();
      m["surface_container_highest"] = surface_container_highest.toArgb();
      m["outline"] = outline.toArgb();
      m["outline_variant"] = outline_variant.toArgb();
      m["shadow"] = shadow.toArgb();
      m["scrim"] = scrim.toArgb();
      m["inverse_surface"] = inverse_surface.toArgb();
      m["inverse_on_surface"] = inverse_on_surface.toArgb();
      m["inverse_primary"] = inverse_primary.toArgb();
      m["background"] = background.toArgb();
      m["on_background"] = on_background.toArgb();
      return m;
    }

  } // namespace

  GeneratedPalette generateCustom(const std::vector<uint8_t>& rgb112, Scheme scheme) {
    const auto palette = extractPalette(rgb112, scheme, 5);
    GeneratedPalette out;
    if (scheme == Scheme::Muted) {
      out.dark = generateMutedDark(palette);
      out.light = generateMutedLight(palette);
    } else {
      out.dark = generateNormalDark(palette);
      out.light = generateNormalLight(palette);
    }
    if (!palette.empty()) {
      out.dark["source_color"] = palette[0].toArgb();
      out.light["source_color"] = palette[0].toArgb();
    }
    synthesizeTerminalPaletteTokens(out);
    return out;
  }

} // namespace noctalia::theme
