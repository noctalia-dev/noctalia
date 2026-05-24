#include "ui/controls/input.h"

#include "core/key_modifiers.h"
#include "core/key_symbols.h"
#include "core/text_clipboard.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/glyph_node.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <wayland-client-protocol.h>

namespace {

  TextClipboard* g_clipboard = nullptr;
  Input::PasswordMaskStyle g_passwordMaskStyle = Input::PasswordMaskStyle::CircleFilled;
  std::function<bool(std::uint32_t, std::uint32_t)> g_validateKeyMatcher;

  std::optional<std::string> readClipboardText() {
    return g_clipboard != nullptr ? g_clipboard->clipboardText() : std::nullopt;
  }

  constexpr float kMinWidth = 48.0f;
  constexpr float kCursorWidth = 1.25f;
  constexpr float kCursorPadV = 4.0f;
  constexpr float kCursorMinHeight = 14.0f;
  constexpr float kCursorHeightRatio = 0.50f;
  constexpr float kCursorRevealPadding = 2.0f;
  constexpr float kClearGlyphScale = 0.85f;
  constexpr auto kCursorBlinkInterval = std::chrono::milliseconds(530);
  constexpr auto kCursorBlinkResumeDelay = std::chrono::milliseconds(650);
  constexpr float kTextInnerInset = 3.0f;
  constexpr float kPlaceholderAlpha = 0.68f;
  constexpr float kPrimaryPlaceholderAlpha = 0.50f;
  constexpr float kPasswordGlyphScale = 0.82f;
  constexpr auto kDoubleClickThreshold = std::chrono::milliseconds(400);
  constexpr float kDoubleClickDistance = 6.0f;
  constexpr std::size_t kUndoStackLimit = 100;
  constexpr auto kTypingUndoCoalesceWindow = std::chrono::milliseconds(1000);

  float chromeScaleForControlHeight(float controlHeight) noexcept {
    return std::max(0.1f, controlHeight / Style::controlHeight);
  }

  bool isWordCodepoint(const std::string& text, std::size_t bytePos) {
    if (bytePos >= text.size()) {
      return false;
    }

    const unsigned char lead = static_cast<unsigned char>(text[bytePos]);
    if ((lead & 0x80U) != 0) {
      return true;
    }
    return std::isalnum(lead) != 0 || lead == '_';
  }

  char32_t passwordMaskCodepointForIndex(std::size_t index) {
    if (g_passwordMaskStyle == Input::PasswordMaskStyle::CircleFilled) {
      return GlyphRegistry::lookup("circle-filled");
    }
    static const std::array<char32_t, 7> randomCodepoints = {
        GlyphRegistry::lookup("circle-filled"),        GlyphRegistry::lookup("pentagon-filled"),
        GlyphRegistry::lookup("michelin-star-filled"), GlyphRegistry::lookup("square-rounded-filled"),
        GlyphRegistry::lookup("guitar-pick-filled"),   GlyphRegistry::lookup("blob-filled"),
        GlyphRegistry::lookup("triangle-filled"),
    };
    return randomCodepoints[index % randomCodepoints.size()];
  }

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

Input::Input() {
  // 0: background
  auto bg = std::make_unique<RectNode>();
  m_background = static_cast<RectNode*>(addChild(std::move(bg)));

  // 1: text viewport clip layer
  auto textViewport = std::make_unique<Node>();
  textViewport->setClipChildren(true);
  textViewport->setHitTestVisible(false);
  m_textViewport = addChild(std::move(textViewport));

  // Text-layer children:
  // 0: selection highlight (rendered behind text)
  auto sel = std::make_unique<RectNode>();
  sel->setStyle(
      RoundedRectStyle{
          .fill = resolved(ColorRole::Primary),
          .fillMode = FillMode::Solid,
          .radius = 2.0f,
      }
  );
  sel->setOpacity(0.3f);
  sel->setVisible(false);
  m_selectionRect = static_cast<RectNode*>(m_textViewport->addChild(std::move(sel)));

  // 1: text
  auto label = std::make_unique<Label>();
  label->setFontSize(m_fontSize);
  label->setMaxLines(1);
  label->setColor(colorSpecFromRole(ColorRole::OnSurface));
  m_label = static_cast<Label*>(m_textViewport->addChild(std::move(label)));

  // 2: cursor
  auto cursor = std::make_unique<RectNode>();
  cursor->setStyle(
      RoundedRectStyle{
          .fill = resolved(ColorRole::Primary),
          .fillMode = FillMode::Solid,
          .radius = 1.0f,
      }
  );
  cursor->setVisible(false);
  m_cursor = static_cast<RectNode*>(m_textViewport->addChild(std::move(cursor)));

  // Full-field input area.
  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyVisualState(); });
  area->setOnLeave([this]() { applyVisualState(); });
  area->setOnFocusGain([this]() {
    updateInteractiveGeometry();
    revealCursor();
    startCursorBlink();
    applyVisualState();
  });
  area->setOnFocusLoss([this]() {
    stopCursorBlink();
    updateCursorVisibility();
    applyVisualState();
    if (m_onFocusLoss) {
      m_onFocusLoss();
    }
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.pressed) {
      resetUndoCoalescing();
      const float textStartX = m_horizontalPadding + kTextInnerInset;
      const std::size_t offset = xToByteOffset(data.localX - textStartX + m_scrollOffset - m_contentLeadSlack);
      const auto now = std::chrono::steady_clock::now();
      const bool isDoubleClick = data.button == BTN_LEFT && m_hasLastPrimaryPress &&
                                 now - m_lastPrimaryPressTime <= kDoubleClickThreshold &&
                                 std::abs(data.localX - m_lastPrimaryPressX) <= kDoubleClickDistance &&
                                 std::abs(data.localY - m_lastPrimaryPressY) <= kDoubleClickDistance;

      if (data.button == BTN_LEFT) {
        m_lastPrimaryPressTime = now;
        m_lastPrimaryPressX = data.localX;
        m_lastPrimaryPressY = data.localY;
        m_hasLastPrimaryPress = true;
      } else {
        m_hasLastPrimaryPress = false;
      }

      if (isDoubleClick) {
        selectWordAtByteOffset(offset);
      } else {
        m_cursorPos = offset;
        m_selectionAnchor = offset;
      }
      updateInteractiveGeometry();
      revealCursor();
      markLayoutDirty();
      markPaintDirty();
    }
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_inputArea != nullptr && m_inputArea->pressed()) {
      resetUndoCoalescing();
      const float widthPx = width() > 0.0f ? width() : kMinWidth;
      const float edgePx = std::max(12.0f, m_horizontalPadding);
      const float scrollNudge = std::max(4.0f, textViewportWidth() * 0.02f);
      bool handledByEdgeScroll = false;
      if (data.localX <= edgePx) {
        m_scrollOffset -= scrollNudge;
        m_cursorPos = prevCharPos(m_value, m_cursorPos);
        handledByEdgeScroll = true;
      } else if (data.localX >= widthPx - edgePx) {
        m_scrollOffset += scrollNudge;
        m_cursorPos = nextCharPos(m_value, m_cursorPos);
        handledByEdgeScroll = true;
      }
      clampScrollOffset();
      if (!handledByEdgeScroll) {
        const float textStartX = m_horizontalPadding + kTextInnerInset;
        m_cursorPos = xToByteOffset(data.localX - textStartX + m_scrollOffset - m_contentLeadSlack);
      }
      updateInteractiveGeometry();
      revealCursor();
      markLayoutDirty();
      markPaintDirty();
    }
  });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || m_inputArea == nullptr || !m_inputArea->focused()) {
      return false;
    }
    const float delta = data.scrollDelta(1.0f);
    if (std::abs(delta) < 0.001f) {
      return false;
    }
    resetUndoCoalescing();
    // Wheel should move caret through text, not pan viewport directly.
    constexpr int kWheelCaretStep = 1;
    if (delta > 0.0f) {
      for (int i = 0; i < kWheelCaretStep; ++i) {
        m_cursorPos = prevCharPos(m_value, m_cursorPos);
      }
    } else {
      for (int i = 0; i < kWheelCaretStep; ++i) {
        m_cursorPos = nextCharPos(m_value, m_cursorPos);
      }
    }
    m_selectionAnchor = m_cursorPos;
    updateInteractiveGeometry();
    revealCursor();
    markLayoutDirty();
    markPaintDirty();
    return true;
  });
  area->setOnKeyDown([this](const InputArea::KeyData& k) { handleKey(k.sym, k.utf32, k.modifiers, k.preedit); });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  // Optional clear button, kept above the full-field input area for hit-testing.
  auto clearButtonArea = std::make_unique<InputArea>();
  clearButtonArea->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyVisualState(); });
  clearButtonArea->setOnLeave([this]() { applyVisualState(); });
  clearButtonArea->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      clearFromButton();
    }
  });
  clearButtonArea->setVisible(false);
  m_clearButtonArea = static_cast<InputArea*>(addChild(std::move(clearButtonArea)));

  auto clearGlyph = std::make_unique<GlyphNode>();
  clearGlyph->setCodepoint(GlyphRegistry::lookup("close"));
  clearGlyph->setHitTestVisible(false);
  m_clearButtonGlyph = static_cast<GlyphNode*>(m_clearButtonArea->addChild(std::move(clearGlyph)));

  applyVisualState();
  m_paletteConn = paletteChanged().connect([this] {
    updateDisplayText();
    applyVisualState();
  });
}

