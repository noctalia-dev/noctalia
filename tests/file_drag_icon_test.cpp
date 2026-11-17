#include "filemanager/file_drag_icon.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>

namespace {
  void assertPremultiplied(const NativeDataTransfer::DragIconImage& image) {
    TEST_CHECK(image.width > 0 && image.height > 0);
    TEST_CHECK(image.pixels.size() == static_cast<std::size_t>(image.width) * image.height);
    for (const std::uint32_t pixel : image.pixels) {
      const auto alpha = (pixel >> 24U) & 0xFFU;
      TEST_CHECK(((pixel >> 16U) & 0xFFU) <= alpha);
      TEST_CHECK(((pixel >> 8U) & 0xFFU) <= alpha);
      TEST_CHECK((pixel & 0xFFU) <= alpha);
    }
  }
} // namespace

int main() {
  const Color background{0.07F, 0.09F, 0.14F, 0.9F};
  const Color foreground{0.94F, 0.95F, 1.0F, 1.0F};
  const Color accent{0.95F, 0.18F, 0.13F, 0.8F};

  const auto document = filemanager::renderFileDragIcon("Résumé – 月報.pdf", false, 1, background, foreground, accent);
  TEST_CHECK(document.width == 280 && document.height == 64 && document.scale == 1);
  assertPremultiplied(document);
  TEST_CHECK(document.pixels.front() == 0);
  TEST_CHECK(document.pixels[static_cast<std::size_t>(document.width - 1)] == 0);

  const std::string longLabel = "selected 🌌 item — " + std::string(800, 'x');
  const auto folder = filemanager::renderFileDragIcon(longLabel, true, 2, background, foreground, accent);
  TEST_CHECK(folder.width == 560 && folder.height == 128 && folder.scale == 2);
  assertPremultiplied(folder);
  TEST_CHECK(folder.pixels.front() == 0);
  TEST_CHECK(folder.pixels != document.pixels);

  const std::string malformedUtf8("bad\xFFname", 8);
  const auto repaired = filemanager::renderFileDragIcon(malformedUtf8, false, 0, background, foreground, accent);
  TEST_CHECK(repaired.scale == 1);
  assertPremultiplied(repaired);
}
