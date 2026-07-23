#pragma once

#include "core/timer_manager.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "ui/text_input_client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class TextClipboard;
class GlyphNode;
class InputArea;
class Label;
class RectNode;

class Input : public Node, public TextInputClient {
public:
  enum class PasswordMaskStyle : std::uint8_t {
    CircleFilled = 0,
    RandomIcons = 1,
  };

  Input();
  ~Input() override;

  void setValue(std::string_view value);
  void setPlaceholder(std::string_view placeholder);
  void setFontSize(float size);
  void setControlHeight(float height);
  void setHorizontalPadding(float padding);
  void setClearButtonEnabled(bool enabled);
  void setPasswordMode(bool enabled);
  /// Multi-line editing: Enter inserts '\n' (Ctrl+Enter submits), the text wraps
  /// at the viewport width and scrolls vertically. The control keeps whatever
  /// height layout assigns (explicit height or flex-grown) instead of forcing
  /// the single-line control height. Mutually exclusive with password mode.
  void setMultiline(bool enabled);
  void setInvalid(bool invalid);
  void setFrameVisible(bool visible);
  /// When the frame is hidden, treat the field as sitting on a solid Primary fill (e.g. segmented control center).
  void setEmbeddedOnSolidPrimary(bool embedded);
  void setFontWeight(FontWeight fontWeight);
  void setMinLayoutWidth(float width);
  void setTextAlign(TextAlign align);
  void setOnChange(std::function<void(const std::string&)> callback);
  void setOnSubmit(std::function<void(const std::string&)> callback);
  void setOnKeyEvent(std::function<bool(std::uint32_t sym, std::uint32_t modifiers)> callback);
  void setOnFocusLoss(std::function<void()> callback);
  void setOnFocusGain(std::function<void()> callback);
  void setSubmitOnFocusLoss(bool enabled);
  void setEnabled(bool enabled);
  void setSurfaceOpacity(float opacity);
  void setFrameRadius(float radius);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  void selectAll();
  void moveCaretLeft(bool shift = false);
  void moveCaretRight(bool shift = false);

  // Set once at application startup; all Input instances use this for Ctrl+C/X/V.
  static void setTextClipboard(TextClipboard* clipboard) noexcept;
  /// Submit invokes onSubmit only when this matcher returns true (Application wires ConfigService validate keybinds).
  static void setValidateKeyMatcher(std::function<bool(std::uint32_t sym, std::uint32_t modifiers)> matcher) noexcept;
  static void setPasswordMaskStyle(PasswordMaskStyle style) noexcept;
  void clearSelection();

  [[nodiscard]] const std::string& value() const noexcept { return m_value; }
  [[nodiscard]] InputArea* inputArea() const noexcept { return m_inputArea; }
  [[nodiscard]] bool invalid() const noexcept { return m_invalid; }

  [[nodiscard]] TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService& service) override;
  void textInputDeactivated(TextInputService& service) override;

