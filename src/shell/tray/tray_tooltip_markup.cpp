#include "shell/tray/tray_tooltip_markup.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tray {

  namespace {

    constexpr std::size_t kMaxTagDepth = 16U;
    constexpr std::size_t kMaxTagNameLength = 64U;
    constexpr std::size_t kMaxEntityLength = 12U;
    constexpr int kMaxConsecutiveNewlines = 2;

    enum class PangoTag : std::uint8_t {
      Bold,
      Italic,
      Underline,
      Strikethrough,
      Monospace,
      Superscript,
      Subscript,
      Small,
      Big,
    };

    struct ParsedTag {
      std::string name;
      std::optional<std::string> altText;
      bool closing = false;
      bool selfClosing = false;
      std::size_t endPos = 0;
    };

    // Tracks forward-scan EOF misses so repeated unclosed delimiters never rescan quadratically.
    struct ScanState {
      bool noMoreGt = false;
      bool noMoreCommentEnd = false;
      bool noMorePiEnd = false;
      bool noMoreSingleQuote = false;
      bool noMoreDoubleQuote = false;
    };

    std::string_view openTagText(PangoTag tag) {
      switch (tag) {
      case PangoTag::Bold:
        return "<b>";
      case PangoTag::Italic:
        return "<i>";
      case PangoTag::Underline:
        return "<u>";
      case PangoTag::Strikethrough:
        return "<s>";
      case PangoTag::Monospace:
        return "<tt>";
      case PangoTag::Superscript:
        return "<sup>";
      case PangoTag::Subscript:
        return "<sub>";
      case PangoTag::Small:
        return "<small>";
      case PangoTag::Big:
        return "<big>";
      }
      return {};
    }

    std::string_view closeTagText(PangoTag tag) {
      switch (tag) {
      case PangoTag::Bold:
        return "</b>";
      case PangoTag::Italic:
        return "</i>";
      case PangoTag::Underline:
        return "</u>";
      case PangoTag::Strikethrough:
        return "</s>";
      case PangoTag::Monospace:
        return "</tt>";
      case PangoTag::Superscript:
        return "</sup>";
      case PangoTag::Subscript:
        return "</sub>";
      case PangoTag::Small:
        return "</small>";
      case PangoTag::Big:
        return "</big>";
      }
      return {};
    }

    std::optional<PangoTag> mapFormattingTag(std::string_view name) {
      if (name == "b"
          || name == "strong"
          || name == "th"
          || name == "h1"
          || name == "h2"
          || name == "h3"
          || name == "h4"
          || name == "h5"
          || name == "h6") {
        return PangoTag::Bold;
      }
      if (name == "i" || name == "em") {
        return PangoTag::Italic;
      }
      if (name == "u" || name == "ins" || name == "a") {
        return PangoTag::Underline;
      }
      if (name == "s" || name == "del" || name == "strike") {
        return PangoTag::Strikethrough;
      }
      if (name == "code" || name == "tt" || name == "kbd" || name == "samp") {
        return PangoTag::Monospace;
      }
      if (name == "sup") {
        return PangoTag::Superscript;
      }
      if (name == "sub") {
        return PangoTag::Subscript;
      }
      if (name == "small") {
        return PangoTag::Small;
      }
      if (name == "big") {
        return PangoTag::Big;
      }
      return std::nullopt;
    }

    bool isAsciiSpace(char ch) {
      return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
    }

    bool isBlockTag(std::string_view name) {
      return name == "p"
          || name == "div"
          || name == "ul"
          || name == "ol"
          || name == "blockquote"
          || name == "pre"
          || name == "hr"
          || name == "h1"
          || name == "h2"
          || name == "h3"
          || name == "h4"
          || name == "h5"
          || name == "h6";
    }

    void appendPangoEscaped(std::string& dst, std::string_view text) {
      for (char ch : text) {
        switch (ch) {
        case '&':
          dst += "&amp;";
          break;
        case '<':
          dst += "&lt;";
          break;
        case '>':
          dst += "&gt;";
          break;
        default:
          dst += ch;
          break;
        }
      }
    }

    void appendUtf8CodePoint(std::string& dst, std::uint32_t cp) {
      if (cp < 0x80U) {
        dst.push_back(static_cast<char>(cp));
      } else if (cp < 0x800U) {
        dst.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        dst.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
      } else if (cp < 0x10000U) {
        dst.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        dst.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        dst.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
      } else if (cp <= 0x10FFFFU) {
        dst.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        dst.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        dst.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        dst.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
      }
    }

    bool tryDecodeEntity(std::string_view rest, std::string& decoded, std::size_t& consumed) {
      const std::size_t limit = std::min(rest.size(), kMaxEntityLength);
      const std::size_t semi = rest.substr(0, limit).find(';');
      if (semi == std::string_view::npos || semi < 2U) {
        return false;
      }

      const std::string_view entity = rest.substr(1, semi - 1);
      if (entity.front() == '#') {
        std::uint32_t cp = 0;
        const char* begin = nullptr;
        const char* end = entity.data() + entity.size();
        int base = 10;
        if (entity.size() >= 3U && (entity[1] == 'x' || entity[1] == 'X')) {
          begin = entity.data() + 2;
          base = 16;
        } else if (entity.size() >= 2U) {
          begin = entity.data() + 1;
        } else {
          return false;
        }
        const auto [ptr, ec] = std::from_chars(begin, end, cp, base);
        if (ec != std::errc{}
            || ptr != end
            || cp == 0U
            || cp > 0x10FFFFU
            || (cp >= 0xD800U && cp <= 0xDFFFU)
            || (cp < 0x20U && cp != '\t' && cp != '\n' && cp != '\r')) {
          return false;
        }
        decoded.clear();
        appendUtf8CodePoint(decoded, cp);
        consumed = semi + 1;
        return true;
      }

      const std::string lowerEntity = StringUtils::toLower(entity);
      std::string_view replacement;
      if (lowerEntity == "amp") {
        replacement = "&";
      } else if (lowerEntity == "lt") {
        replacement = "<";
      } else if (lowerEntity == "gt") {
        replacement = ">";
      } else if (lowerEntity == "quot") {
        replacement = "\"";
      } else if (lowerEntity == "apos") {
        replacement = "'";
      } else if (lowerEntity == "nbsp") {
        replacement = "\xC2\xA0";
      } else if (lowerEntity == "ndash") {
        replacement = "\xE2\x80\x93";
      } else if (lowerEntity == "mdash") {
        replacement = "\xE2\x80\x94";
      } else if (lowerEntity == "hellip") {
        replacement = "\xE2\x80\xA6";
      } else if (lowerEntity == "bull") {
        replacement = "\xE2\x80\xA2";
      } else if (lowerEntity == "copy") {
        replacement = "\xC2\xA9";
      } else if (lowerEntity == "times") {
        replacement = "\xC3\x97";
      } else if (lowerEntity == "deg") {
        replacement = "\xC2\xB0";
      } else if (lowerEntity == "lsquo") {
        replacement = "\xE2\x80\x98";
      } else if (lowerEntity == "rsquo") {
        replacement = "\xE2\x80\x99";
      } else if (lowerEntity == "ldquo") {
        replacement = "\xE2\x80\x9C";
      } else if (lowerEntity == "rdquo") {
        replacement = "\xE2\x80\x9D";
      } else {
        return false;
      }

      decoded.assign(replacement);
      consumed = semi + 1;
      return true;
    }

    std::string decodeEntities(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      std::string decoded;
      std::size_t i = 0;
      while (i < text.size()) {
        if (text[i] == '&') {
          std::size_t consumed = 0;
          if (tryDecodeEntity(text.substr(i), decoded, consumed)) {
            out += decoded;
            i += consumed;
            continue;
          }
        }
        out.push_back(text[i]);
        ++i;
      }
      return out;
    }

    std::optional<ParsedTag> tryParseTag(std::string_view input, std::size_t pos, ScanState& scan) {
      if (scan.noMoreGt) {
        return std::nullopt;
      }

      std::size_t j = pos + 1;
      if (j >= input.size()) {
        return std::nullopt;
      }

      bool closing = false;
      if (input[j] == '/') {
        closing = true;
        ++j;
      }
      if (j >= input.size() || std::isalpha(static_cast<unsigned char>(input[j])) == 0) {
        return std::nullopt;
      }

      const std::size_t nameStart = j;
      while (j < input.size()) {
        const auto ch = static_cast<unsigned char>(input[j]);
        if (std::isalnum(ch) != 0 || ch == '-' || ch == ':') {
          ++j;
        } else {
          break;
        }
      }
      const std::size_t nameLen = j - nameStart;
      if (nameLen == 0 || nameLen > kMaxTagNameLength) {
        return std::nullopt;
      }
      std::string name = StringUtils::toLower(input.substr(nameStart, nameLen));

      if (closing) {
        while (j < input.size() && isAsciiSpace(input[j])) {
          ++j;
        }
        if (j >= input.size()) {
          scan.noMoreGt = true;
          return std::nullopt;
        }
        if (input[j] == '>') {
          return ParsedTag{.name = std::move(name), .closing = true, .selfClosing = false, .endPos = j + 1};
        }
        return std::nullopt;
      }

      const bool captureAlt = name == "img";
      std::optional<std::string> altText;

      while (j < input.size()) {
        if (input[j] == '>') {
          return ParsedTag{
              .name = std::move(name),
              .altText = std::move(altText),
              .closing = false,
              .selfClosing = false,
              .endPos = j + 1,
          };
        }
        if (input[j] == '/') {
          if (j + 1 < input.size() && input[j + 1] == '>') {
            return ParsedTag{
                .name = std::move(name),
                .altText = std::move(altText),
                .closing = false,
                .selfClosing = true,
                .endPos = j + 2,
            };
          }
          return std::nullopt;
        }
        if (!isAsciiSpace(input[j])) {
          return std::nullopt;
        }
        while (j < input.size() && isAsciiSpace(input[j])) {
          ++j;
        }
        if (j >= input.size()) {
          scan.noMoreGt = true;
          return std::nullopt;
        }
        if (input[j] == '>') {
          return ParsedTag{
              .name = std::move(name),
              .altText = std::move(altText),
              .closing = false,
              .selfClosing = false,
              .endPos = j + 1,
          };
        }
        if (input[j] == '/') {
          if (j + 1 < input.size() && input[j + 1] == '>') {
            return ParsedTag{
                .name = std::move(name),
                .altText = std::move(altText),
                .closing = false,
                .selfClosing = true,
                .endPos = j + 2,
            };
          }
          return std::nullopt;
        }

        const std::size_t attrNameStart = j;
        const auto attrLead = static_cast<unsigned char>(input[j]);
        if (std::isalpha(attrLead) == 0 && attrLead != '_' && attrLead != ':') {
          return std::nullopt;
        }
        ++j;
        while (j < input.size()) {
          const auto ch = static_cast<unsigned char>(input[j]);
          if (std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == ':' || ch == '-') {
            ++j;
          } else {
            break;
          }
        }
        const std::string_view attrName = input.substr(attrNameStart, j - attrNameStart);
        while (j < input.size() && isAsciiSpace(input[j])) {
          ++j;
        }
        if (j < input.size() && input[j] == '=') {
          ++j;
          while (j < input.size() && isAsciiSpace(input[j])) {
            ++j;
          }
          if (j >= input.size()) {
            scan.noMoreGt = true;
            return std::nullopt;
          }
          std::string_view attrValue;
          if (input[j] == '"' || input[j] == '\'') {
            const char quote = input[j++];
            bool& noMoreQuote = quote == '"' ? scan.noMoreDoubleQuote : scan.noMoreSingleQuote;
            if (noMoreQuote) {
              return std::nullopt;
            }
            const std::size_t valStart = j;
            const std::size_t closeQuote = input.find(quote, j);
            if (closeQuote == std::string_view::npos) {
              noMoreQuote = true;
              return std::nullopt;
            }
            attrValue = input.substr(valStart, closeQuote - valStart);
            j = closeQuote + 1;
          } else {
            const std::size_t valStart = j;
            while (j < input.size() && !isAsciiSpace(input[j])) {
              const char c = input[j];
              if (c == '>' || (c == '/' && j + 1 < input.size() && input[j + 1] == '>')) {
                break;
              }
              if (c == '<' || c == '"' || c == '\'' || c == '=' || c == '`') {
                return std::nullopt;
              }
              ++j;
            }
            if (j == valStart) {
              return std::nullopt;
            }
            attrValue = input.substr(valStart, j - valStart);
          }

          if (captureAlt && !altText.has_value() && StringUtils::toLower(attrName) == "alt") {
            altText = decodeEntities(attrValue);
          }
        }
      }
      scan.noMoreGt = true;
      return std::nullopt;
    }

    std::optional<std::size_t>
    findClosingTagEnd(std::string_view input, std::size_t from, std::string_view tagName, ScanState& scan) {
      std::size_t pos = from;
      while (pos < input.size()) {
        const std::size_t lt = input.find('<', pos);
        if (lt == std::string_view::npos) {
          return std::nullopt;
        }
        if (const auto parsed = tryParseTag(input, lt, scan);
            parsed.has_value() && parsed->closing && parsed->name == tagName) {
          return parsed->endPos;
        }
        pos = lt + 1;
      }
      return std::nullopt;
    }

    std::optional<std::size_t> trySkipCommentOrDeclaration(std::string_view input, std::size_t pos, ScanState& scan) {
      const std::string_view rest = input.substr(pos);
      if (rest.starts_with("<!--")) {
        if (scan.noMoreCommentEnd) {
          return std::nullopt;
        }
        if (const std::size_t endComment = input.find("-->", pos + 4); endComment != std::string_view::npos) {
          return endComment + 3;
        }
        scan.noMoreCommentEnd = true;
        return std::nullopt;
      }
      if (rest.size() >= 3 && rest[1] == '!' && std::isalpha(static_cast<unsigned char>(rest[2])) != 0) {
        if (scan.noMoreGt) {
          return std::nullopt;
        }
        std::size_t j = pos + 2;
        while (j < input.size()) {
          if (input[j] == '"' || input[j] == '\'') {
            const char quote = input[j++];
            bool& noMoreQuote = quote == '"' ? scan.noMoreDoubleQuote : scan.noMoreSingleQuote;
            if (noMoreQuote) {
              return std::nullopt;
            }
            const std::size_t endQuote = input.find(quote, j);
            if (endQuote == std::string_view::npos) {
              noMoreQuote = true;
              return std::nullopt;
            }
            j = endQuote + 1;
            continue;
          }
          if (input[j] == '<') {
            return std::nullopt;
          }
          if (input[j] == '>') {
            return j + 1;
          }
          ++j;
        }
        scan.noMoreGt = true;
        return std::nullopt;
      }
      if (rest.size() >= 3 && rest[1] == '?') {
        if (scan.noMorePiEnd) {
          return std::nullopt;
        }
        for (std::size_t j = pos + 2; j < input.size(); ++j) {
          if (input[j] == '<') {
            return std::nullopt;
          }
          if (input[j] == '?' && j + 1 < input.size() && input[j + 1] == '>') {
            return j + 2;
          }
        }
        scan.noMorePiEnd = true;
      }
      return std::nullopt;
    }

    class HtmlMarkupBuilder {
    public:
      explicit HtmlMarkupBuilder(std::size_t reserveBytes) {
        m_out.markup.reserve(reserveBytes + 16U);
        m_out.plainText.reserve(reserveBytes);
      }

      void pushTag(PangoTag tag) {
        if (m_activeTags.size() < kMaxTagDepth) {
          m_activeTags.push_back(tag);
        }
      }

      void popTag(PangoTag tag) {
        for (auto it = m_activeTags.rbegin(); it != m_activeTags.rend(); ++it) {
          if (*it == tag) {
            m_activeTags.erase(std::next(it).base());
            break;
          }
        }
      }

      void triggerBlockBreak() {
        if (m_hasVisibleText) {
          m_pendingWs.clear();
          m_pendingNewlines = std::max(m_pendingNewlines, 1);
          m_fromBlockTag = true;
        }
      }

      void addExplicitBreak() {
        if (m_hasVisibleText) {
          m_pendingWs.clear();
          m_pendingNewlines = std::min(m_pendingNewlines + 1, kMaxConsecutiveNewlines);
          m_fromBlockTag = true;
        }
      }

      void openListItem() {
        triggerBlockBreak();
        m_pendingBullet = true;
        m_bulletActive = effectiveActive();
      }

      void closeListItem() {
        m_pendingBullet = false;
        triggerBlockBreak();
      }

      void clearPendingBullet() { m_pendingBullet = false; }

      void setTableRow(bool inRow) {
        triggerBlockBreak();
        m_inTableRow = inRow;
        m_rowHasCell = false;
      }

      void openTableCell(bool header, bool selfClosing) {
        if (m_inTableRow && m_rowHasCell && m_hasVisibleText && m_pendingNewlines == 0 && m_pendingWs.empty()) {
          m_pendingWs = " ";
          m_wsActive = effectiveActive();
        }
        if (header && !selfClosing) {
          pushTag(PangoTag::Bold);
        }
      }

      void closeTableCell(bool header) {
        if (header) {
          popTag(PangoTag::Bold);
        }
        if (m_inTableRow && m_hasVisibleText) {
          m_rowHasCell = true;
        }
      }

      void appendWhitespace(char ch) {
        if (!m_hasVisibleText) {
          return;
        }
        if (ch == '\n' || ch == '\r') {
          if (!m_fromBlockTag) {
            m_pendingWs.clear();
            m_pendingNewlines = std::min(m_pendingNewlines + 1, kMaxConsecutiveNewlines);
          }
        } else if (!m_fromBlockTag) {
          if (m_pendingWs.empty()) {
            m_wsActive = effectiveActive();
          }
          m_pendingWs.push_back(ch);
        }
      }

      void emitVisible(std::string_view chunk) {
        const auto desired = effectiveActive();
        if (m_pendingNewlines > 0) {
          std::size_t lcp = 0;
          while (lcp < m_emittedTags.size() && lcp < desired.size() && m_emittedTags[lcp] == desired[lcp]) {
            ++lcp;
          }
          while (m_emittedTags.size() > lcp) {
            m_out.markup += closeTagText(m_emittedTags.back());
            m_emittedTags.pop_back();
          }
          m_out.plainText.append(static_cast<std::size_t>(m_pendingNewlines), '\n');
          m_out.markup.append(static_cast<std::size_t>(m_pendingNewlines), '\n');
          m_pendingNewlines = 0;
          m_fromBlockTag = false;
        }
        if (!m_pendingWs.empty()) {
          syncEmittedTo(m_wsActive);
          m_out.plainText += m_pendingWs;
          appendPangoEscaped(m_out.markup, m_pendingWs);
          m_pendingWs.clear();
        }
        if (m_pendingBullet) {
          syncEmittedTo(m_bulletActive);
          m_out.plainText += "\xE2\x80\xA2 ";
          m_out.markup += "\xE2\x80\xA2 ";
          m_pendingBullet = false;
        }
        syncEmittedTo(desired);
        m_out.plainText += chunk;
        appendPangoEscaped(m_out.markup, chunk);
        m_hasVisibleText = true;
      }

      HtmlMarkupResult finish() {
        while (!m_emittedTags.empty()) {
          m_out.markup += closeTagText(m_emittedTags.back());
          m_emittedTags.pop_back();
        }
        return std::move(m_out);
      }

    private:
      std::vector<PangoTag> effectiveActive() const {
        std::vector<PangoTag> dedup;
        dedup.reserve(m_activeTags.size());
        for (const PangoTag tag : m_activeTags) {
          if (!std::ranges::contains(dedup, tag)) {
            dedup.push_back(tag);
          }
        }
        return dedup;
      }

      void syncEmittedTo(const std::vector<PangoTag>& target) {
        std::size_t lcp = 0;
        while (lcp < m_emittedTags.size() && lcp < target.size() && m_emittedTags[lcp] == target[lcp]) {
          ++lcp;
        }
        while (m_emittedTags.size() > lcp) {
          m_out.markup += closeTagText(m_emittedTags.back());
          m_emittedTags.pop_back();
        }
        while (m_emittedTags.size() < target.size()) {
          const PangoTag tag = target[m_emittedTags.size()];
          m_out.markup += openTagText(tag);
          m_emittedTags.push_back(tag);
          m_out.hasFormatting = true;
        }
      }

      HtmlMarkupResult m_out;
      std::vector<PangoTag> m_activeTags;
      std::vector<PangoTag> m_emittedTags;
      std::string m_pendingWs;
      std::vector<PangoTag> m_wsActive;
      std::vector<PangoTag> m_bulletActive;
      int m_pendingNewlines = 0;
      bool m_hasVisibleText = false;
      bool m_fromBlockTag = false;
      bool m_pendingBullet = false;
      bool m_inTableRow = false;
      bool m_rowHasCell = false;
    };

  } // namespace

  std::string escapePangoMarkup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    appendPangoEscaped(out, text);
    return out;
  }

  HtmlMarkupResult htmlToPangoMarkup(std::string_view html) {
    HtmlMarkupBuilder builder(html.size());
    ScanState scan;
    std::string decodedEntity;

    std::size_t i = 0;
    while (i < html.size()) {
      if (html[i] == '<') {
        if (const auto skipped = trySkipCommentOrDeclaration(html, i, scan); skipped.has_value()) {
          i = *skipped;
          continue;
        }

        if (const auto tag = tryParseTag(html, i, scan); tag.has_value()) {
          i = tag->endPos;

          if (!tag->closing
              && !tag->selfClosing
              && (tag->name == "style" || tag->name == "script" || tag->name == "head")) {
            i = findClosingTagEnd(html, i, tag->name, scan).value_or(html.size());
            continue;
          }

          if (tag->name == "img") {
            if (!tag->closing && tag->altText.has_value()) {
              const std::string_view trimmedAlt = StringUtils::trimRightView(StringUtils::trimLeftView(*tag->altText));
              if (!trimmedAlt.empty()) {
                builder.emitVisible(trimmedAlt);
              }
            }
            continue;
          }

          if (tag->name == "br") {
            if (!tag->closing) {
              builder.addExplicitBreak();
            }
            continue;
          }

          if (tag->name == "li") {
            if (!tag->closing) {
              builder.openListItem();
            } else {
              builder.closeListItem();
            }
            continue;
          }

          if (tag->name == "tr") {
            builder.setTableRow(!tag->closing);
            continue;
          }

          if (tag->name == "td" || tag->name == "th") {
            const bool isHeader = tag->name == "th";
            if (!tag->closing) {
              builder.openTableCell(isHeader, tag->selfClosing);
            } else {
              builder.closeTableCell(isHeader);
            }
            continue;
          }

          if (tag->name == "ul" || tag->name == "ol") {
            if (tag->closing) {
              builder.clearPendingBullet();
            }
            builder.triggerBlockBreak();
            continue;
          }

          if (const auto pangoTag = mapFormattingTag(tag->name); pangoTag.has_value()) {
            const bool block = isBlockTag(tag->name);
            if (!tag->closing) {
              if (block) {
                builder.triggerBlockBreak();
              }
              if (!tag->selfClosing) {
                builder.pushTag(*pangoTag);
              }
            } else {
              builder.popTag(*pangoTag);
              if (block) {
                builder.triggerBlockBreak();
              }
            }
            continue;
          }

          if (isBlockTag(tag->name)) {
            builder.triggerBlockBreak();
          }
          continue;
        }
      }

      if (html[i] == '&') {
        std::size_t consumed = 0;
        if (tryDecodeEntity(html.substr(i), decodedEntity, consumed)) {
          i += consumed;
          if (decodedEntity.size() == 1U && isAsciiSpace(decodedEntity[0])) {
            builder.appendWhitespace(decodedEntity[0]);
          } else {
            builder.emitVisible(decodedEntity);
          }
          continue;
        }
      }

      const char ch = html[i];
      if (isAsciiSpace(ch)) {
        if (ch == '\r' && i + 1 < html.size() && html[i + 1] == '\n') {
          i += 2;
        } else {
          ++i;
        }
        builder.appendWhitespace(ch);
        continue;
      }

      std::size_t runEnd = i + 1;
      while (runEnd < html.size() && html[runEnd] != '<' && html[runEnd] != '&' && !isAsciiSpace(html[runEnd])) {
        ++runEnd;
      }
      builder.emitVisible(html.substr(i, runEnd - i));
      i = runEnd;
    }

    return builder.finish();
  }

} // namespace tray