void Input::setValue(std::string_view value) {
  m_value = std::string(value);
  m_cursorPos = m_value.size();
  m_selectionAnchor = m_cursorPos;
  clearEditHistory();
  updateDisplayText();
  markLayoutDirty();
}

void Input::setPlaceholder(std::string_view placeholder) {
  m_placeholder = std::string(placeholder);
  if (m_value.empty()) {
    updateDisplayText();
    markLayoutDirty();
  }
}

void Input::setFontSize(float size) {
  m_fontSize = std::max(1.0f, size);
  if (m_label != nullptr) {
    m_label->setFontSize(m_fontSize);
  }
  markLayoutDirty();
}

void Input::setControlHeight(float height) {
  m_controlHeight = std::max(1.0f, height);
  markLayoutDirty();
}

void Input::setHorizontalPadding(float padding) {
  m_horizontalPadding = std::max(0.0f, padding);
  markLayoutDirty();
}

void Input::setClearButtonEnabled(bool enabled) {
  if (m_clearButtonEnabled == enabled) {
    return;
  }
  m_clearButtonEnabled = enabled;
  markLayoutDirty();
}

void Input::setPasswordMode(bool enabled) {
  if (m_passwordMode == enabled) {
    return;
  }
  m_passwordMode = enabled;
  if (!m_passwordMode) {
    syncPasswordGlyphNodes(0);
  }
  updateDisplayText();
  markLayoutDirty();
}

void Input::setInvalid(bool invalid) {
  if (m_invalid == invalid) {
    return;
  }
  m_invalid = invalid;
  applyVisualState();
  markPaintDirty();
}

void Input::setFrameVisible(bool visible) {
  if (m_frameVisible == visible) {
    return;
  }
  m_frameVisible = visible;
  applyVisualState();
  markPaintDirty();
}

void Input::setEmbeddedOnSolidPrimary(bool embedded) {
  if (m_embeddedOnSolidPrimary == embedded) {
    return;
  }
  m_embeddedOnSolidPrimary = embedded;
  applyVisualState();
  markPaintDirty();
}

void Input::setFontWeight(FontWeight fontWeight) {
  if (m_label != nullptr) {
    m_label->setFontWeight(fontWeight);
  }
  markLayoutDirty();
}

void Input::setMinLayoutWidth(float width) {
  const float next = std::max(0.0f, width);
  if (m_minLayoutWidth == next) {
    return;
  }
  m_minLayoutWidth = next;
  markLayoutDirty();
}