private:
  enum class EditCoalesceKind : std::uint8_t {
    None = 0,
    Typing = 1,
    Discrete = 2,
  };

  struct EditSnapshot {
    std::string value;
    std::size_t cursorPos = 0;
    std::size_t selectionAnchor = 0;

    bool operator==(const EditSnapshot&) const = default;
  };

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool preedit = false);
  void notifyTextInputStateChanged(TextInputChangeCause cause);
  bool removePreeditText();
  bool deleteSurroundingText(std::uint32_t beforeLength, std::uint32_t afterLength);
  void applyVisualState();
  void updateDisplayText();
  void requestCaretUpdate();
  void updateInteractiveGeometry();
  void clearFromButton();
  void updateCursorVisibility();
  void revealCursor();
  void startCursorBlink();
  void stopCursorBlink();
  void ensureCursorVisible();
  void clampScrollOffset();
  void clampEditState();
  void selectWordAtByteOffset(std::size_t offset);
  [[nodiscard]] std::size_t wordStartForByteOffset(std::size_t offset) const;
  [[nodiscard]] std::size_t wordEndForByteOffset(std::size_t offset) const;
  [[nodiscard]] std::size_t previousWordStartForByteOffset(std::size_t offset) const;
  [[nodiscard]] std::size_t nextWordStartForByteOffset(std::size_t offset) const;
  [[nodiscard]] std::size_t nextWordEndForByteOffset(std::size_t offset) const;
  [[nodiscard]] float textViewportWidth() const noexcept;
  [[nodiscard]] bool clearButtonVisible() const noexcept;
  [[nodiscard]] float clearButtonHitWidth() const noexcept;
  [[nodiscard]] float clearButtonTextReserveWidth() const noexcept;
  [[nodiscard]] bool hasSelection() const noexcept;
  [[nodiscard]] std::size_t selectionStart() const noexcept;
  [[nodiscard]] std::size_t selectionEnd() const noexcept;
  [[nodiscard]] bool isReadOnlyVisual() const noexcept;
  [[nodiscard]] EditSnapshot currentEditSnapshot() const;
  void deleteSelection();
  void clearEditHistory();
  void resetUndoCoalescing();
  void pushUndoSnapshot(EditCoalesceKind kind);
  void noteTypingEditEnd();
  bool undoEdit();
  bool redoEdit();
  bool restoreFromHistory(std::vector<EditSnapshot>& source, std::vector<EditSnapshot>& target);
  void restoreEditSnapshot(const EditSnapshot& snapshot);
  [[nodiscard]] std::size_t xToByteOffset(float localX) const;
  [[nodiscard]] float stopXForByte(std::size_t bytePos) const;
  void syncPasswordGlyphNodes(std::size_t count);
  void animatePasswordGlyphIn(GlyphNode& glyph);

  // Multiline mode: geometry comes from the wrapped cursor-stop rects
  // (m_stopRect, parallel to m_stopByte) instead of the 1-D x array.
  [[nodiscard]] bool multilineStopsValid() const noexcept;
  [[nodiscard]] std::size_t stopIndexForByte(std::size_t bytePos) const;
  [[nodiscard]] std::size_t pointToByteOffset(float localX, float localY) const;
  [[nodiscard]] std::size_t lineStartForByte(std::size_t bytePos) const;
  [[nodiscard]] std::size_t lineEndForByte(std::size_t bytePos) const;
  [[nodiscard]] std::size_t byteForVerticalMove(std::size_t from, int lineDelta);
  [[nodiscard]] float contentTextHeight() const noexcept;
  [[nodiscard]] float textViewportHeight() const noexcept;
  [[nodiscard]] float currentLineHeight() const noexcept;
  void ensureCursorVisibleY();
  void clampScrollOffsetY();
  void updateMultilineSelection();
  void syncSelectionLineRects(std::size_t count);

  void markTextContentChanged();
  void rebuildCursorStops(Renderer& renderer);
  void rebuildCursorStopsFull(Renderer& renderer);
  void recomputeContentLeadSlack(Renderer& renderer, float width, bool showClearButton);
  void updateLabelVisibleSlice(Renderer& renderer);
  void syncLabelScrollPosition();
  [[nodiscard]] std::size_t visibleLabelStartByte() const;
  [[nodiscard]] std::size_t visibleLabelEndByte(float contentWidth, std::size_t startByte) const;

  static std::size_t nextCharPos(const std::string& s, std::size_t pos);
  static std::size_t prevCharPos(const std::string& s, std::size_t pos);
  static std::string utf32ToUtf8(std::uint32_t codepoint);

  RectNode* m_background = nullptr;
  Node* m_textViewport = nullptr;
  RectNode* m_selectionRect = nullptr;
  Label* m_label = nullptr;
  RectNode* m_cursor = nullptr;
  InputArea* m_inputArea = nullptr;
  InputArea* m_clearButtonArea = nullptr;
  GlyphNode* m_clearButtonGlyph = nullptr;

  std::string m_value;
  std::string m_placeholder;
  std::size_t m_cursorPos = 0;
  std::size_t m_selectionAnchor = 0;
  std::size_t m_preeditStart = 0;
  std::size_t m_preeditLen = 0;
  std::vector<EditSnapshot> m_undoStack;
  std::vector<EditSnapshot> m_redoStack;
  EditCoalesceKind m_lastEditCoalesceKind = EditCoalesceKind::None;
  std::chrono::steady_clock::time_point m_lastUndoRecordTime;
  std::size_t m_typingCoalesceCursorPos = 0;

  std::vector<float> m_stopX;
  std::vector<std::size_t> m_stopByte;
  std::vector<TextCursorStop> m_stopRect;
  float m_stopsBuiltForWidth = -1.0f;
  bool m_textMetricsDirty = true;
  float m_cachedLabelY = 0.0f;
  std::string m_labelVisibleSlice;
  std::size_t m_labelVisibleStartByte = 0;
  float m_labelSliceOriginX = 0.0f;
  std::vector<GlyphNode*> m_passwordGlyphs;
  std::vector<RectNode*> m_selectionLineRects;
  float m_scrollOffset = 0.0f;
  float m_scrollOffsetY = 0.0f;
  // Sticky caret column for Up/Down runs; negative = unset.
  float m_goalCaretX = -1.0f;
  bool m_cursorBlinkVisible = true;
  Timer m_cursorBlinkTimer;

  std::function<void(const std::string&)> m_onChange;
  std::function<void(const std::string&)> m_onSubmit;
  std::function<bool(std::uint32_t, std::uint32_t)> m_onKeyEvent;
  std::function<void()> m_onFocusLoss;
  std::function<void()> m_onFocusGain;
  bool m_submitOnFocusLoss = false;
  float m_fontSize = Style::fontSizeBody;
  float m_controlHeight = Style::controlHeight;
  float m_horizontalPadding = Style::spaceMd;
  bool m_clearButtonEnabled = false;
  bool m_passwordMode = false;
  bool m_multiline = false;
  bool m_invalid = false;
  bool m_frameVisible = true;
  bool m_embeddedOnSolidPrimary = false;
  float m_surfaceOpacity = 1.0f;
  float m_frameRadius = Style::radiusMd;
  bool m_enabled = true;
  TextInputService* m_textInputService = nullptr;
  float m_minLayoutWidth = 0.0f;
  float m_contentLeadSlack = 0.0f;
  TextAlign m_textAlign = TextAlign::Start;
  std::chrono::steady_clock::time_point m_lastPrimaryPressTime;
  float m_lastPrimaryPressX = 0.0f;
  float m_lastPrimaryPressY = 0.0f;
  bool m_hasLastPrimaryPress = false;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_inputBordersConn;

  // Detects synchronous self-destruction from user callbacks (onSubmit/onKeyEvent
  // can close a dialog that owns this Input). handleKey takes a weak_ptr to it and
  // bails before touching members if the callback tore us down.
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
};
