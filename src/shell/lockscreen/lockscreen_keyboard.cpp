#include "shell/lockscreen/lockscreen_keyboard.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

  using lockscreen_keyboard::KeyAction;
  using lockscreen_keyboard::KeyCap;
  using lockscreen_keyboard::Layer;

  constexpr float kKeyGap = Style::spaceXs + 2.0F;
  constexpr float kPanelPadding = Style::spaceSm;
  constexpr float kRowUnits = 10.0F;
  constexpr float kMinKeyHeight = 40.0F;
  constexpr float kMaxKeyHeight = 58.0F;

  ButtonVariant variantFor(KeyAction action) {
    switch (action) {
    case KeyAction::Character:
    case KeyAction::Space:
      return ButtonVariant::Default;
    case KeyAction::Enter:
      return ButtonVariant::Primary;
    default:
      return ButtonVariant::Secondary;
    }
  }

  float keyUnitWidth(float panelWidth) {
    const float inner = std::max(0.0F, panelWidth - kPanelPadding * 2.0F);
    return std::max(0.0F, (inner - kKeyGap * (kRowUnits - 1.0F)) / kRowUnits);
  }

  float keyHeightFor(float panelWidth) {
    // Keys stay roughly square on phones and get flatter on wide screens.
    return std::clamp(std::round(keyUnitWidth(panelWidth) * 1.1F), kMinKeyHeight, kMaxKeyHeight);
  }

} // namespace

std::unique_ptr<Flex> LockscreenKeyboard::build(KeyCallback onKey, std::function<void()> onHide) {
  m_onKey = std::move(onKey);
  m_onHide = std::move(onHide);
  m_rows.clear();
  m_keys.clear();
  m_layer = Layer::Lower;

  auto panel = ui::flex(
      FlexDirection::Vertical,
      {
          .out = &m_panel,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Start,
          .gap = kKeyGap,
          .padding = kPanelPadding,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.94F),
          .radius = Style::radiusXl,
          .border = colorSpecFromRole(ColorRole::OutlineVariant),
          .visible = false,
      }
  );

  const auto& rows = lockscreen_keyboard::rowsFor(m_layer);
  for (std::size_t r = 0; r < rows.size(); ++r) {
    Flex* rowFlex = nullptr;
    panel->addChild(
        ui::flex(
            FlexDirection::Horizontal,
            {
                .out = &rowFlex,
                .align = FlexAlign::Center,
                .justify = FlexJustify::Center,
                .gap = kKeyGap,
                .widthPolicy = FlexSizePolicy::Fill,
                .heightPolicy = FlexSizePolicy::Content,
            }
        )
    );
    m_rows.push_back(rowFlex);

    for (std::size_t i = 0; i < rows[r].size(); ++i) {
      const KeyCap& key = rows[r][i];
      Button* button = nullptr;
      rowFlex->addChild(
          ui::button({
              .out = &button,
              .text = "",
              .variant = variantFor(key.action),
              .onClick = [this, r, i] { handlePress(r, i); },
              .configure =
                  [](Button& b) {
                    // Keys must never take focus: a press would blur the password field (dropping its
                    // text-input state), so the next physical key after hiding the keyboard is lost.
                    b.setTabStop(false);
                    if (InputArea* area = b.inputArea(); area != nullptr) {
                      area->setFocusable(false);
                    }
                  },
          })
      );
      m_keys.push_back(KeySlot{.button = button, .row = r, .index = i});
    }
  }

  applyLayer();
  return panel;
}

float LockscreenKeyboard::preferredHeight(float width) noexcept {
  const auto rows = static_cast<float>(lockscreen_keyboard::kRowCount);
  return keyHeightFor(width) * rows + kKeyGap * (rows - 1.0F) + kPanelPadding * 2.0F;
}

void LockscreenKeyboard::setVisible(bool visible) {
  m_visible = visible;
  if (m_panel != nullptr) {
    m_panel->setVisible(visible);
  }
}

