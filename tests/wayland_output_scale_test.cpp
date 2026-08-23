// Canonical per-output scale resolution: capability order (output-management
// fixed scale, then mode/logical ratio, then integer wl_output.scale), transform
// awareness, anisotropy rejection, and single-value recomputation on update.

#include "wayland/output_scale.h"

#include <print>

namespace {

  int g_failures = 0;

  void expectNumerator(const char* what, std::int32_t actual, std::int32_t expected) {
    if (actual != expected) {
      std::println(stderr, "wayland_output_scale_test: FAIL: {} = {}, expected {}", what, actual, expected);
      ++g_failures;
    }
  }

  void expectTrue(const char* what, bool cond) {
    if (!cond) {
      std::println(stderr, "wayland_output_scale_test: FAIL: {}", what);
      ++g_failures;
    }
  }

} // namespace

int main() {
  using namespace wayland;

  // Factor -> /120 numerator quantization.
  expectNumerator("factor 1.0", scaleNumeratorFromFactor(1.0), 120);
  expectNumerator("factor 1.25", scaleNumeratorFromFactor(1.25), 150);
  expectNumerator("factor 1.5", scaleNumeratorFromFactor(1.5), 180);
  expectNumerator("factor 2.0", scaleNumeratorFromFactor(2.0), 240);
  expectNumerator("factor 0 invalid", scaleNumeratorFromFactor(0.0), 0);
  expectNumerator("factor negative invalid", scaleNumeratorFromFactor(-1.5), 0);
  expectNumerator("factor NaN invalid", scaleNumeratorFromFactor(std::nan("")), 0);

  // Capability 1: output-management fixed scale wins over everything else.
  expectNumerator("head scale wins", resolveConfiguredScaleNumerator(1.5, 2.0, 1), 180);

  // Capability 2: transform-aware mode/logical ratio when no head scale.
  const DetectedScale normal = detectScaleFromDimensions(3840, 2160, 2560, 1440);
  expectTrue("normal available", normal.available && !normal.rotated);
  expectNumerator("normal ratio -> 1.5", scaleNumeratorFromFactor(normal.scale), 180);
  expectNumerator("resolve uses detected", resolveConfiguredScaleNumerator(0.0, normal.scale, 1), 180);

  // Rotated output: physical axes swapped relative to logical; the rotated
  // interpretation is the isotropic one.
  const DetectedScale rotated = detectScaleFromDimensions(2160, 3840, 2560, 1440);
  expectTrue("rotated available + flagged", rotated.available && rotated.rotated);
  expectNumerator("rotated ratio -> 1.5", scaleNumeratorFromFactor(rotated.scale), 180);

  // Anisotropic ratio (neither transform is isotropic) is rejected.
  const DetectedScale aniso = detectScaleFromDimensions(1000, 1000, 500, 800);
  expectTrue("anisotropic rejected", !aniso.available);
  // Non-positive dimensions are rejected.
  const DetectedScale degenerate = detectScaleFromDimensions(0, 1080, 1920, 1080);
  expectTrue("degenerate rejected", !degenerate.available);

  // Capability 3: integer wl_output.scale is the final fallback.
  expectNumerator("integer fallback 2x", resolveConfiguredScaleNumerator(0.0, 0.0, 2), 240);
  expectNumerator("integer fallback default 1x", resolveConfiguredScaleNumerator(0.0, 0.0, 0), 120);
  // Rejected detected ratio falls through to integer scale.
  expectNumerator("rejected detected -> integer", resolveConfiguredScaleNumerator(0.0, aniso.scale, 2), 240);

  // Update/hotplug: recomputation publishes exactly the new numerator, never an
  // accumulation of the previous value.
  std::int32_t published = resolveConfiguredScaleNumerator(2.0, 0.0, 1); // 240
  expectNumerator("published before update", published, 240);
  published = resolveConfiguredScaleNumerator(1.5, 0.0, 1); // recompute -> 180
  expectNumerator("published after update replaces", published, 180);

  if (g_failures != 0) {
    std::println(stderr, "wayland_output_scale_test: {} failure(s)", g_failures);
    return 1;
  }
  return 0;
}
