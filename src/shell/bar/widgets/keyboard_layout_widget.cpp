#include "shell/bar/widgets/keyboard_layout_widget.h"

#include "core/log.h"
#include "core/process.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("keyboard_layout_widget");
  constexpr auto kRefreshTickInterval = std::chrono::milliseconds(40);
  constexpr int kRefreshBurstAttempts = 8;
  constexpr std::string_view kUnknownLabel = "--";
  constexpr std::string_view kVerticalStableLabel = "WWW";

  bool isAsciiAlpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

  bool isWordBoundary(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
      return true;
    }
    return !std::isalnum(static_cast<unsigned char>(text[pos])) && text[pos] != '_';
  }

  bool containsWord(std::string_view haystack, std::string_view needle) {
    if (haystack.empty() || needle.empty()) {
      return false;
    }

    std::size_t pos = haystack.find(needle);
    while (pos != std::string_view::npos) {
      if (isWordBoundary(haystack, pos == 0 ? haystack.size() : pos - 1) &&
          isWordBoundary(haystack, pos + needle.size())) {
        return true;
      }
      pos = haystack.find(needle, pos + 1);
    }
    return false;
  }

  bool extractLeadingCode(std::string_view text, std::string& out) {
    std::size_t count = 0;
    while (count < text.size() && count < 3 && isAsciiAlpha(text[count])) {
      ++count;
    }
    if (count < 2 || count > 3) {
      return false;
    }
    if (count < text.size() && text[count] != '+' && !std::isspace(static_cast<unsigned char>(text[count])) &&
        text[count] != '_' && text[count] != '-') {
      return false;
    }
    out.assign(text.substr(0, count));
    return true;
  }

  bool extractParenthesizedCode(std::string_view text, std::string& out) {
    const std::size_t open = text.find('(');
    const std::size_t close = text.find(')', open == std::string_view::npos ? 0 : open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 2) {
      return false;
    }
    std::string_view inner = text.substr(open + 1, close - open - 1);
    if (inner.size() < 2 || inner.size() > 3) {
      return false;
    }
    if (!std::all_of(inner.begin(), inner.end(), [](char ch) { return isAsciiAlpha(ch); })) {
      return false;
    }
    out.assign(inner);
    return true;
  }

  void uppercaseAscii(std::string& text) {
    for (char& ch : text) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
  }

  const std::vector<std::pair<std::string_view, std::string_view>>& variantMap() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kMap = {
        {"programmer dvorak", "Dvk-P"}, {"colemak", "Colemak"}, {"dvorak", "Dvorak"},
        {"workman", "Workman"},         {"norman", "Norman"},   {"altgr-intl", "Intl"},
        {"international", "Intl"},      {"intl", "Intl"},       {"with dead keys", "Dead"},
        {"phonetic", "Phon"},           {"extended", "Ext"},    {"ergonomic", "Ergo"},
        {"legacy", "Legacy"},           {"pinyin", "Pinyin"},   {"cangjie", "Cangjie"},
        {"romaji", "Romaji"},           {"kana", "Kana"},
    };
    return kMap;
  }

  const std::vector<std::pair<std::string_view, std::string_view>>& languageMap() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kMap = {
        {"english", "us"},
        {"american", "us"},
        {"united states", "us"},
        {"us english", "us"},
        {"british", "gb"},
        {"united kingdom", "gb"},
        {"english (uk)", "gb"},
        {"canadian", "ca"},
        {"canada", "ca"},
        {"canadian english", "ca"},
        {"australian", "au"},
        {"australia", "au"},
        {"swedish", "se"},
        {"svenska", "se"},
        {"sweden", "se"},
        {"norwegian", "no"},
        {"norsk", "no"},
        {"norway", "no"},
        {"danish", "dk"},
        {"dansk", "dk"},
        {"denmark", "dk"},
        {"finnish", "fi"},
        {"suomi", "fi"},
        {"finland", "fi"},
        {"icelandic", "is"},
        {"iceland", "is"},
        {"german", "de"},
        {"deutsch", "de"},
        {"germany", "de"},
        {"austrian", "at"},
        {"austria", "at"},
        {"swiss", "ch"},
        {"switzerland", "ch"},
        {"schweiz", "ch"},
        {"suisse", "ch"},
        {"dutch", "nl"},
        {"nederlands", "nl"},
        {"netherlands", "nl"},
        {"holland", "nl"},
        {"belgian", "be"},
        {"belgium", "be"},
        {"french", "fr"},
        {"francais", "fr"},
        {"france", "fr"},
        {"canadian french", "ca"},
        {"spanish", "es"},
        {"espanol", "es"},
        {"spain", "es"},
        {"castilian", "es"},
        {"italian", "it"},
        {"italiano", "it"},
        {"italy", "it"},
        {"portuguese", "pt"},
        {"portugues", "pt"},
        {"portugal", "pt"},
        {"catalan", "ad"},
        {"andorra", "ad"},
        {"romanian", "ro"},
        {"romania", "ro"},
        {"russian", "ru"},
        {"russia", "ru"},
        {"polish", "pl"},
        {"polski", "pl"},
        {"poland", "pl"},
        {"czech", "cz"},
        {"czech republic", "cz"},
        {"slovak", "sk"},
        {"slovakia", "sk"},
        {"ukraine", "ua"},
        {"ukrainian", "ua"},
        {"bulgarian", "bg"},
        {"bulgaria", "bg"},
        {"serbian", "rs"},
        {"serbia", "rs"},
        {"croatian", "hr"},
        {"croatia", "hr"},
        {"slovenian", "si"},
        {"slovenia", "si"},
        {"bosnian", "ba"},
        {"bosnia", "ba"},
        {"macedonian", "mk"},
        {"macedonia", "mk"},
        {"irish", "ie"},
        {"ireland", "ie"},
        {"welsh", "gb"},
        {"wales", "gb"},
        {"scottish", "gb"},
        {"scotland", "gb"},
        {"estonian", "ee"},
        {"estonia", "ee"},
        {"latvian", "lv"},
        {"latvia", "lv"},
        {"lithuanian", "lt"},
        {"lithuania", "lt"},
        {"hungarian", "hu"},
        {"hungary", "hu"},
        {"greek", "gr"},
        {"greece", "gr"},
        {"albanian", "al"},
        {"albania", "al"},
        {"maltese", "mt"},
        {"malta", "mt"},
        {"turkish", "tr"},
        {"turkey", "tr"},
        {"arabic", "ar"},
        {"arab", "ar"},
        {"hebrew", "il"},
        {"israel", "il"},
        {"brazilian", "br"},
        {"brazilian portuguese", "br"},
        {"brasil", "br"},
        {"brazil", "br"},
        {"japanese", "jp"},
        {"japan", "jp"},
        {"korean", "kr"},
        {"korea", "kr"},
        {"south korea", "kr"},
        {"chinese", "cn"},
        {"china", "cn"},
        {"simplified chinese", "cn"},
        {"traditional chinese", "tw"},
        {"taiwan", "tw"},
        {"thai", "th"},
        {"thailand", "th"},
        {"vietnamese", "vn"},
        {"vietnam", "vn"},
        {"hindi", "in"},
        {"india", "in"},
        {"afrikaans", "za"},
        {"south africa", "za"},
        {"south african", "za"},
    };
    return kMap;
  }

  std::string shortLayoutLabel(const std::string& layoutName) {
    if (layoutName.empty()) {
      return std::string(kUnknownLabel);
    }

    const std::string lower = StringUtils::toLower(layoutName);

    std::string code;
    if (extractLeadingCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    for (const auto& [pattern, display] : variantMap()) {
      if (lower.find(pattern) != std::string::npos) {
        return std::string(display);
      }
    }

    if (extractParenthesizedCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    for (const auto& [lang, mapped] : languageMap()) {
      if (lower.rfind(lang, 0) == 0) {
        code = std::string(mapped);
        uppercaseAscii(code);
        return code;
      }
    }

    for (const auto& [lang, mapped] : languageMap()) {
      if (containsWord(lower, lang)) {
        code = std::string(mapped);
        uppercaseAscii(code);
        return code;
      }
    }

    if (extractLeadingCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    return std::string(kUnknownLabel);
  }

} // namespace

KeyboardLayoutWidget::KeyboardLayoutWidget(
    CompositorPlatform& platform, std::string cycleCommand, DisplayMode displayMode, bool hideLabel
)
    : m_platform(platform), m_cycleCommand(std::move(cycleCommand)), m_displayMode(displayMode),
      m_hideLabel(hideLabel) {}

void KeyboardLayoutWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnLeave([this]() { m_clickArmed = false; });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed) {
      return;
    }
    m_clickArmed = data.button == BTN_LEFT;
  });
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (!m_clickArmed || data.button != BTN_LEFT) {
      return;
    }
    m_clickArmed = false;
    cycleLayout();
  });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "keyboard",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .text = "--",
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
      })
  );

  setRoot(std::move(area));

  // The bar's normal second tick refreshes passive compositor-side layout changes.
  // Click-initiated switches still use the short burst timer below for responsive feedback.
}

void KeyboardLayoutWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_label == nullptr || root() == nullptr) {
    return;
  }

  m_isVertical = containerHeight > containerWidth;
  sync(renderer);

  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    if (m_glyph->width() <= 0.0f) {
      // Some icon fonts may miss the keyboard glyph; use a guaranteed fallback.
      m_glyph->setGlyph("world");
      m_glyph->measure(renderer);
    }
  }

  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setVisible(!m_hideLabel);
  m_label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
  if (!m_hideLabel) {
    const float stableLabelWidth =
        std::round(renderer.measureText(kVerticalStableLabel, m_label->fontSize(), labelFontWeight()).width);
    m_label->setMinWidth(m_isVertical ? std::min(containerWidth, stableLabelWidth) : 0.0f);
    m_label->measure(renderer);
  }

  if (m_isVertical && m_glyph != nullptr) {
    const float w = m_hideLabel ? m_glyph->width() : std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
    if (m_hideLabel) {
      root()->setSize(w, m_glyph->height());
    } else {
      m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
      root()->setSize(w, m_glyph->height() + m_label->height());
    }
  } else {
    const float spacing = Style::spaceXs;
    float x = 0.0f;
    float h = m_hideLabel ? 0.0f : m_label->height();
    if (m_glyph != nullptr) {
      h = std::max(h, m_glyph->height());
      const float glyphY = std::round((h - m_glyph->height()) * 0.5f);
      m_glyph->setPosition(0.0f, glyphY);
      x = m_hideLabel ? m_glyph->width() : (m_glyph->width() + spacing);
    }
    if (m_hideLabel) {
      root()->setSize(x, h);
    } else {
      const float labelY = std::round((h - m_label->height()) * 0.5f);
      m_label->setPosition(x, labelY);
      root()->setSize(m_label->x() + m_label->width(), h);
    }
  }
}

