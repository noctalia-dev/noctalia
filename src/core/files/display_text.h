#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace filemanager {

  // Escape control bytes, line/paragraph separators, and Unicode bidirectional
  // control characters for display-only UI labels so filesystem-provided names
  // cannot inject fake lines or visually spoof file extensions. Never use this
  // on underlying filesystem paths, editable rename drafts, or clipboard text.
  [[nodiscard]] inline std::string sanitizeFilenameForDisplay(std::string_view text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    auto appendCodepoint = [&](std::string& out, std::uint32_t cp) {
      out += "<U+";
      out.push_back(kHex[(cp >> 12U) & 0x0FU]);
      out.push_back(kHex[(cp >> 8U) & 0x0FU]);
      out.push_back(kHex[(cp >> 4U) & 0x0FU]);
      out.push_back(kHex[cp & 0x0FU]);
      out.push_back('>');
    };

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
      const auto b0 = static_cast<unsigned char>(text[i]);
      if (b0 == '\n') {
        out += "\\n";
        ++i;
        continue;
      }
      if (b0 == '\r') {
        out += "\\r";
        ++i;
        continue;
      }
      if (b0 == '\t') {
        out += "\\t";
        ++i;
        continue;
      }
      if (b0 < 0x20U || b0 == 0x7FU) {
        out += "\\x";
        out.push_back(kHex[(b0 >> 4U) & 0x0FU]);
        out.push_back(kHex[b0 & 0x0FU]);
        ++i;
        continue;
      }
      // U+061C ARABIC LETTER MARK
      if (b0 == 0xD8U && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x9CU) {
        appendCodepoint(out, 0x061CU);
        i += 2;
        continue;
      }
      if (b0 == 0xE2U && i + 2 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        const auto b2 = static_cast<unsigned char>(text[i + 2]);
        // U+200E..U+200F (LRM/RLM), U+2028..U+2029 (LS/PS), U+202A..U+202E (LRE/RLE/PDF/LRO/RLO)
        if (b1 == 0x80U && ((b2 >= 0xA8U && b2 <= 0xAEU) || b2 == 0x8EU || b2 == 0x8FU)) {
          appendCodepoint(out, 0x2000U | static_cast<std::uint32_t>(b2 - 0x80U));
          i += 3;
          continue;
        }
        // U+2066..U+2069 (LRI/RLI/FSI/PDI)
        if (b1 == 0x81U && b2 >= 0xA6U && b2 <= 0xA9U) {
          appendCodepoint(out, 0x2040U | static_cast<std::uint32_t>(b2 - 0x80U));
          i += 3;
          continue;
        }
      }
      out.push_back(text[i]);
      ++i;
    }
    return out;
  }

} // namespace filemanager