void Input::setTextAlign(TextAlign align) {
  if (m_textAlign == align) {
    return;
  }
  m_textAlign = align;
  markLayoutDirty();
}

void Input::setOnChange(std::function<void(const std::string&)> callback) { m_onChange = std::move(callback); }

void Input::setOnSubmit(std::function<void(const std::string&)> callback) { m_onSubmit = std::move(callback); }

void Input::setOnKeyEvent(std::function<bool(std::uint32_t, std::uint32_t)> callback) {
  m_onKeyEvent = std::move(callback);
}

void Input::setOnFocusLoss(std::function<void()> callback) { m_onFocusLoss = std::move(callback); }

void Input::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  if (m_clearButtonArea != nullptr) {
    m_clearButtonArea->setEnabled(enabled);
  }
  setOpacity(enabled ? 1.0f : 0.55f);
  applyVisualState();
}

void Input::setTextClipboard(TextClipboard* clipboard) noexcept { g_clipboard = clipboard; }

void Input::setValidateKeyMatcher(std::function<bool(std::uint32_t, std::uint32_t)> matcher) noexcept {
  g_validateKeyMatcher = std::move(matcher);
}

void Input::setPasswordMaskStyle(PasswordMaskStyle style) noexcept { g_passwordMaskStyle = style; }

void Input::selectAll() {
  resetUndoCoalescing();
  m_selectionAnchor = 0;
  m_cursorPos = m_value.size();
  updateInteractiveGeometry();
  markPaintDirty();
}

void Input::moveCaretLeft(bool shift) {
  resetUndoCoalescing();
  if (!shift && hasSelection()) {
    m_cursorPos = selectionStart();
    m_selectionAnchor = m_cursorPos;
  } else {
    m_cursorPos = prevCharPos(m_value, m_cursorPos);
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  }
  updateDisplayText();
  markLayoutDirty();
  revealCursor();
}

void Input::moveCaretRight(bool shift) {
  resetUndoCoalescing();
  if (!shift && hasSelection()) {
    m_cursorPos = selectionEnd();
    m_selectionAnchor = m_cursorPos;
  } else {
    m_cursorPos = nextCharPos(m_value, m_cursorPos);
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  }
  updateDisplayText();
  markLayoutDirty();
  revealCursor();
}

void Input::clearSelection() {
  resetUndoCoalescing();
  m_selectionAnchor = m_cursorPos;
  updateInteractiveGeometry();
  markPaintDirty();
}

void Input::doLayout(Renderer& renderer) {
  const float minFromHint = m_minLayoutWidth > 0.0f ? m_minLayoutWidth : 0.0f;
  const float wBase = width() > 0.0f ? width() : (minFromHint > 0.0f ? minFromHint : kMinWidth);
  const float w = std::max(wBase, minFromHint);
  const float h = m_controlHeight;
  setSize(w, h);
  const bool showClearButton = clearButtonVisible();

  const bool showPasswordGlyphs = m_passwordMode && !m_value.empty();
  m_label->setVisible(!showPasswordGlyphs);
  if (!showPasswordGlyphs) {
    m_label->measure(renderer);
  }

  m_stopByte.clear();
  m_stopX.clear();
  m_stopByte.push_back(0);
  m_stopX.push_back(0.0f);
  std::size_t charCount = 0;
  float maskGlyphY = 0.0f;
  float passwordGlyphSize = 0.0f;
  if (showPasswordGlyphs) {
    passwordGlyphSize = m_fontSize * kPasswordGlyphScale;
    const auto metrics = renderer.measureGlyph(passwordMaskCodepointForIndex(0), passwordGlyphSize);
    const float glyphInkCenter = (metrics.top + metrics.bottom) * 0.5f;
    maskGlyphY = std::round(h * 0.5f - glyphInkCenter);
  }
  if (!m_value.empty()) {
    std::size_t pos = 0;
    float maskX = 0.0f;
    while (pos < m_value.size()) {
      pos = nextCharPos(m_value, pos);
      ++charCount;
      m_stopByte.push_back(pos);
      if (showPasswordGlyphs) {
        const auto metrics = renderer.measureGlyph(passwordMaskCodepointForIndex(charCount - 1), passwordGlyphSize);
        maskX += metrics.width;
        m_stopX.push_back(maskX);
      }
    }
    if (!showPasswordGlyphs) {
      renderer.measureTextCursorStops(m_value, m_fontSize, m_stopByte, m_stopX);
      if (m_stopX.size() != m_stopByte.size()) {
        m_stopX.assign(m_stopByte.size(), 0.0f);
      }
    }
  }

  m_contentLeadSlack = 0.0f;
  if (!showPasswordGlyphs && m_textAlign == TextAlign::Center) {
    const float textInset = m_horizontalPadding + kTextInnerInset;
    const float rightInset = showClearButton ? clearButtonTextReserveWidth() : textInset;
    const float vw = std::max(0.0f, w - textInset - rightInset);
    float textExtent = 0.0f;
    if (!m_value.empty() && m_stopX.size() > 1U) {
      textExtent = m_stopX.back();
    } else if (m_value.empty() && !m_placeholder.empty()) {
      textExtent = renderer.measureText(m_placeholder, m_fontSize, m_label->fontWeight()).width;
    }
    if (vw > 0.0f && textExtent > 0.0f && textExtent + 0.5f < vw) {
      m_contentLeadSlack = std::round((vw - textExtent) * 0.5f);
    }
  }

  if (m_inputArea != nullptr && m_inputArea->focused()) {
    ensureCursorVisible();
  } else {
    // Keep unfocused inputs anchored to the beginning of the text.
    m_scrollOffset = 0.0f;
  }

  if (showPasswordGlyphs) {
    syncPasswordGlyphNodes(charCount);
    float gx = m_contentLeadSlack - m_scrollOffset;
    for (std::size_t i = 0; i < m_passwordGlyphs.size(); ++i) {
      auto* glyph = m_passwordGlyphs[i];
      const char32_t codepoint = passwordMaskCodepointForIndex(i);
      const auto metrics = renderer.measureGlyph(codepoint, passwordGlyphSize);
      glyph->setCodepoint(codepoint);
      glyph->setFontSize(passwordGlyphSize);
      glyph->setColor(colorForRole(ColorRole::OnSurface));
      glyph->setPosition(gx, maskGlyphY);
      glyph->setVisible(true);
      gx += metrics.width;
    }
  } else {
    syncPasswordGlyphNodes(0);
    const float labelY = std::round((h - m_label->height()) * 0.5f);
    m_label->setPosition(-m_scrollOffset + m_contentLeadSlack, labelY);
  }

  m_background->setPosition(0.0f, 0.0f);
  m_background->setFrameSize(w, h);

  if (m_textViewport != nullptr) {
    const float textInset = m_horizontalPadding + kTextInnerInset;
    const float rightInset = showClearButton ? clearButtonTextReserveWidth() : textInset;
    const float viewportW = std::max(0.0f, w - textInset - rightInset);
    m_textViewport->setPosition(textInset, 0.0f);
    m_textViewport->setSize(viewportW, h);
  }

  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(w, h);

  if (m_clearButtonArea != nullptr && m_clearButtonGlyph != nullptr) {
    const float buttonSize = clearButtonHitWidth();
    m_clearButtonArea->setVisible(showClearButton);
    m_clearButtonArea->setPosition(std::max(0.0f, w - buttonSize), 0.0f);
    m_clearButtonArea->setFrameSize(buttonSize, h);

    const float clearGlyphSize = std::round(m_fontSize * kClearGlyphScale);
    const auto metrics = renderer.measureGlyph(m_clearButtonGlyph->codepoint(), clearGlyphSize);
    const float glyphCenterX = (metrics.left + metrics.right) * 0.5f;
    const float glyphInkCenter = (metrics.top + metrics.bottom) * 0.5f;
    m_clearButtonGlyph->setFontSize(clearGlyphSize);
    m_clearButtonGlyph->setPosition(buttonSize * 0.5f - glyphCenterX, h * 0.5f - glyphInkCenter);
  }

  updateInteractiveGeometry();
  applyVisualState();
  updateCursorVisibility();
}

