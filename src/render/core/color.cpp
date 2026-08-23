#include "render/core/color.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <system_error>

namespace {

  float linearizedColorChannel(float channel) {
    channel = std::clamp(channel, 0.0F, 1.0F);
    if (channel <= 0.03928F) {
      return channel / 12.92F;
    }
    return std::pow((channel + 0.055F) / 1.055F, 2.4F);
  }

} // namespace

Color hsv(float h, float s, float v, float a) {
  h = h - std::floor(h);
  const float saturation = std::clamp(s, 0.0F, 1.0F);
  const float value = std::clamp(v, 0.0F, 1.0F);
  const float chroma = value * saturation;
  const float hh = h * 6.0F;
  const float x = chroma * (1.0F - std::fabs(std::fmod(hh, 2.0F) - 1.0F));

  float rp = 0.0F;
  float gp = 0.0F;
  float bp = 0.0F;
  switch (static_cast<int>(hh) % 6) {
  case 0:
    rp = chroma;
    gp = x;
    break;
  case 1:
    rp = x;
    gp = chroma;
    break;
  case 2:
    gp = chroma;
    bp = x;
    break;
  case 3:
    gp = x;
    bp = chroma;
    break;
  case 4:
    rp = x;
    bp = chroma;
    break;
  default:
    rp = chroma;
    bp = x;
    break;
  }

  const float m = value - chroma;
  return rgba(rp + m, gp + m, bp + m, std::clamp(a, 0.0F, 1.0F));
}

Color hsl(float h, float s, float l, float a) {
  h = std::fmod(h, 360.0F);
  if (h < 0.0F) {
    h += 360.0F;
  }
  s = std::clamp(s, 0.0F, 1.0F);
  l = std::clamp(l, 0.0F, 1.0F);

  const float chroma = (1.0F - std::fabs(2.0F * l - 1.0F)) * s;
  const float x = chroma * (1.0F - std::fabs(std::fmod(h / 60.0F, 2.0F) - 1.0F));
  const float m = l - chroma / 2.0F;

  float rp = 0.0F;
  float gp = 0.0F;
  float bp = 0.0F;
  if (h < 60.0F) {
    rp = chroma;
    gp = x;
  } else if (h < 120.0F) {
    rp = x;
    gp = chroma;
  } else if (h < 180.0F) {
    gp = chroma;
    bp = x;
  } else if (h < 240.0F) {
    gp = x;
    bp = chroma;
  } else if (h < 300.0F) {
    rp = x;
    bp = chroma;
  } else {
    rp = chroma;
    bp = x;
  }

  return rgba(rp + m, gp + m, bp + m, std::clamp(a, 0.0F, 1.0F));
}

void rgbToHsv(const Color& rgb, float& h, float& s, float& v) {
  const float maxChannel = std::max({rgb.r, rgb.g, rgb.b});
  const float minChannel = std::min({rgb.r, rgb.g, rgb.b});
  const float delta = maxChannel - minChannel;

  v = maxChannel;
  if (maxChannel <= 1e-6F) {
    h = 0.0F;
    s = 0.0F;
    return;
  }

  s = delta / maxChannel;
  if (delta <= 1e-6F) {
    h = 0.0F;
    return;
  }

  if (maxChannel == rgb.r) {
    h = (rgb.g - rgb.b) / delta + (rgb.g < rgb.b ? 6.0F : 0.0F);
  } else if (maxChannel == rgb.g) {
    h = (rgb.b - rgb.r) / delta + 2.0F;
  } else {
    h = (rgb.r - rgb.g) / delta + 4.0F;
  }

  h /= 6.0F;
  h = h - std::floor(h);
}

Color lerpHsv(const Color& a, const Color& b, float t) {
  float h0, s0, v0;
  rgbToHsv(a, h0, s0, v0);
  float h1, s1, v1;
  rgbToHsv(b, h1, s1, v1);

  // Hue is undefined at negligible chroma; borrow the other endpoint's hue to avoid spurious tints.
  constexpr float kChromaEpsilon = 1e-6F;
  if (s0 * v0 <= kChromaEpsilon) {
    h0 = h1;
  }
  if (s1 * v1 <= kChromaEpsilon) {
    h1 = h0;
  }

  float hDelta = h1 - h0;
  if (hDelta > 0.5F) {
    hDelta -= 1.0F;
  } else if (hDelta < -0.5F) {
    hDelta += 1.0F;
  }
  return hsv(h0 + hDelta * t, s0 + (s1 - s0) * t, v0 + (v1 - v0) * t, a.a + (b.a - a.a) * t);
}

