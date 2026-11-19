#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  enum class FileColumn { Name, Size, Modified, Type, Created, Accessed };

  inline constexpr std::array kFileColumns{
      FileColumn::Name, FileColumn::Size, FileColumn::Modified, FileColumn::Type, FileColumn::Created, FileColumn::Accessed
  };
  inline constexpr std::array kDefaultFileColumns{FileColumn::Name, FileColumn::Size, FileColumn::Modified};
  inline constexpr std::array<std::string_view, kFileColumns.size()> kFileColumnLabels{
      "Name", "Size", "Modified", "Type", "Created", "Accessed"
  };
  inline constexpr float fileColumnTextInset(bool trailing, float gap) noexcept {
    return trailing ? 12.0F : 12.0F - gap * 0.5F;
  }

  inline constexpr std::size_t columnIndex(FileColumn column) noexcept { return static_cast<std::size_t>(column); }

  // Logical pixels, keyed by identity so moving or hiding a column retains its width.
  struct FileColumns {
    std::array<float, kFileColumns.size()> widths{0.0F, 96.0F, 152.0F, 144.0F, 152.0F, 152.0F};

    float& operator[](FileColumn column) noexcept { return widths[columnIndex(column)]; }
    float operator[](FileColumn column) const noexcept { return widths[columnIndex(column)]; }
  };

  inline constexpr FileColumns kMinimumFileColumns{{120.0F, 72.0F, 112.0F, 88.0F, 112.0F, 112.0F}};

  struct FileColumnLayout {
    std::array<FileColumn, kFileColumns.size()> order = kFileColumns;
    std::array<bool, kFileColumns.size()> visible{true, true, true, false, false, false};

    [[nodiscard]] std::vector<FileColumn> displayed() const {
      std::vector<FileColumn> columns;
      for (auto column : order)
        if (visible[columnIndex(column)])
          columns.push_back(column);
      return columns;
    }

    // Move through the visible sequence, leaving hidden slots in place.
    bool move(FileColumn column, std::size_t destination) {
      auto columns = displayed();
      const auto from = std::ranges::find(columns, column);
      if (from == columns.end() || destination >= columns.size() || from == columns.begin() + destination)
        return false;
      columns.erase(from);
      columns.insert(columns.begin() + destination, column);
      auto next = columns.begin();
      for (auto& slot : order)
        if (visible[columnIndex(slot)])
          slot = *next++;
      return true;
    }
  };

  inline float fileColumnTotal(FileColumns widths, std::span<const FileColumn> visible) {
    float total = 0.0F;
    for (auto column : visible)
      total += widths[column];
    return total;
  }

  inline bool hasOptionalFileColumns(std::span<const FileColumn> visible) {
    return std::ranges::any_of(visible, [](auto column) { return column >= FileColumn::Type; });
  }

  inline FileColumns fitFileColumns(
      FileColumns widths, float available, std::span<const FileColumn> visible = kDefaultFileColumns
  ) {
    available = std::max(0.0F, available);
    const bool flexibleName = widths[FileColumn::Name] <= 0.0F;
    for (auto column : visible)
      widths[column] = std::max(kMinimumFileColumns[column], widths[column]);
    const float minimum = fileColumnTotal(kMinimumFileColumns, visible);
    const float wanted = fileColumnTotal(widths, visible);
    if (available < minimum) {
      for (auto column : visible)
        widths[column] = kMinimumFileColumns[column] * available / minimum;
    } else if (hasOptionalFileColumns(visible) && wanted > available) {
      // Keep newly shown columns in view without changing any saved widths.
      const float ratio = (available - minimum) / (wanted - minimum);
      for (auto column : visible)
        widths[column] = kMinimumFileColumns[column] + (widths[column] - kMinimumFileColumns[column]) * ratio;
    } else if (flexibleName && wanted < available) {
      widths[FileColumn::Name] += available - wanted;
    }
    return widths;
  }

  inline FileColumns resizeFileColumnBoundary(
      FileColumns widths, FileColumn column, float delta, float available,
      std::span<const FileColumn> visible = kDefaultFileColumns, bool flexibleName = false
  ) {
    widths = fitFileColumns(widths, available, visible);
    if (available < fileColumnTotal(kMinimumFileColumns, visible) || !std::isfinite(delta))
      return widths;
    const float original = widths[column];
    widths[column] = std::clamp(original + delta, kMinimumFileColumns[column], 10000.0F);
    if (hasOptionalFileColumns(visible)) {
      const auto name = FileColumn::Name;
      if (flexibleName && column != name)
        widths[name] = kMinimumFileColumns[name];
      const float excess = fileColumnTotal(widths, visible) - available;
      if (excess > 0.0F)
        widths[column] = std::max(kMinimumFileColumns[column], widths[column] - excess);
      if (flexibleName && column != name)
        widths[name] = 0.0F;
      widths = fitFileColumns(widths, available, visible);
    }
    return widths;
  }

  inline std::string serializeFileColumns(const FileColumns& widths) {
    std::string text;
    for (float width : widths.widths) {
      if (!text.empty())
        text += ',';
      text += std::to_string(width);
    }
    return text;
  }

  inline bool parseFileColumns(std::string_view text, FileColumns& widths) {
    FileColumns parsed;
    std::size_t count = 0;
    while (count < parsed.widths.size()) {
      const auto comma = text.find(',');
      const auto token = text.substr(0, comma);
      if (token.empty())
        return false;
      const auto index = count++;
      auto& value = parsed.widths[index];
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value)
          || value < 0.0F || (value == 0.0F && index != 0) || value > 10000.0F)
        return false;
      if (comma == std::string_view::npos) {
        if (count != kDefaultFileColumns.size() && count != parsed.widths.size())
          return false;
        widths = parsed;
        return true;
      }
      text.remove_prefix(comma + 1);
    }
    return false;
  }

  inline std::string serializeFileColumnLayout(const FileColumnLayout& layout) {
    std::string text;
    for (auto column : layout.order) {
      if (!text.empty())
        text += ',';
      if (!layout.visible[columnIndex(column)])
        text += '-';
      text += kFileColumnLabels[columnIndex(column)];
    }
    return text;
  }

  inline bool parseFileColumnLayout(std::string_view text, FileColumnLayout& layout) {
    FileColumnLayout parsed;
    std::array<bool, kFileColumns.size()> seen{};
    for (std::size_t i = 0; i < kFileColumns.size(); ++i) {
      const auto comma = text.find(',');
      auto token = text.substr(0, comma);
      const bool visible = !token.starts_with('-');
      if (!visible)
        token.remove_prefix(1);
      const auto found = std::ranges::find(kFileColumnLabels, token);
      if (found == kFileColumnLabels.end() || (comma == std::string_view::npos) != (i + 1 == kFileColumns.size()))
        return false;
      const auto index = static_cast<std::size_t>(found - kFileColumnLabels.begin());
      if (seen[index] || (!visible && kFileColumns[index] == FileColumn::Name))
        return false;
      seen[index] = true;
      parsed.order[i] = kFileColumns[index];
      parsed.visible[index] = visible;
      if (comma != std::string_view::npos)
        text.remove_prefix(comma + 1);
    }
    layout = parsed;
    return true;
  }

} // namespace filemanager