void Input::handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool preedit) {
  if (m_onKeyEvent && m_onKeyEvent(sym, modifiers)) {
    return;
  }

  const bool validateMatch = g_validateKeyMatcher && g_validateKeyMatcher(sym, modifiers);
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  const bool ctrl = (modifiers & KeyMod::Ctrl) != 0;
  const bool undoShortcut = ctrl && !shift && (sym == 'z' || sym == 'Z');
  const bool redoShortcut = (ctrl && (sym == 'y' || sym == 'Y')) || (ctrl && shift && (sym == 'z' || sym == 'Z'));

  // Ignore keys that produce no text and aren't action keys we handle below
  if (utf32 == 0 && !preedit) {
    const bool navigationOrEdit = KeySymbol::isBackspace(sym) || KeySymbol::isDelete(sym) || KeySymbol::isLeft(sym) ||
                                  KeySymbol::isRight(sym) || KeySymbol::isHome(sym) || KeySymbol::isEnd(sym) ||
                                  KeySymbol::isInsert(sym) || undoShortcut || redoShortcut;
    if (!navigationOrEdit && !validateMatch) {
      return;
    }
  }

  bool changed = false;

  // Remove previous preedit text before processing
  if (m_preeditLen > 0) {
    m_value.erase(m_preeditStart, m_preeditLen);
    m_cursorPos = m_preeditStart;
    m_selectionAnchor = m_cursorPos;
    m_preeditLen = 0;
    changed = true;
  }

  const bool copyShortcut = ctrl && (KeySymbol::isInsert(sym) || sym == 'c' || sym == 'C');
  const bool cutShortcut =
      (ctrl && (sym == 'x' || sym == 'X')) || (!ctrl && shift && KeySymbol::isDelete(sym) && hasSelection());
  const bool pasteShortcut = (ctrl && (sym == 'v' || sym == 'V')) || (!ctrl && shift && KeySymbol::isInsert(sym));

  if (undoShortcut) {
    if (undoEdit()) {
      return;
    }
    if (changed) {
      updateDisplayText();
      markLayoutDirty();
      revealCursor();
      if (!preedit && m_onChange) {
        m_onChange(m_value);
      }
    }
    return;
  }
  if (redoShortcut) {
    if (redoEdit()) {
      return;
    }
    if (changed) {
      updateDisplayText();
      markLayoutDirty();
      revealCursor();
      if (!preedit && m_onChange) {
        m_onChange(m_value);
      }
    }
    return;
  }

  if (ctrl && (sym == 'a' || sym == 'A')) {
    // Select all
    resetUndoCoalescing();
    m_selectionAnchor = 0;
    m_cursorPos = m_value.size();
  } else if (copyShortcut) {
    if (g_clipboard != nullptr && hasSelection()) {
      g_clipboard->setClipboardText(m_value.substr(selectionStart(), selectionEnd() - selectionStart()));
    }
  } else if (cutShortcut) {
    if (g_clipboard != nullptr && hasSelection()) {
      pushUndoSnapshot(EditCoalesceKind::Discrete);
      g_clipboard->setClipboardText(m_value.substr(selectionStart(), selectionEnd() - selectionStart()));
      deleteSelection();
      changed = true;
    }
  } else if (pasteShortcut) {
    if (g_clipboard != nullptr) {
      if (auto text = readClipboardText(); text.has_value()) {
        pushUndoSnapshot(EditCoalesceKind::Discrete);
        if (hasSelection()) {
          deleteSelection();
        }
        m_value.insert(m_cursorPos, *text);
        m_cursorPos += text->size();
        m_selectionAnchor = m_cursorPos;
        changed = true;
      }
    }
  } else if (KeySymbol::isBackspace(sym)) {
    if (hasSelection()) {
      pushUndoSnapshot(EditCoalesceKind::Discrete);
      deleteSelection();
      changed = true;
    } else if (m_cursorPos > 0) {
      pushUndoSnapshot(EditCoalesceKind::Discrete);
      const std::size_t prev = ctrl ? previousWordStartForByteOffset(m_cursorPos) : prevCharPos(m_value, m_cursorPos);
      m_value.erase(prev, m_cursorPos - prev);
      m_cursorPos = prev;
      m_selectionAnchor = prev;
      changed = true;
    }
  } else if (KeySymbol::isDelete(sym)) {
    if (hasSelection()) {
      pushUndoSnapshot(EditCoalesceKind::Discrete);
      deleteSelection();
      changed = true;
    } else if (m_cursorPos < m_value.size()) {
      pushUndoSnapshot(EditCoalesceKind::Discrete);
      const std::size_t next = ctrl ? nextWordEndForByteOffset(m_cursorPos) : nextCharPos(m_value, m_cursorPos);
      m_value.erase(m_cursorPos, next - m_cursorPos);
      changed = true;
    }
  } else if (KeySymbol::isLeft(sym)) {
    resetUndoCoalescing();
    if (!shift && hasSelection()) {
      // Collapse to start of selection
      m_cursorPos = selectionStart();
      m_selectionAnchor = m_cursorPos;
    } else {
      m_cursorPos = ctrl ? previousWordStartForByteOffset(m_cursorPos) : prevCharPos(m_value, m_cursorPos);
      if (!shift) {
        m_selectionAnchor = m_cursorPos;
      }
    }
  } else if (KeySymbol::isRight(sym)) {
    resetUndoCoalescing();
    if (!shift && hasSelection()) {
      // Collapse to end of selection
      m_cursorPos = selectionEnd();
      m_selectionAnchor = m_cursorPos;
    } else {
      m_cursorPos = ctrl ? nextWordStartForByteOffset(m_cursorPos) : nextCharPos(m_value, m_cursorPos);
      if (!shift) {
        m_selectionAnchor = m_cursorPos;
      }
    }
  } else if (KeySymbol::isHome(sym)) {
    resetUndoCoalescing();
    m_cursorPos = 0;
    if (!shift) {
      m_selectionAnchor = 0;
    }
  } else if (KeySymbol::isEnd(sym)) {
    resetUndoCoalescing();
    m_cursorPos = m_value.size();
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  } else if (validateMatch) {
    if (m_onSubmit) {
      m_onSubmit(m_value);
    }
  } else if (utf32 >= 0x20U && utf32 != 0x7FU) {
    // Printable character (skip DEL = 0x7F)
    if (!preedit) {
      pushUndoSnapshot(hasSelection() ? EditCoalesceKind::Discrete : EditCoalesceKind::Typing);
    }
    if (hasSelection()) {
      deleteSelection();
      changed = true;
    }
    const auto bytes = utf32ToUtf8(utf32);
    m_value.insert(m_cursorPos, bytes);
    if (preedit) {
      m_preeditStart = m_cursorPos;
      m_preeditLen = bytes.size();
    }
    m_cursorPos += bytes.size();
    m_selectionAnchor = m_cursorPos;
    changed = true;
    if (!preedit) {
      noteTypingEditEnd();
    }
  }

  updateDisplayText();
  markLayoutDirty();
  revealCursor();

  if (changed && !preedit && m_onChange) {
    m_onChange(m_value);
  }
}