Color lerpHsvChromaWeighted(const Color& a, const Color& b, float t) {
  float h0, s0, v0;
  rgbToHsv(a, h0, s0, v0);
  float h1, s1, v1;
  rgbToHsv(b, h1, s1, v1);

  float hDelta = h1 - h0;
  if (hDelta > 0.5F) {
    hDelta -= 1.0F;
  } else if (hDelta < -0.5F) {
    hDelta += 1.0F;
  }

  // A low-chroma color's hue is weak or undefined, so give it proportionally less influence. When both endpoints
  // have equal chroma this reduces to the usual linear hue interpolation.
  const float fromHueWeight = (1.0F - t) * s0 * v0;
  const float toHueWeight = t * s1 * v1;
  const float hueWeightSum = fromHueWeight + toHueWeight;
  const float hueT = hueWeightSum > 0.0F ? toHueWeight / hueWeightSum : t;

  return hsv(h0 + hDelta * hueT, s0 + (s1 - s0) * t, v0 + (v1 - v0) * t, a.a + (b.a - a.a) * t);
}

float relativeLuminance(const Color& color) {
  return 0.2126F * linearizedColorChannel(color.r)
      + 0.7152F * linearizedColorChannel(color.g)
      + 0.0722F * linearizedColorChannel(color.b);
}

Color readableTextColorForBackground(const Color& background) {
  return relativeLuminance(background) > 0.179F ? rgba(0.0F, 0.0F, 0.0F) : rgba(1.0F, 1.0F, 1.0F);
}

std::string formatRgbHex(const Color& color) {
  auto toByte = [](float channel) { return static_cast<int>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F)); };

  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", toByte(color.r), toByte(color.g), toByte(color.b));
  return std::string(buffer);
}

bool tryParseHexColor(std::string_view input, Color& out) {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
    input.remove_prefix(1);
  }
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0) {
    input.remove_suffix(1);
  }
  if (input.empty()) {
    return false;
  }

  std::string normalized(input);
  if (!normalized.empty() && normalized.front() != '#') {
    normalized.insert(normalized.begin(), '#');
  }

  try {
    out = hex(normalized);
    return true;
  } catch (...) {
    return false;
  }
}

