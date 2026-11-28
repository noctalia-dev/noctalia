#pragma once

#include <string>
#include <string_view>

namespace tray {

  struct HtmlMarkupResult {
    std::string markup;
    std::string plainText;
    bool hasFormatting = false;
  };

  std::string escapePangoMarkup(std::string_view text);

  // Convert HTML (such as StatusNotifierItem ToolTip markup) into balanced Pango
  // markup. Common formatting and layout tags are converted, hidden blocks
  // (head/style/script) and comments are dropped, and uncommon tags are stripped
  // while keeping their inner text.
  HtmlMarkupResult htmlToPangoMarkup(std::string_view html);

} // namespace tray