void Input::applyVisualState() {
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();
  const bool clearButtonHovered = m_clearButtonArea != nullptr && m_clearButtonArea->hovered();
  const bool inputHovered = (m_inputArea != nullptr && m_inputArea->hovered()) || clearButtonHovered;
  const bool readOnly = isReadOnlyVisual();
  const float chromeScale = chromeScaleForControlHeight(m_controlHeight);

  if (m_frameVisible) {
    m_background->setVisible(true);
    const Color fill = focused ? resolved(ColorRole::Surface) : resolved(ColorRole::SurfaceVariant);
    const Color border = m_invalid
                             ? resolved(ColorRole::Error)
                             : (focused ? resolved(ColorRole::Primary)
                                        : (inputHovered ? resolved(ColorRole::Hover) : resolved(ColorRole::Outline)));

    m_background->setStyle(
        RoundedRectStyle{
            .fill = fill,
            .border = border,
            .fillMode = FillMode::Solid,
            .radius = Style::scaledRadiusMd(chromeScale),
            .softness = 1.0f,
            .borderWidth = Style::borderWidth,
        }
    );
  } else if (m_background != nullptr) {
    m_background->setVisible(false);
  }

  if (m_embeddedOnSolidPrimary && !m_frameVisible) {
    auto selectionStyleEmb = m_selectionRect->style();
    selectionStyleEmb.fill = resolved(ColorRole::Surface, 0.4f);
    selectionStyleEmb.fillMode = FillMode::Solid;
    selectionStyleEmb.radius = 2.0f;
    m_selectionRect->setStyle(selectionStyleEmb);

    auto cursorStyleEmb = m_cursor->style();
    cursorStyleEmb.fill = resolved(ColorRole::Surface);
    cursorStyleEmb.fillMode = FillMode::Solid;
    cursorStyleEmb.radius = 1.0f;
    m_cursor->setStyle(cursorStyleEmb);

    const bool showingPlaceholder = m_value.empty() && !m_placeholder.empty();
    if (m_invalid) {
      m_label->setColor(colorSpecFromRole(ColorRole::Error));
    } else if (showingPlaceholder) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary, kPrimaryPlaceholderAlpha));
    } else if (readOnly) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary, 0.65f));
    } else {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    }
    const Color passwordGlyphEmb =
        m_invalid ? resolved(ColorRole::Error)
                  : (showingPlaceholder
                         ? resolved(ColorRole::OnPrimary, kPrimaryPlaceholderAlpha)
                         : (readOnly ? resolved(ColorRole::OnPrimary, 0.65f) : resolved(ColorRole::OnPrimary)));
    for (auto* glyph : m_passwordGlyphs) {
      glyph->setColor(passwordGlyphEmb);
    }
    if (m_clearButtonGlyph != nullptr) {
      m_clearButtonGlyph->setColor(resolved(ColorRole::OnPrimary, clearButtonHovered ? 1.0f : 0.72f));
    }
    return;
  }

  auto selectionStyle = m_selectionRect->style();
  selectionStyle.fill = resolved(ColorRole::Primary);
  selectionStyle.fillMode = FillMode::Solid;
  selectionStyle.radius = 2.0f;
  m_selectionRect->setStyle(selectionStyle);

  auto cursorStyle = m_cursor->style();
  cursorStyle.fill = resolved(ColorRole::Primary);
  cursorStyle.fillMode = FillMode::Solid;
  cursorStyle.radius = 1.0f;
  m_cursor->setStyle(cursorStyle);

  const bool showingPlaceholder = m_value.empty() && !m_placeholder.empty();
  const ColorSpec textColor =
      m_invalid ? colorSpecFromRole(ColorRole::Error)
                : (showingPlaceholder ? colorSpecFromRole(ColorRole::OnSurfaceVariant, kPlaceholderAlpha)
                                      : (readOnly ? colorSpecFromRole(ColorRole::OnSurfaceVariant)
                                                  : colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setColor(textColor);
  const Color passwordGlyphColor = resolveColorSpec(textColor);
  for (auto* glyph : m_passwordGlyphs) {
    glyph->setColor(passwordGlyphColor);
  }
  if (m_clearButtonGlyph != nullptr) {
    m_clearButtonGlyph->setColor(resolved(clearButtonHovered ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
  }
}

void Input::updateDisplayText() {
  if (m_value.empty() && !m_placeholder.empty()) {
    m_label->setText(m_placeholder);
  } else {
    m_label->setText(m_passwordMode ? std::string{} : m_value);
  }
}

bool Input::isReadOnlyVisual() const noexcept {
  return m_inputArea != nullptr && (!m_inputArea->focusable() || !m_inputArea->enabled());
}

void Input::clearFromButton() {
  if (m_value.empty()) {
    return;
  }
  pushUndoSnapshot(EditCoalesceKind::Discrete);
  m_value.clear();
  m_cursorPos = 0;
  m_selectionAnchor = 0;
  m_preeditStart = 0;
  m_preeditLen = 0;
  m_scrollOffset = 0.0f;
  updateDisplayText();
  if (m_clearButtonArea != nullptr) {
    m_clearButtonArea->setVisible(false);
  }
  updateInteractiveGeometry();
  revealCursor();
  applyVisualState();
  markLayoutDirty();
  markPaintDirty();
  if (m_onChange) {
    m_onChange(m_value);
  }
}

void Input::updateInteractiveGeometry() {
  if (m_cursor == nullptr || m_selectionRect == nullptr) {
    return;
  }

  const float previousScrollOffset = m_scrollOffset;
  const bool shouldRevealCursor =
      m_inputArea != nullptr && (m_inputArea->focused() || m_inputArea->pressed() || hasSelection());
  if (shouldRevealCursor) {
    ensureCursorVisible();
  } else {
    m_scrollOffset = 0.0f;
  }
  if (std::abs(m_scrollOffset - previousScrollOffset) > 0.001f) {
    markLayoutDirty();
  }

  const float controlHeight = height() > 0.0f ? height() : m_controlHeight;
  const float maxCursorHeight = std::max(0.0f, controlHeight - kCursorPadV * 2.0f);
  const float cursorHeight =
      std::clamp(controlHeight * kCursorHeightRatio, std::min(kCursorMinHeight, maxCursorHeight), maxCursorHeight);
  const float cursorY = std::round((controlHeight - cursorHeight) * 0.5f);
  const float cursorX = stopXForByte(m_cursorPos) - m_scrollOffset + m_contentLeadSlack;
  m_cursor->setPosition(cursorX, cursorY);
  m_cursor->setFrameSize(kCursorWidth, cursorHeight);

  if (hasSelection()) {
    const float selX0 = stopXForByte(selectionStart()) - m_scrollOffset + m_contentLeadSlack;
    const float selX1 = stopXForByte(selectionEnd()) - m_scrollOffset + m_contentLeadSlack;
    m_selectionRect->setPosition(selX0, cursorY);
    m_selectionRect->setFrameSize(std::max(0.0f, selX1 - selX0), cursorHeight);
    m_selectionRect->setVisible(true);
  } else {
    m_selectionRect->setVisible(false);
  }
}

void Input::ensureCursorVisible() {
  if (m_value.empty() || m_stopX.empty()) {
    m_scrollOffset = 0.0f;
    return;
  }

  const float viewportWidth = textViewportWidth();
  if (viewportWidth <= 0.0f) {
    m_scrollOffset = 0.0f;
    return;
  }

  const float cursorContentX = stopXForByte(m_cursorPos);
  const float revealPad = std::max(kCursorRevealPadding, kTextInnerInset);
  const float slack = m_contentLeadSlack;
  const float cursorVx = cursorContentX - m_scrollOffset + slack;
  const float leftEdge = revealPad;
  const float rightEdge = viewportWidth - revealPad - kCursorWidth;

  if (cursorVx < leftEdge) {
    m_scrollOffset = cursorContentX + slack - leftEdge;
  } else if (cursorVx > rightEdge) {
    m_scrollOffset = cursorContentX + slack - rightEdge;
  }

  clampScrollOffset();
}

void Input::clampScrollOffset() {
  if (m_value.empty() || m_stopX.empty()) {
    m_scrollOffset = 0.0f;
    return;
  }
  const float maxOffset = std::max(0.0f, m_stopX.back() - textViewportWidth() + kCursorWidth + kCursorRevealPadding);
  m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxOffset);
}

LayoutSize Input::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  const float minFromHint = m_minLayoutWidth > 0.0f ? m_minLayoutWidth : 0.0f;
  if (constraints.hasExactWidth()) {
    const float assignW = std::max(constraints.maxWidth, minFromHint);
    setSize(assignW, m_controlHeight);
  }
  doLayout(renderer);
  const float w = std::max(width(), minFromHint);
  return constraints.constrain(LayoutSize{.width = w, .height = height()});
}

void Input::updateCursorVisibility() {
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();
  m_cursor->setVisible(focused && m_cursorBlinkVisible);
}

void Input::revealCursor() {
  m_cursorBlinkVisible = true;
  updateCursorVisibility();
  markPaintDirty();
  if (m_inputArea != nullptr && m_inputArea->focused()) {
    m_cursorBlinkTimer.start(kCursorBlinkResumeDelay, [this]() { startCursorBlink(); });
  } else {
    m_cursorBlinkTimer.stop();
  }
}

void Input::startCursorBlink() {
  m_cursorBlinkTimer.startRepeating(kCursorBlinkInterval, [this]() {
    if (m_inputArea == nullptr || !m_inputArea->focused()) {
      stopCursorBlink();
      return;
    }
    m_cursorBlinkVisible = !m_cursorBlinkVisible;
    updateCursorVisibility();
    markPaintDirty();
  });
}

void Input::stopCursorBlink() {
  m_cursorBlinkTimer.stop();
  m_cursorBlinkVisible = false;
}

void Input::selectWordAtByteOffset(std::size_t offset) {
  const std::size_t start = wordStartForByteOffset(offset);
  const std::size_t end = wordEndForByteOffset(offset);
  m_selectionAnchor = start;
  m_cursorPos = end;
}

std::size_t Input::wordStartForByteOffset(std::size_t offset) const {
  if (m_value.empty()) {
    return 0;
  }

  std::size_t pos = std::min(offset, m_value.size());
  if (pos == m_value.size() && pos > 0) {
    pos = prevCharPos(m_value, pos);
  }

  if (!isWordCodepoint(m_value, pos)) {
    return pos;
  }

  while (pos > 0) {
    const std::size_t prev = prevCharPos(m_value, pos);
    if (prev == pos || !isWordCodepoint(m_value, prev)) {
      break;
    }
    pos = prev;
  }
  return pos;
}

std::size_t Input::wordEndForByteOffset(std::size_t offset) const {
  if (m_value.empty()) {
    return 0;
  }

  std::size_t pos = std::min(offset, m_value.size());
  if (pos == m_value.size() && pos > 0) {
    pos = prevCharPos(m_value, pos);
  }

  if (!isWordCodepoint(m_value, pos)) {
    return nextCharPos(m_value, pos);
  }

  std::size_t end = nextCharPos(m_value, pos);
  while (end < m_value.size() && isWordCodepoint(m_value, end)) {
    end = nextCharPos(m_value, end);
  }
  return end;
}

std::size_t Input::previousWordStartForByteOffset(std::size_t offset) const {
  if (m_value.empty()) {
    return 0;
  }

  std::size_t pos = std::min(offset, m_value.size());
  while (pos > 0) {
    const std::size_t prev = prevCharPos(m_value, pos);
    if (prev == pos || isWordCodepoint(m_value, prev)) {
      break;
    }
    pos = prev;
  }
  while (pos > 0) {
    const std::size_t prev = prevCharPos(m_value, pos);
    if (prev == pos || !isWordCodepoint(m_value, prev)) {
      break;
    }
    pos = prev;
  }
  return pos;
}

std::size_t Input::nextWordStartForByteOffset(std::size_t offset) const {
  if (m_value.empty()) {
    return 0;
  }

  std::size_t pos = std::min(offset, m_value.size());
  if (pos < m_value.size() && isWordCodepoint(m_value, pos)) {
    while (pos < m_value.size() && isWordCodepoint(m_value, pos)) {
      pos = nextCharPos(m_value, pos);
    }
  }
  while (pos < m_value.size() && !isWordCodepoint(m_value, pos)) {
    pos = nextCharPos(m_value, pos);
  }
  return pos;
}

std::size_t Input::nextWordEndForByteOffset(std::size_t offset) const {
  if (m_value.empty()) {
    return 0;
  }

  std::size_t pos = std::min(offset, m_value.size());
  while (pos < m_value.size() && !isWordCodepoint(m_value, pos)) {
    pos = nextCharPos(m_value, pos);
  }
  while (pos < m_value.size() && isWordCodepoint(m_value, pos)) {
    pos = nextCharPos(m_value, pos);
  }
  return pos;
}

void Input::syncPasswordGlyphNodes(std::size_t count) {
  if (m_textViewport == nullptr) {
    return;
  }
  while (m_passwordGlyphs.size() > count) {
    auto* node = m_passwordGlyphs.back();
    (void)m_textViewport->removeChild(node);
    m_passwordGlyphs.pop_back();
  }
  while (m_passwordGlyphs.size() < count) {
    auto glyph = std::make_unique<GlyphNode>();
    auto* glyphPtr = static_cast<GlyphNode*>(m_textViewport->insertChildAt(2, std::move(glyph)));
    m_passwordGlyphs.push_back(glyphPtr);
  }
}

float Input::textViewportWidth() const noexcept {
  const float w = width() > 0.0f ? width() : kMinWidth;
  const float textInset = m_horizontalPadding + kTextInnerInset;
  const float rightInset = clearButtonVisible() ? clearButtonTextReserveWidth() : textInset;
  return std::max(0.0f, w - textInset - rightInset);
}

bool Input::clearButtonVisible() const noexcept { return m_clearButtonEnabled && !m_value.empty(); }

float Input::clearButtonHitWidth() const noexcept {
  if (!clearButtonVisible()) {
    return 0.0f;
  }
  const float h = height() > 0.0f ? height() : m_controlHeight;
  return std::max(0.0f, h);
}

float Input::clearButtonTextReserveWidth() const noexcept {
  if (!clearButtonVisible()) {
    return 0.0f;
  }
  const float clearGlyphSize = std::round(m_fontSize * kClearGlyphScale);
  return clearButtonHitWidth() * 0.5f + clearGlyphSize * 0.5f + kTextInnerInset;
}

bool Input::hasSelection() const noexcept { return m_selectionAnchor != m_cursorPos; }

std::size_t Input::selectionStart() const noexcept { return std::min(m_selectionAnchor, m_cursorPos); }

std::size_t Input::selectionEnd() const noexcept { return std::max(m_selectionAnchor, m_cursorPos); }

Input::EditSnapshot Input::currentEditSnapshot() const {
  return EditSnapshot{
      .value = m_value,
      .cursorPos = m_cursorPos,
      .selectionAnchor = m_selectionAnchor,
  };
}

void Input::deleteSelection() {
  const std::size_t start = selectionStart();
  const std::size_t end = selectionEnd();
  m_value.erase(start, end - start);
  m_cursorPos = start;
  m_selectionAnchor = start;
}

void Input::clearEditHistory() {
  m_undoStack.clear();
  m_redoStack.clear();
  resetUndoCoalescing();
}

void Input::resetUndoCoalescing() {
  m_lastEditCoalesceKind = EditCoalesceKind::None;
  m_lastUndoRecordTime = {};
  m_typingCoalesceCursorPos = m_cursorPos;
}

void Input::pushUndoSnapshot(EditCoalesceKind kind) {
  if (kind == EditCoalesceKind::None) {
    resetUndoCoalescing();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (kind == EditCoalesceKind::Typing && m_lastEditCoalesceKind == EditCoalesceKind::Typing &&
      m_cursorPos == m_typingCoalesceCursorPos && !hasSelection() && !m_undoStack.empty() &&
      now - m_lastUndoRecordTime <= kTypingUndoCoalesceWindow) {
    m_redoStack.clear();
    m_lastUndoRecordTime = now;
    return;
  }

  const EditSnapshot snapshot = currentEditSnapshot();
  if (m_undoStack.empty() || !(m_undoStack.back() == snapshot)) {
    m_undoStack.push_back(snapshot);
    if (m_undoStack.size() > kUndoStackLimit) {
      m_undoStack.erase(m_undoStack.begin());
    }
  }
  m_redoStack.clear();
  m_lastEditCoalesceKind = kind;
  m_lastUndoRecordTime = now;
  m_typingCoalesceCursorPos = m_cursorPos;
}

void Input::noteTypingEditEnd() { m_typingCoalesceCursorPos = m_cursorPos; }

bool Input::undoEdit() { return restoreFromHistory(m_undoStack, m_redoStack); }

bool Input::redoEdit() { return restoreFromHistory(m_redoStack, m_undoStack); }

bool Input::restoreFromHistory(std::vector<EditSnapshot>& source, std::vector<EditSnapshot>& target) {
  if (source.empty()) {
    resetUndoCoalescing();
    return false;
  }

  const EditSnapshot current = currentEditSnapshot();
  const EditSnapshot snapshot = source.back();
  source.pop_back();
  if (target.empty() || !(target.back() == current)) {
    target.push_back(current);
    if (target.size() > kUndoStackLimit) {
      target.erase(target.begin());
    }
  }
  restoreEditSnapshot(snapshot);
  resetUndoCoalescing();
  return true;
}

void Input::restoreEditSnapshot(const EditSnapshot& snapshot) {
  const std::string previousValue = m_value;
  m_value = snapshot.value;
  m_cursorPos = std::min(snapshot.cursorPos, m_value.size());
  m_selectionAnchor = std::min(snapshot.selectionAnchor, m_value.size());
  m_preeditStart = 0;
  m_preeditLen = 0;
  updateDisplayText();
  updateInteractiveGeometry();
  revealCursor();
  applyVisualState();
  markLayoutDirty();
  markPaintDirty();
  if (m_value != previousValue && m_onChange) {
    m_onChange(m_value);
  }
}

std::size_t Input::xToByteOffset(float localX) const {
  if (m_stopX.empty() || localX <= 0.0f) {
    return 0;
  }
  if (localX >= m_stopX.back()) {
    return m_stopByte.back();
  }
  for (std::size_t i = 1; i < m_stopX.size(); ++i) {
    const float mid = (m_stopX[i - 1] + m_stopX[i]) * 0.5f;
    if (localX < mid) {
      return m_stopByte[i - 1];
    }
  }
  return m_stopByte.back();
}

float Input::stopXForByte(std::size_t bytePos) const {
  for (std::size_t i = 0; i < m_stopByte.size(); ++i) {
    if (m_stopByte[i] == bytePos) {
      return m_stopX[i];
    }
  }
  return m_stopX.empty() ? 0.0f : m_stopX.back();
}

std::size_t Input::nextCharPos(const std::string& s, std::size_t pos) {
  if (pos >= s.size()) {
    return pos;
  }
  ++pos;
  while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
    ++pos;
  }
  return pos;
}

std::size_t Input::prevCharPos(const std::string& s, std::size_t pos) {
  if (pos == 0) {
    return 0;
  }
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
    --pos;
  }
  return pos;
}

std::string Input::utf32ToUtf8(std::uint32_t cp) {
  std::string result;
  if (cp < 0x80U) {
    result += static_cast<char>(cp);
  } else if (cp < 0x800U) {
    result += static_cast<char>(0xC0U | (cp >> 6U));
    result += static_cast<char>(0x80U | (cp & 0x3FU));
  } else if (cp < 0x10000U) {
    result += static_cast<char>(0xE0U | (cp >> 12U));
    result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    result += static_cast<char>(0x80U | (cp & 0x3FU));
  } else {
    result += static_cast<char>(0xF0U | (cp >> 18U));
    result += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
    result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    result += static_cast<char>(0x80U | (cp & 0x3FU));
  }
  return result;
}
