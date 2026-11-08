#include "render/codecs/color_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <lcms2.h>
#include <map>
#include <memory>
#include <type_traits>

namespace codecs {

  namespace {

    // Pixels per cmsDoTransform call: bounds the scratch buffer for large wallpapers.
    constexpr std::size_t kChunkPixels = 1 << 16;
    // Colorant (XYZ, D50) difference below which a profile counts as sRGB; real wide-gamut profiles
    // differ from sRGB by more than 0.05 in at least one component.
    constexpr double kSrgbColorantTolerance = 0.002;
    // Tone curves are compared at this many points, within half an 8-bit step.
    constexpr int kCurveSamples = 32;
    constexpr float kSrgbCurveTolerance = 0.5F / 255.0F;

    struct ContextDeleter {
      void operator()(cmsContext context) const { cmsDeleteContext(context); }
    };
    struct ProfileDeleter {
      void operator()(cmsHPROFILE profile) const { cmsCloseProfile(profile); }
    };
    struct TransformDeleter {
      void operator()(cmsHTRANSFORM transform) const { cmsDeleteTransform(transform); }
    };
    struct CurveDeleter {
      void operator()(cmsToneCurve* curve) const { cmsFreeToneCurve(curve); }
    };
    using Context = std::unique_ptr<std::remove_pointer_t<cmsContext>, ContextDeleter>;
    using Profile = std::unique_ptr<void, ProfileDeleter>;
    using Transform = std::unique_ptr<void, TransformDeleter>;
    using Curve = std::unique_ptr<cmsToneCurve, CurveDeleter>;

    bool hasSrgbColorants(cmsHPROFILE profile, cmsHPROFILE srgb) {
      constexpr std::array kColorants{cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
      return std::ranges::all_of(kColorants, [&](cmsTagSignature tag) {
        const auto* own = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, tag));
        const auto* reference = static_cast<const cmsCIEXYZ*>(cmsReadTag(srgb, tag));
        return own != nullptr
            && reference != nullptr
            && std::abs(own->X - reference->X) <= kSrgbColorantTolerance
            && std::abs(own->Y - reference->Y) <= kSrgbColorantTolerance
            && std::abs(own->Z - reference->Z) <= kSrgbColorantTolerance;
      });
    }

    bool sameCurve(const cmsToneCurve* curve, const cmsToneCurve* reference) {
      for (int i = 0; i < kCurveSamples; ++i) {
        const float x = static_cast<float>(i) / (kCurveSamples - 1);
        if (std::abs(cmsEvalToneCurveFloat(curve, x) - cmsEvalToneCurveFloat(reference, x)) > kSrgbCurveTolerance) {
          return false;
        }
      }
      return true;
    }

