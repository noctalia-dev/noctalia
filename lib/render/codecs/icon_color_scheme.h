#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct Palette;

namespace codecs {

  // Icon themes whose index.theme sets FollowsColorScheme, as KDE apps draw them: an SVG file under one of `roots`
  // that has a `<style id="current-color-scheme">` takes its ColorScheme-* classes from `colors`
  // (iconColorSchemeStylesheet()). No roots turns it off. Applies to later loads; callers reload the icons they show.
  void setIconColorScheme(std::vector<std::string> roots, const Palette& colors);

  // The stylesheet the SVG file at `path` with contents `data` renders with, or null when it is not a color-scheme
  // icon of a following theme. Thread safe.
  std::shared_ptr<const std::string>
  iconColorSchemeStylesheetFor(const std::string& path, std::span<const std::uint8_t> data);

} // namespace codecs