bool tryParseCssColor(std::string_view text, Color& out) {
  while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return false;
  }

  // Hex requires a literal '#'. Use hex() rather than tryParseHexColor(): the latter
  // inserts a missing '#', which would treat plain words like "facade" as colors.
  if (text.front() == '#') {
    try {
      out = hex(text);
      return true;
    } catch (...) {
      return false;
    }
  }

  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  const std::string_view lv(lower);

  const bool isRgba = lv.starts_with("rgba(") && lv.back() == ')';
  const bool isRgb = !isRgba && lv.starts_with("rgb(") && lv.back() == ')';
  const bool isHsla = lv.starts_with("hsla(") && lv.back() == ')';
  const bool isHsl = !isHsla && lv.starts_with("hsl(") && lv.back() == ')';

  auto skipSpaces = [](std::string_view& sv) {
    while (!sv.empty() && sv.front() == ' ') {
      sv.remove_prefix(1);
    }
  };
  // Between components: an optional comma (legacy) or just whitespace (CSS Color 4).
  auto consumeSeparator = [&](std::string_view& sv) {
    skipSpaces(sv);
    if (!sv.empty() && sv.front() == ',') {
      sv.remove_prefix(1);
    }
    skipSpaces(sv);
  };
  // Before alpha: a comma (legacy) or a slash (CSS Color 4) is required.
  auto consumeAlphaSeparator = [&](std::string_view& sv) -> bool {
    skipSpaces(sv);
    if (!sv.empty() && (sv.front() == ',' || sv.front() == '/')) {
      sv.remove_prefix(1);
      skipSpaces(sv);
      return true;
    }
    return false;
  };
  // Alpha: number in [0,1] or percentage in [0,100]%.
  auto parseAlpha = [&](std::string_view& sv, float& result) -> bool {
    skipSpaces(sv);
    float v = 0.0F;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{}) {
      return false;
    }
    sv.remove_prefix(static_cast<std::size_t>(ptr - sv.data()));
    if (!sv.empty() && sv.front() == '%') {
      sv.remove_prefix(1);
      v /= 100.0F;
    }
    if (v < 0.0F || v > 1.0F) {
      return false;
    }
    result = v;
    skipSpaces(sv);
    return true;
  };

  if (isRgb || isRgba) {
    const std::size_t prefixLen = isRgba ? 5 : 4;
    std::string_view inner = lv.substr(prefixLen, lv.size() - prefixLen - 1);
    // Channel: number in [0,255] or percentage in [0,100]%, normalized to [0,1].
    auto parseChannel = [&](std::string_view& sv, float& result) -> bool {
      skipSpaces(sv);
      float v = 0.0F;
      const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
      if (ec != std::errc{}) {
        return false;
      }
      sv.remove_prefix(static_cast<std::size_t>(ptr - sv.data()));
      if (!sv.empty() && sv.front() == '%') {
        sv.remove_prefix(1);
        if (v < 0.0F || v > 100.0F) {
          return false;
        }
        result = v / 100.0F;
      } else {
        if (v < 0.0F || v > 255.0F) {
          return false;
        }
        result = v / 255.0F;
      }
      skipSpaces(sv);
      return true;
    };

    Color color;
    if (!parseChannel(inner, color.r)) {
      return false;
    }
    consumeSeparator(inner);
    if (!parseChannel(inner, color.g)) {
      return false;
    }
    consumeSeparator(inner);
    if (!parseChannel(inner, color.b)) {
      return false;
    }
    skipSpaces(inner);
    if (!inner.empty()) {
      if (!consumeAlphaSeparator(inner) || !parseAlpha(inner, color.a)) {
        return false;
      }
    }
    if (!inner.empty()) {
      return false;
    }
    out = color;
    return true;
  }

  if (isHsl || isHsla) {
    const std::size_t prefixLen = isHsla ? 5 : 4;
    std::string_view inner = lv.substr(prefixLen, lv.size() - prefixLen - 1);
    // Hue: number with an optional angle unit; result in degrees (hsl() wraps the range).
    auto parseHue = [&](std::string_view& sv, float& result) -> bool {
      skipSpaces(sv);
      float v = 0.0F;
      const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
      if (ec != std::errc{}) {
        return false;
      }
      sv.remove_prefix(static_cast<std::size_t>(ptr - sv.data()));
      if (sv.starts_with("deg")) {
        sv.remove_prefix(3);
      } else if (sv.starts_with("grad")) {
        v *= 360.0F / 400.0F;
        sv.remove_prefix(4);
      } else if (sv.starts_with("rad")) {
        v *= 180.0F / std::numbers::pi_v<float>;
        sv.remove_prefix(3);
      } else if (sv.starts_with("turn")) {
        v *= 360.0F;
        sv.remove_prefix(4);
      }
      result = v;
      skipSpaces(sv);
      return true;
    };
    // Saturation/lightness: percentage in [0,100]%, normalized to [0,1].
    auto parsePercent = [&](std::string_view& sv, float& result) -> bool {
      skipSpaces(sv);
      float v = 0.0F;
      const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
      if (ec != std::errc{}) {
        return false;
      }
      sv.remove_prefix(static_cast<std::size_t>(ptr - sv.data()));
      if (sv.empty() || sv.front() != '%' || v < 0.0F || v > 100.0F) {
        return false;
      }
      sv.remove_prefix(1);
      result = v / 100.0F;
      skipSpaces(sv);
      return true;
    };

    float h = 0.0F;
    float s = 0.0F;
    float l = 0.0F;
    float a = 1.0F;
    if (!parseHue(inner, h)) {
      return false;
    }
    consumeSeparator(inner);
    if (!parsePercent(inner, s)) {
      return false;
    }
    consumeSeparator(inner);
    if (!parsePercent(inner, l)) {
      return false;
    }
    skipSpaces(inner);
    if (!inner.empty()) {
      if (!consumeAlphaSeparator(inner) || !parseAlpha(inner, a)) {
        return false;
      }
    }
    if (!inner.empty()) {
      return false;
    }
    out = hsl(h, s, l, a);
    return true;
  }

  return false;
}
