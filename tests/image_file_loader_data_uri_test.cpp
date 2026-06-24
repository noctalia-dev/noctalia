#include "render/core/image_file_loader.h"

#include <cstdio>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "image_file_loader_data_uri_test: %s\n", message);
    }
    return condition;
  }

} // namespace

int main() {
  bool ok = true;

  // A 1x1 PNG deliberately declared as image/jpeg. The loader should trust the
  // decoded bytes, not the data URI media type.
  const std::string mismatchedDataUri =
      "data:image/jpeg;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=";

  auto image = loadImageFile(mismatchedDataUri);
  ok = check(image.has_value(), image ? "failed to decode mismatched data URI" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 1, "decoded data URI width should be 1") && ok;
    ok = check(image->height == 1, "decoded data URI height should be 1") && ok;
    ok = check(image->rgba.size() == 4, "decoded data URI should contain one RGBA pixel") && ok;
  }

  const std::string pngDeclaredJpegDataUri =
      "data:image/png;base64,"
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8Q"
      "EBEQCgwSExIQEw8QEBD/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQ"
      "EAAD8AVN//2Q==";

  image = loadImageFile(pngDeclaredJpegDataUri);
  ok = check(image.has_value(), image ? "failed to decode PNG-declared JPEG data URI" : image.error().c_str()) && ok;
  if (image) {
    ok = check(image->width == 1, "decoded JPEG data URI width should be 1") && ok;
    ok = check(image->height == 1, "decoded JPEG data URI height should be 1") && ok;
    ok = check(image->rgba.size() == 4, "decoded JPEG data URI should contain one RGBA pixel") && ok;
  }

  image = loadImageFile("data:image/png;base64,not_base64!");
  ok = check(!image, "invalid base64 data URI should fail") && ok;
  if (!image) {
    const bool mentionsBase64 = image.error().find("base64") != std::string::npos;
    ok = check(mentionsBase64, "invalid base64 failure should explain the data issue") && ok;
  }

  image = loadImageFile("data:image/png;base64");
  ok = check(!image, "data URI without comma should fail") && ok;
  if (!image) {
    const bool mentionsSeparator = image.error().find("separator") != std::string::npos;
    ok = check(mentionsSeparator, "missing comma failure should mention separator") && ok;
  }

  return ok ? 0 : 1;
}
