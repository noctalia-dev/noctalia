#include "render/core/color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

  float linearizedColorChannel(float channel) {
    channel = std::clamp(channel, 0.0f, 1.0f);
    if (channel <= 0.03928f) {
      return channel / 12.92f;
    }
    return std::pow((channel + 0.055f) / 1.055f, 2.4f);
  }

} // namespace

Color hsv(float h, float s, float v, float a) {
  h = h - std::floor(h);
  const float saturation = std::clamp(s, 0.0f, 1.0f);
  const float value = std::clamp(v, 0.0f, 1.0f);
  const float chroma = value * saturation;
  const float hh = h * 6.0f;
  const float x = chroma * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));

  float rp = 0.0f;
  float gp = 0.0f;
  float bp = 0.0f;
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
  return rgba(rp + m, gp + m, bp + m, std::clamp(a, 0.0f, 1.0f));
}

void rgbToHsv(const Color& rgb, float& h, float& s, float& v) {
  const float maxChannel = std::max({rgb.r, rgb.g, rgb.b});
  const float minChannel = std::min({rgb.r, rgb.g, rgb.b});
  const float delta = maxChannel - minChannel;

  v = maxChannel;
  if (maxChannel <= 1e-6f) {
    h = 0.0f;
    s = 0.0f;
    return;
  }

  s = delta / maxChannel;
  if (delta <= 1e-6f) {
    h = 0.0f;
    return;
  }

  if (maxChannel == rgb.r) {
    h = ((rgb.g - rgb.b) / delta) + (rgb.g < rgb.b ? 6.0f : 0.0f);
  } else if (maxChannel == rgb.g) {
    h = ((rgb.b - rgb.r) / delta) + 2.0f;
  } else {
    h = ((rgb.r - rgb.g) / delta) + 4.0f;
  }

  h /= 6.0f;
  h = h - std::floor(h);
}

float relativeLuminance(const Color& color) {
  return (0.2126f * linearizedColorChannel(color.r))
      + (0.7152f * linearizedColorChannel(color.g))
      + (0.0722f * linearizedColorChannel(color.b));
}

Color readableTextColorForBackground(const Color& background) {
  return relativeLuminance(background) > 0.179f ? rgba(0.0f, 0.0f, 0.0f) : rgba(1.0f, 1.0f, 1.0f);
}

std::string formatRgbHex(const Color& color) {
  auto toByte = [](float channel) { return static_cast<int>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f)); };

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
