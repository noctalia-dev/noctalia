// Canonical per-output scale resolution: capability order (output-management
// fixed scale, then mode/logical ratio, then integer wl_output.scale), transform
// awareness, anisotropy rejection, and single-value recomputation on update.

#include "tests/test_check.h"
#include "wayland/output_scale.h"

#include <cmath>

int main() {
  using namespace wayland;

  // Factor -> /120 numerator quantization.
  TEST_CHECK(scaleNumeratorFromFactor(1.0) == 120);
  TEST_CHECK(scaleNumeratorFromFactor(1.25) == 150);
  TEST_CHECK(scaleNumeratorFromFactor(1.5) == 180);
  TEST_CHECK(scaleNumeratorFromFactor(2.0) == 240);
  TEST_CHECK(scaleNumeratorFromFactor(0.0) == 0);
  TEST_CHECK(scaleNumeratorFromFactor(-1.5) == 0);
  TEST_CHECK(scaleNumeratorFromFactor(std::nan("")) == 0);

  // Capability 1: output-management fixed scale wins over everything else.
  TEST_CHECK(resolveConfiguredScaleNumerator(1.5, 2.0, 1) == 180);

  // Capability 2: transform-aware mode/logical ratio when no head scale.
  const DetectedScale normal = detectScaleFromDimensions(3840, 2160, 2560, 1440);
  TEST_CHECK(normal.available && !normal.rotated);
  TEST_CHECK(scaleNumeratorFromFactor(normal.scale) == 180);
  TEST_CHECK(resolveConfiguredScaleNumerator(0.0, normal.scale, 1) == 180);

  // Rotated output: physical axes swapped relative to logical; the rotated
  // interpretation is the isotropic one.
  const DetectedScale rotated = detectScaleFromDimensions(2160, 3840, 2560, 1440);
  TEST_CHECK(rotated.available && rotated.rotated);
  TEST_CHECK(scaleNumeratorFromFactor(rotated.scale) == 180);

  // Anisotropic ratio (neither transform is isotropic) is rejected.
  const DetectedScale aniso = detectScaleFromDimensions(1000, 1000, 500, 800);
  TEST_CHECK(!aniso.available);
  // Non-positive dimensions are rejected.
  const DetectedScale degenerate = detectScaleFromDimensions(0, 1080, 1920, 1080);
  TEST_CHECK(!degenerate.available);

  // Capability 3: integer wl_output.scale is the final fallback.
  TEST_CHECK(resolveConfiguredScaleNumerator(0.0, 0.0, 2) == 240);
  TEST_CHECK(resolveConfiguredScaleNumerator(0.0, 0.0, 0) == 120);
  // Rejected detected ratio falls through to integer scale.
  TEST_CHECK(resolveConfiguredScaleNumerator(0.0, aniso.scale, 2) == 240);

  // Update/hotplug: recomputation publishes exactly the new numerator, never an
  // accumulation of the previous value.
  std::int32_t published = resolveConfiguredScaleNumerator(2.0, 0.0, 1); // 240
  TEST_CHECK(published == 240);
  published = resolveConfiguredScaleNumerator(1.5, 0.0, 1); // recompute -> 180
  TEST_CHECK(published == 180);

  return 0;
}