void LockscreenKeyboard::setRadius(float panelRadius, float keyRadius) {
  m_keyRadius = keyRadius;
  if (m_panel != nullptr) {
    m_panel->setRadius(panelRadius);
  }
}

void LockscreenKeyboard::reset() {
  if (m_layer != Layer::Lower) {
    m_layer = Layer::Lower;
    applyLayer();
  }
}

void LockscreenKeyboard::handlePress(std::size_t row, std::size_t index) {
  const auto& rows = lockscreen_keyboard::rowsFor(m_layer);
  if (row >= rows.size() || index >= rows[row].size()) {
    return;
  }
  const KeyCap key = rows[row][index];

  if (key.action == KeyAction::Hide) {
    if (m_onHide) {
      m_onHide();
    }
    return;
  }

  if (const auto output = lockscreen_keyboard::outputFor(key); output.has_value() && m_onKey) {
    m_onKey(*output);
  }

  const Layer next = lockscreen_keyboard::nextLayer(m_layer, key.action);
  if (next != m_layer) {
    m_layer = next;
    applyLayer();
  }
}

void LockscreenKeyboard::applyLayer() {
  const auto& rows = lockscreen_keyboard::rowsFor(m_layer);
  for (const KeySlot& slot : m_keys) {
    if (slot.button == nullptr || slot.row >= rows.size() || slot.index >= rows[slot.row].size()) {
      continue;
    }
    const KeyCap& key = rows[slot.row][slot.index];
    if (key.action == KeyAction::Character) {
      // Character slots never carry a glyph (Button would otherwise draw a placeholder icon).
      slot.button->setText(key.text);
      continue;
    }
    const std::string_view glyph = lockscreen_keyboard::specialGlyph(key.action, m_layer);
    slot.button->setText(lockscreen_keyboard::specialLabel(key.action, m_layer));
    if (!glyph.empty()) {
      slot.button->setGlyph(glyph);
    }
    // Button hides an empty label itself but never its glyph.
    if (auto* glyphNode = slot.button->glyph(); glyphNode != nullptr) {
      glyphNode->setVisible(!glyph.empty());
    }
  }
}

void LockscreenKeyboard::arrange(Renderer& renderer, const LayoutRect& rect) {
  if (m_panel == nullptr) {
    return;
  }
  const float unit = keyUnitWidth(rect.width);
  const float keyHeight = keyHeightFor(rect.width);
  const float fontSize = std::clamp(std::round(keyHeight * 0.38F), Style::fontSizeBody, Style::fontSizeHeader);
  const float glyphSize = std::round(fontSize * 1.1F);

  const auto& rows = lockscreen_keyboard::rowsFor(m_layer);
  for (const KeySlot& slot : m_keys) {
    if (slot.button == nullptr || slot.row >= rows.size() || slot.index >= rows[slot.row].size()) {
      continue;
    }
    const float units = rows[slot.row][slot.index].width;
    const float keyWidth = std::floor(unit * units + kKeyGap * (units - 1.0F));
    // An explicit size counts as an assigned box for Button, which otherwise squares glyph-only keys
    // (Shift, Backspace) to their width whenever they are re-laid out after a relabel.
    slot.button->setSize(keyWidth, keyHeight);
    slot.button->setMinWidth(keyWidth);
    slot.button->setMaxWidth(keyWidth);
    slot.button->setMinHeight(keyHeight);
    slot.button->setMaxHeight(keyHeight);
    slot.button->setControlHeight(keyHeight);
    slot.button->setFontSize(fontSize);
    if (slot.button->glyph() != nullptr) {
      slot.button->setGlyphSize(glyphSize);
    }
    slot.button->setRadius(m_keyRadius);
  }

  m_panel->setMinWidth(rect.width);
  m_panel->setMaxWidth(rect.width);
  m_panel->setMinHeight(rect.height);
  m_panel->setMaxHeight(rect.height);
  m_panel->arrange(renderer, rect);
}
