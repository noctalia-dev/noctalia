#include "render/codecs/codecs.h"
#include "tests/test_check.h"

#include <cstdio>
#include <print>
#include <string>

int main() {
  // A 1x1 PNG deliberately declared as image/jpeg. The loader should trust the
  // decoded bytes, not the data URI media type.
  const std::string mismatchedDataUri =
      "data:image/jpeg;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=";

  auto image = codecs::loadImageFile(mismatchedDataUri);
  if (!image) {
    std::println(stderr, "load error: {}", image.error());
  }
  TEST_CHECK(image.has_value());
  TEST_CHECK(image->width == 1);
  TEST_CHECK(image->height == 1);
  TEST_CHECK(image->rgba.size() == 4);

  const std::string pngDeclaredJpegDataUri =
      "data:image/png;base64,"
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8Q"
      "EBEQCgwSExIQEw8QEBD/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQ"
      "EAAD8AVN//2Q==";

  image = codecs::loadImageFile(pngDeclaredJpegDataUri);
  if (!image) {
    std::println(stderr, "load error: {}", image.error());
  }
  TEST_CHECK(image.has_value());
  TEST_CHECK(image->width == 1);
  TEST_CHECK(image->height == 1);
  TEST_CHECK(image->rgba.size() == 4);

  image = codecs::loadImageFile("data:image/png;base64,not_base64!");
  TEST_CHECK(!image);
  const bool mentionsBase64 = image.error().contains("base64");
  TEST_CHECK(mentionsBase64);

  image = codecs::loadImageFile("data:image/png;base64");
  TEST_CHECK(!image);
  const bool mentionsSeparator = image.error().contains("separator");
  TEST_CHECK(mentionsSeparator);

  return 0;
}