void KeyboardLayoutWidget::doUpdate(Renderer& renderer) { sync(renderer); }

std::string KeyboardLayoutWidget::resolvedLayoutName() const {
  const auto state = m_platform.keyboardLayoutState();
  if (state.has_value() && state->currentIndex >= 0 && state->currentIndex < static_cast<int>(state->names.size())) {
    const std::string actual = state->names[static_cast<std::size_t>(state->currentIndex)];
    if (!m_pendingLayoutName.empty() && (actual.empty() || actual == m_lastLayoutName)) {
      return m_pendingLayoutName;
    }
    return actual;
  }

  std::string layoutName = m_platform.currentKeyboardLayoutName();
  if (!m_pendingLayoutName.empty() && (layoutName.empty() || layoutName == m_lastLayoutName)) {
    return m_pendingLayoutName;
  }
  if (!layoutName.empty()) {
    return layoutName;
  }

  return m_pendingLayoutName;
}

void KeyboardLayoutWidget::sync(Renderer& renderer) {
  if (m_label == nullptr) {
    return;
  }

  std::string layoutName = resolvedLayoutName();
  if (!m_pendingLayoutName.empty() && layoutName == m_pendingLayoutName) {
    m_pendingLayoutName.clear();
    m_refreshAttemptsRemaining = 0;
    m_refreshTimer.stop();
  }
  std::string layoutLabel = formatLayoutLabel(layoutName, m_displayMode);
  if (m_isVertical && layoutLabel.size() > 3) {
    layoutLabel = layoutLabel.substr(0, 3);
  }

  if (layoutName == m_lastLayoutName && layoutLabel == m_lastLabel && m_isVertical == m_lastVertical) {
    return;
  }

  m_lastLayoutName = layoutName;
  m_lastLabel = layoutLabel;
  m_lastVertical = m_isVertical;

  m_label->setVisible(!m_hideLabel);
  if (!m_hideLabel) {
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(layoutLabel);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    if (m_hideLabel) {
      const std::string tooltipText = layoutName.empty() ? layoutLabel : layoutName;
      area->setTooltip(tooltipText);
    } else {
      area->clearTooltip();
    }
  }

  if (auto* node = root(); node != nullptr) {
    node->setOpacity((m_cycleCommand.empty() && !m_platform.hasKeyboardLayoutBackend()) ? 0.85f : 1.0f);
  }

  requestRedraw();
}

