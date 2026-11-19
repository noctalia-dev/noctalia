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
  const auto defaults = fitFileColumns({}, 600.0F);
  ok &= expect(std::abs(defaults[FileColumn::Name] - 352.0F) < 0.01F, "default Name width does not fill remaining space");
  const auto resized = resizeFileColumnBoundary(defaults, FileColumn::Name, 50.0F, 600.0F);
  ok &= expect(std::abs(resized[FileColumn::Name] - (defaults[FileColumn::Name] + 50.0F)) < 0.01F, "Name boundary did not resize Name");
  ok &= expect(std::abs(resized[FileColumn::Size] - defaults[FileColumn::Size]) < 0.01F, "Name resize changed Size");
  ok &= expect(std::abs(resized[FileColumn::Modified] - defaults[FileColumn::Modified]) < 0.01F, "Name resize changed Modified");
  ok &= expect(resized[FileColumn::Name] + resized[FileColumn::Size] > resized[FileColumn::Name], "Name/Size boundary positions are not contiguous");
  const auto secondBoundary = resizeFileColumnBoundary(defaults, FileColumn::Size, 50.0F, 600.0F);
  ok &= expect(std::abs(secondBoundary[FileColumn::Name] - defaults[FileColumn::Name]) < 0.01F, "Size/Modified boundary changed Name width");
  ok &= expect(
      std::abs(secondBoundary[FileColumn::Size] - 146.0F) < 0.01F && std::abs(secondBoundary[FileColumn::Modified] - defaults[FileColumn::Modified]) < 0.01F,
      "Size/Modified boundary changed the wrong column or failed to preserve Modified"
  );
  const auto modifiedBoundary = resizeFileColumnBoundary(defaults, FileColumn::Modified, 30.0F, 600.0F);
  ok &= expect(std::abs(modifiedBoundary[FileColumn::Name] - defaults[FileColumn::Name]) < 0.01F, "Modified resize changed Name");
  ok &= expect(std::abs(modifiedBoundary[FileColumn::Size] - defaults[FileColumn::Size]) < 0.01F, "Modified resize changed Size");
  ok &= expect(
      std::abs(modifiedBoundary[FileColumn::Modified] - (defaults[FileColumn::Modified] + 30.0F)) < 0.01F,
      "Modified trailing boundary did not resize Modified"
  );
  const auto limited = resizeFileColumnBoundary(defaults, FileColumn::Size, -10000.0F, 600.0F);
  ok &= expect(limited[FileColumn::Size] >= kMinimumFileColumns[FileColumn::Size], "Size width fell below minimum");
  ok &= expect(limited[FileColumn::Modified] == defaults[FileColumn::Modified], "Size resize changed Modified");
  const auto nameLimited = resizeFileColumnBoundary(defaults, FileColumn::Name, -10000.0F, 600.0F);
  ok &= expect(nameLimited[FileColumn::Name] == kMinimumFileColumns[FileColumn::Name], "Name width fell below minimum");
  const auto narrow = fitFileColumns({}, 180.0F);
  ok &= expect(narrow[FileColumn::Name] >= 0.0F && narrow[FileColumn::Size] >= 0.0F && narrow[FileColumn::Modified] >= 0.0F, "narrow columns became negative");
  ok &= expect(
      std::abs(narrow[FileColumn::Name] + narrow[FileColumn::Size] + narrow[FileColumn::Modified] - 180.0F) < 0.01F, "narrow widths overlap or leave a gap"
  );
  const FileColumns preferred{{303.25F, 101.5F, 177.75F, 144.0F, 152.0F, 152.0F}};
  const auto serialized = serializeFileColumns(preferred);
  FileColumns parsed;
  ok &= expect(parseFileColumns(serialized, parsed), "serialized widths did not parse");
  ok &= expect(
      std::abs(parsed[FileColumn::Name] - 303.25F) < 0.01F
          && std::abs(parsed[FileColumn::Size] - 101.5F) < 0.01F
          && std::abs(parsed[FileColumn::Modified] - 177.75F) < 0.01F,
      "parsed widths differ from serialized values"
  );
  const auto narrowPreferredFit = fitFileColumns(parsed, 180.0F);
  const auto restoredFit = fitFileColumns(parsed, 700.0F);
  ok &= expect(
      std::abs(narrowPreferredFit[FileColumn::Name] + narrowPreferredFit[FileColumn::Size] + narrowPreferredFit[FileColumn::Modified] - 180.0F) < 0.01F,
      "narrow preferred widths do not fit the window"
  );
  ok &= expect(
      std::abs(restoredFit[FileColumn::Size] - preferred[FileColumn::Size]) < 0.01F
          && std::abs(restoredFit[FileColumn::Modified] - preferred[FileColumn::Modified]) < 0.01F,
      "fitting narrow widths overwrote the saved preference"
  );
  ok &= expect(!parseFileColumns("12,nan,14", parsed), "non-finite persisted width was accepted");
  ok &= expect(!parseFileColumns("12,inf,14", parsed), "infinite persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14,16,18", parsed), "extra persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14", parsed), "incomplete persisted widths were accepted");
  ok &= expect(!parseFileColumns("12,14,16,18", parsed), "extra persisted width was accepted");
  ok &= expect(!parseFileColumns("12,14,1000000", parsed), "unbounded persisted width was accepted");
  ok &= expect(parseFileColumns("303.25,101.5,177.75", parsed), "legacy widths did not parse");
  ok &= expect(parsed[FileColumn::Created] == FileColumns{}[FileColumn::Created], "missing columns got invalid defaults");

  FileColumnLayout layout;
  ok &= expect(layout.displayed() == std::vector<FileColumn>(kDefaultFileColumns.begin(), kDefaultFileColumns.end()),
               "default visibility differs from the existing list");
  layout.visible[columnIndex(FileColumn::Size)] = false;
  layout.visible[columnIndex(FileColumn::Type)] = true;
  ok &= expect(layout.move(FileColumn::Name, 2), "Name did not move");
  ok &= expect(layout.displayed() == std::vector{FileColumn::Modified, FileColumn::Type, FileColumn::Name},
               "visible order did not move around the hidden slot");
  FileColumnLayout restored;
  ok &= expect(parseFileColumnLayout(serializeFileColumnLayout(layout), restored), "column layout did not round-trip");
  ok &= expect(restored.visible == layout.visible && restored.order == layout.order, "saved layout order or visibility changed");
  ok &= expect(!parseFileColumnLayout("-Name,Size,Modified,Type,Created,Accessed", restored), "Name could be hidden");
  ok &= expect(!parseFileColumnLayout("Name,Size,Size,Type,Created,Accessed", restored), "duplicate column accepted");
  ok &= expect(!parseFileColumnLayout("Name,Size,Modified", restored), "incomplete layout accepted");
  restored.visible[columnIndex(FileColumn::Size)] = true;
  const auto shown = restored.displayed();
  const auto wide = fitFileColumns(preferred, 1000.0F, shown);
  ok &= expect(wide[FileColumn::Size] == preferred[FileColumn::Size], "reshown Size lost its saved width");
  ok &= expect(wide[FileColumn::Name] == preferred[FileColumn::Name], "reordered Name lost its saved width");
  const auto compact = fitFileColumns(preferred, 450.0F, shown);
  ok &= expect(std::abs(fileColumnTotal(compact, shown) - 450.0F) < 0.01F, "optional columns did not fit the viewport");
  const auto start = fitFileColumns({}, 900.0F, shown);
  const auto newType = resizeFileColumnBoundary(start, FileColumn::Type, 30.0F, 900.0F, shown, true);
  ok &= expect(newType[FileColumn::Type] == start[FileColumn::Type] + 30.0F, "Type boundary did not resize Type");
  ok &= expect(newType[FileColumn::Size] == start[FileColumn::Size], "Type resize changed Size");
  return ok ? 0 : 1;
}
