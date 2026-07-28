#include "theme/color.h"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

  bool fail(std::string_view msg) {
    std::fprintf(stderr, "color_test: FAIL: %.*s\n", static_cast<int>(msg.size()), msg.data());
    return false;
  }

  bool expect(bool cond, std::string_view msg) { return cond ? true : fail(msg); }

  bool near(double a, double b, double eps = 0.005) { return std::fabs(a - b) < eps; }

  bool testRoundTrip() {
    using noctalia::theme::Color;
    const Color original{180, 60, 200};
    auto [L, C, H] = original.toOklch();
    const Color result = Color::fromOklch(L, C, H);
    return expect(std::abs(original.r - result.r) <= 1, "round-trip r")
        && expect(std::abs(original.g - result.g) <= 1, "round-trip g")
        && expect(std::abs(original.b - result.b) <= 1, "round-trip b");
  }

  bool testWhite() {
    using noctalia::theme::Color;
    auto [L, C, H] = Color{255, 255, 255}.toOklch();
    return expect(near(L, 1.0, 0.001), "white L=1")
        && expect(C < 0.02, "white C≈0");
  }

  bool testBlack() {
    using noctalia::theme::Color;
    auto [L, C, H] = Color{0, 0, 0}.toOklch();
    return expect(near(L, 0.0, 0.001), "black L=0")
        && expect(C < 0.02, "black C≈0");
  }

  // set_lightness 40 → OkLCH L = 0.40, C and H unchanged
  bool testSetLightness() {
    using noctalia::theme::Color;
    const Color col = Color::fromHex("#e06c75");
    auto [L0, C0, H0] = col.toOklch();
    const Color adjusted = Color::fromOklch(0.40, C0, H0);
    auto [L1, C1, H1] = adjusted.toOklch();
    return expect(near(L1, 0.40, 0.005), "set_lightness sets L to 0.40")
        && expect(near(C1, C0, 0.005), "chroma preserved")
        && expect(near(std::fmod(H1 - H0 + 360.0, 360.0), 0.0, 1.0), "hue preserved");
  }

  // lighten 10 → L increases by 0.10 in OkLCH
  bool testLighten() {
    using noctalia::theme::Color;
    const Color col = Color::fromHex("#333344");
    auto [L0, C0, H0] = col.toOklch();
    const double newL = std::min(L0 + 0.10, 1.0);
    auto [L1, C1, H1] = Color::fromOklch(newL, C0, H0).toOklch();
    return expect(near(L1, newL, 0.005), "lighten 10 adds 0.10 to OkLCH L");
  }

  // darken 10 → L decreases by 0.10 in OkLCH
  bool testDarken() {
    using noctalia::theme::Color;
    const Color col = Color::fromHex("#aabbcc");
    auto [L0, C0, H0] = col.toOklch();
    const double newL = std::max(L0 - 0.10, 0.0);
    auto [L1, C1, H1] = Color::fromOklch(newL, C0, H0).toOklch();
    return expect(near(L1, newL, 0.005), "darken 10 subtracts 0.10 from OkLCH L");
  }

} // namespace

int main() {
  bool ok = true;
  ok = testRoundTrip()    && ok;
  ok = testWhite()        && ok;
  ok = testBlack()        && ok;
  ok = testSetLightness() && ok;
  ok = testLighten()      && ok;
  ok = testDarken()       && ok;
  return ok ? 0 : 1;
}