void KeyboardLayoutWidget::cycleLayout() {
  const auto stateBefore = m_platform.keyboardLayoutState();

  bool cycled = false;
  if (!m_cycleCommand.empty()) {
    cycled = process::runSync(m_cycleCommand);
    if (!cycled) {
      kLog.warn("keyboard_layout: cycle command failed");
      return;
    }
  } else if (m_platform.hasKeyboardLayoutBackend()) {
    cycled = m_platform.cycleKeyboardLayout();
    if (!cycled) {
      kLog.warn("keyboard_layout: compositor backend failed to cycle layout");
      return;
    }
  } else {
    return;
  }

  if (stateBefore.has_value() && stateBefore->currentIndex >= 0 &&
      stateBefore->currentIndex < static_cast<int>(stateBefore->names.size()) && stateBefore->names.size() > 1) {
    std::size_t nextIndex = static_cast<std::size_t>(stateBefore->currentIndex + 1);
    if (nextIndex >= stateBefore->names.size()) {
      nextIndex = 0;
    }
    m_pendingLayoutName = stateBefore->names[nextIndex];
  }

  scheduleRefreshBurst();
  requestUpdate();
}

void KeyboardLayoutWidget::scheduleRefreshBurst() {
  m_refreshAttemptsRemaining = kRefreshBurstAttempts;
  armRefreshTick();
}

void KeyboardLayoutWidget::armRefreshTick() {
  m_refreshTimer.start(kRefreshTickInterval, [this]() {
    if (m_refreshAttemptsRemaining <= 0) {
      m_pendingLayoutName.clear();
      m_refreshTimer.stop();
      requestUpdate();
      return;
    }

    --m_refreshAttemptsRemaining;
    if (m_refreshAttemptsRemaining > 0) {
      armRefreshTick();
    } else {
      m_pendingLayoutName.clear();
      m_refreshTimer.stop();
    }

    requestUpdate();
  });
}

KeyboardLayoutWidget::DisplayMode KeyboardLayoutWidget::parseDisplayMode(const std::string& value) {
  return value == "full" ? DisplayMode::Full : DisplayMode::Short;
}

std::string KeyboardLayoutWidget::formatLayoutLabel(const std::string& layoutName, DisplayMode displayMode) {
  if (layoutName.empty()) {
    return std::string(kUnknownLabel);
  }

  if (displayMode == DisplayMode::Full) {
    return layoutName;
  }
  return shortLayoutLabel(layoutName);
}
