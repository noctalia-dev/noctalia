#include "filemanager/file_columns.h"

#include <cmath>
#include <iostream>

namespace {
  bool expect(bool value, const char* message) {
    if (!value)
      std::cerr << message << '\n';
    return value;
  }
} // namespace

int main() {
  using namespace filemanager;
  bool ok = true;
  const auto defaults = fitFileColumns({0.0F, 96.0F, 152.0F}, 600.0F);
  ok &= expect(std::abs(defaults.name - 352.0F) < 0.01F, "default Name width does not fill remaining space");
  const auto resized = resizeFileColumnBoundary(defaults, 0, 50.0F, 600.0F);
  ok &= expect(std::abs(resized.name - (defaults.name + 50.0F)) < 0.01F, "Name boundary did not resize Name");
  ok &= expect(std::abs(resized.size - defaults.size) < 0.01F, "Name resize changed Size");
  ok &= expect(std::abs(resized.modified - defaults.modified) < 0.01F, "Name resize changed Modified");
  ok &= expect(resized.name + resized.size > resized.name, "Name/Size boundary positions are not contiguous");
  const auto secondBoundary = resizeFileColumnBoundary(defaults, 1, 50.0F, 600.0F);
  ok &= expect(std::abs(secondBoundary.name - defaults.name) < 0.01F, "Size/Modified boundary changed Name width");
  ok &= expect(
      std::abs(secondBoundary.size - 146.0F) < 0.01F && std::abs(secondBoundary.modified - defaults.modified) < 0.01F,
      "Size/Modified boundary changed the wrong column or failed to preserve Modified"
  );
  const auto modifiedBoundary = resizeFileColumnBoundary(defaults, 2, 30.0F, 600.0F);
  ok &= expect(std::abs(modifiedBoundary.name - defaults.name) < 0.01F, "Modified resize changed Name");
  ok &= expect(std::abs(modifiedBoundary.size - defaults.size) < 0.01F, "Modified resize changed Size");
  ok &= expect(
      std::abs(modifiedBoundary.modified - (defaults.modified + 30.0F)) < 0.01F,
      "Modified trailing boundary did not resize Modified"
  );
  const auto limited = resizeFileColumnBoundary(defaults, 1, -10000.0F, 600.0F);
  ok &= expect(limited.size >= kSizeColumnMin, "Size width fell below minimum");
  ok &= expect(limited.modified == defaults.modified, "Size resize changed Modified");
  const auto nameLimited = resizeFileColumnBoundary(defaults, 0, -10000.0F, 600.0F);
  ok &= expect(nameLimited.name == kNameColumnMin, "Name width fell below minimum");
  const auto narrow = fitFileColumns({0.0F, 96.0F, 152.0F}, 180.0F);
  ok &= expect(narrow.name >= 0.0F && narrow.size >= 0.0F && narrow.modified >= 0.0F, "narrow columns became negative");
  ok &= expect(
      std::abs(narrow.name + narrow.size + narrow.modified - 180.0F) < 0.01F, "narrow widths overlap or leave a gap"
  );
  const FileColumns preferred{303.25F, 101.5F, 177.75F};
  const auto serialized = serializeFileColumns(preferred);
  FileColumns parsed;
  ok &= expect(parseFileColumns(serialized, parsed), "serialized widths did not parse");
  ok &= expect(
      std::abs(parsed.name - 303.25F) < 0.01F
          && std::abs(parsed.size - 101.5F) < 0.01F
          && std::abs(parsed.modified - 177.75F) < 0.01F,
      "parsed widths differ from serialized values"
  );
  const auto narrowPreferredFit = fitFileColumns(parsed, 180.0F);
  const auto restoredFit = fitFileColumns(parsed, 700.0F);
  ok &= expect(
      std::abs(narrowPreferredFit.name + narrowPreferredFit.size + narrowPreferredFit.modified - 180.0F) < 0.01F,
      "narrow preferred widths do not fit the window"
  );
  ok &= expect(
      std::abs(restoredFit.size - preferred.size) < 0.01F
          && std::abs(restoredFit.modified - preferred.modified) < 0.01F,
      "fitting narrow widths overwrote the saved preference"
  );
  ok &= expect(!parseFileColumns("12,nan,14", parsed), "non-finite persisted width was accepted");
  ok &= expect(!parseFileColumns("12,inf,14", parsed), "infinite persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14,16,18", parsed), "extra persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14", parsed), "incomplete persisted widths were accepted");
  ok &= expect(!parseFileColumns("12,14,16,18", parsed), "extra persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14,1000000", parsed), "unbounded persisted width was accepted");
  return ok ? 0 : 1;
}
