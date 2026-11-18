#include "render/text/cairo_glyph_renderer.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4)
    return EXIT_FAILURE;
  CairoGlyphRenderer renderer;
  try {
    renderer.initialize(argv[1], nullptr, nullptr);
    renderer.initializeDisplayFont(argv[2]);
  } catch (const std::exception& e) {
    std::cerr << "font load failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  const auto wordmark = renderer.measureDisplayText(1.0F, "NYTHERA", 25.0F);
  if (!(wordmark.width > 0.0F && wordmark.right > wordmark.left && wordmark.bottom > wordmark.top)) {
    std::cerr << "NYTHERA did not produce direct-font measurements\n";
    return EXIT_FAILURE;
  }
  const auto unsupported = renderer.measureDisplayText(1.0F, "NYTHERA🙂", 25.0F);
  if (unsupported.width != 0.0F) {
    std::cerr << "unsupported glyph unexpectedly produced a partial/fallback measurement\n";
    return EXIT_FAILURE;
  }

  const auto tablerGlyph = renderer.measureGlyph(1.0F, 0xF292, 20.0F);
  if (!(tablerGlyph.width > 0.0F)) {
    std::cerr << "Tabler glyph measurement regressed\n";
    return EXIT_FAILURE;
  }
  TEST_CHECK(renderer.measureGlyph(1.0F, 0x10FFFF, 16.0F).width == 0.0F);

  for (const std::string& path : {std::string(argv[1]) + "/missing.ttf", std::string(argv[3])}) {
    CairoGlyphRenderer optionalFont;
    optionalFont.initialize(path, nullptr, nullptr, false);
    optionalFont.initializeDisplayFont(argv[2]);
    TEST_CHECK(optionalFont.measureGlyph(1.0F, 0xF292, 16.0F).width == 0.0F);
    TEST_CHECK(optionalFont.measureDisplayText(1.0F, "NYTHERA", 25.0F).width > 0.0F);

    bool rejected = false;
    try {
      CairoGlyphRenderer requiredFont;
      requiredFont.initialize(path, nullptr, nullptr);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
  }
  std::cout
      << "NYTHERA direct width="
      << wordmark.width
      << " ink="
      << wordmark.left
      << ','
      << wordmark.right
      << " Tabler check="
      << tablerGlyph.width
      << '\n';
  return EXIT_SUCCESS;
}