    // A profile can share sRGB's primaries with another curve (linear sRGB, gamma 2.2), so both must match.
    // Curves are compared by value, since sRGB's may be stored as a formula or a table.
    // A profile without TRC tags (a LUT profile) fails this check and is converted.
    bool hasSrgbCurves(cmsHPROFILE profile, cmsHPROFILE srgb) {
      constexpr std::array kCurves{cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
      const auto* reference = static_cast<const cmsToneCurve*>(cmsReadTag(srgb, cmsSigRedTRCTag));
      if (reference == nullptr) {
        return false;
      }
      return std::ranges::all_of(kCurves, [&](cmsTagSignature tag) {
        const auto* own = static_cast<const cmsToneCurve*>(cmsReadTag(profile, tag));
        return own != nullptr && sameCurve(own, reference);
      });
    }

    // Lcms2 objects are created in a context of their own: decoding runs on several worker threads.
    void transformToSrgb(std::vector<std::uint8_t>& rgba, cmsContext context, cmsHPROFILE source) {
      if (source == nullptr || cmsGetColorSpace(source) != cmsSigRgbData) {
        return;
      }
      const Profile srgb(cmsCreate_sRGBProfileTHR(context));
      if (srgb == nullptr || (hasSrgbColorants(source, srgb.get()) && hasSrgbCurves(source, srgb.get()))) {
        return;
      }
      // The output is a scratch chunk copied back over the pixels, so alpha must be copied through.
      const Transform transform(cmsCreateTransformTHR(
          context, source, TYPE_RGBA_8, srgb.get(), TYPE_RGBA_8, INTENT_RELATIVE_COLORIMETRIC,
          cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA
      ));
      if (transform == nullptr) {
        return;
      }
      const std::size_t pixels = rgba.size() / 4;
      std::vector<std::uint8_t> chunk(std::min(pixels, kChunkPixels) * 4);
      for (std::size_t first = 0; first < pixels; first += kChunkPixels) {
        const std::size_t count = std::min(kChunkPixels, pixels - first);
        std::uint8_t* at = rgba.data() + first * 4;
        cmsDoTransform(transform.get(), at, chunk.data(), static_cast<cmsUInt32Number>(count));
        std::memcpy(at, chunk.data(), count * 4);
      }
    }

    struct Primaries {
      cmsCIExyY red;
      cmsCIExyY green;
      cmsCIExyY blue;
      cmsCIExyY white;
    };

    constexpr cmsCIExyY kD65{0.3127, 0.3290, 1.0};

    // H.273 ColourPrimaries; BT.709 (1) and unspecified (2) are sRGB's own.
    const Primaries* primariesFor(int code) {
      static constexpr Primaries kBt2020{{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0}, {0.131, 0.046, 1.0}, kD65};
      static constexpr Primaries kDciP3{
          {0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}, {0.314, 0.351, 1.0}
      };
      static constexpr Primaries kDisplayP3{{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}, kD65};
      switch (code) {
      case 9:
        return &kBt2020;
      case 11:
        return &kDciP3;
      case 12:
        return &kDisplayP3;
      default:
        return nullptr;
      }
    }

    // H.273 TransferCharacteristics as an lcms2 curve; null for unknown ones and for HDR (PQ, HLG), which would need
    // tone mapping, not a colorimetric transform that clips every highlight.
    Curve transferFor(cmsContext context, int code) {
      // Type 4: Y = (aX + b)^g for X >= d, Y = cX below.
      static constexpr std::array<cmsFloat64Number, 5> kSrgb{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
      static constexpr std::array<cmsFloat64Number, 5> kBt709{1.0 / 0.45, 1.0 / 1.099, 0.099 / 1.099, 1.0 / 4.5, 0.081};
      switch (code) {
      case 1:
      case 6:
      case 14:
      case 15:
        return Curve(cmsBuildParametricToneCurve(context, 4, kBt709.data()));
      case 2: // unspecified: the sRGB curve, as for untagged images
      case 13:
        return Curve(cmsBuildParametricToneCurve(context, 4, kSrgb.data()));
      case 4:
        return Curve(cmsBuildGamma(context, 2.2));
      case 5:
        return Curve(cmsBuildGamma(context, 2.8));
      case 8:
        return Curve(cmsBuildGamma(context, 1.0));
      default:
        return nullptr;
      }
    }

  } // namespace

  void convertIccToSrgb(std::vector<std::uint8_t>& rgba, std::span<const std::uint8_t> icc) {
    if (icc.empty() || rgba.empty()) {
      return;
    }
    const Context context(cmsCreateContext(nullptr, nullptr));
    if (context == nullptr) {
      return;
    }
    const Profile source(cmsOpenProfileFromMemTHR(context.get(), icc.data(), static_cast<cmsUInt32Number>(icc.size())));
    transformToSrgb(rgba, context.get(), source.get());
  }

  void convertCicpToSrgb(std::vector<std::uint8_t>& rgba, int primaries, int transfer) {
    const Primaries* space = primariesFor(primaries);
    if (space == nullptr || rgba.empty()) {
      return;
    }
    const Context context(cmsCreateContext(nullptr, nullptr));
    if (context == nullptr) {
      return;
    }
    const Curve curve = transferFor(context.get(), transfer);
    if (curve == nullptr) {
      return;
    }
    const cmsCIExyYTRIPLE triple{space->red, space->green, space->blue};
    std::array<cmsToneCurve*, 3> curves{curve.get(), curve.get(), curve.get()};
    const Profile source(cmsCreateRGBProfileTHR(context.get(), &space->white, &triple, curves.data()));
    transformToSrgb(rgba, context.get(), source.get());
  }

  std::vector<std::uint8_t> jpegIccProfile(const std::uint8_t* data, std::size_t size) {
    static constexpr std::array<std::uint8_t, 12> kSignature{'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
    if (data == nullptr || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
      return {};
    }
    // Profiles over 64 KiB span several APP2 segments, numbered from 1.
    std::map<int, std::span<const std::uint8_t>> chunks;
    int expected = 0;
    std::size_t pos = 2;
    while (pos + 4 <= size && data[pos] == 0xFF) {
      const std::uint8_t marker = data[pos + 1];
      if (marker == 0xFF) {
        ++pos;
        continue;
      }
      if (marker == 0xDA || marker == 0xD9) {
        break; // the metadata segments come before the scan
      }
      const std::size_t length = (static_cast<std::size_t>(data[pos + 2]) << 8) | data[pos + 3];
      if (length < 2 || pos + 2 + length > size) {
        break;
      }
      const std::uint8_t* segment = data + pos + 4;
      const std::size_t segmentSize = length - 2;
      if (marker == 0xE2
          && segmentSize > kSignature.size() + 2
          && std::memcmp(segment, kSignature.data(), kSignature.size()) == 0) {
        const int sequence = segment[kSignature.size()];
        expected = segment[kSignature.size() + 1];
        chunks[sequence] =
            std::span<const std::uint8_t>(segment + kSignature.size() + 2, segmentSize - kSignature.size() - 2);
      }
      pos += 2 + length;
    }
    if (expected == 0 || chunks.size() != static_cast<std::size_t>(expected)) {
      return {};
    }
    std::vector<std::uint8_t> profile;
    for (int sequence = 1; sequence <= expected; ++sequence) {
      const auto it = chunks.find(sequence);
      if (it == chunks.end()) {
        return {};
      }
      profile.insert(profile.end(), it->second.begin(), it->second.end());
    }
    return profile;
  }

} // namespace codecs
