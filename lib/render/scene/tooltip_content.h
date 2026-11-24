#pragma once

#include "render/core/renderer.h"

#include <string>
#include <variant>
#include <vector>

struct TooltipRow {
  std::string key;
  std::string value;
  TextEllipsize valueEllipsize = TextEllipsize::End;
};

enum class TooltipPlacement : std::uint8_t {
  Default,
  Above,
  Below,
  Left,
  Right,
};

struct TooltipAnchorInsets {
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
  float left = 0.0F;
};

// Pango-markup text rendered with Label::setUseMarkup(true).
// Callers must pass validated, Pango-escaped markup.
struct TooltipMarkup {
  std::string markup;

  bool operator==(const TooltipMarkup&) const = default;
};

using TooltipContent = std::variant<std::monostate, std::string, TooltipMarkup, std::vector<TooltipRow>>;
