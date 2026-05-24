#include "shell/bar/widgets/lock_keys_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "system/lock_keys_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

  void configureLabel(Label* label, const std::string& text, bool visible, float contentScale, FontWeight fontWeight) {
    if (label == nullptr) {
      return;
    }

    label->setVisible(visible);
    label->setFontSize(Style::fontSizeBody * contentScale);
    label->setFontWeight(fontWeight);
    label->setText(text);
  }

} // namespace

LockKeysWidget::LockKeysWidget(
    LockKeysService* lockKeys, bool showCapsLock, bool showNumLock, bool showScrollLock, bool hideWhenOff,
    DisplayMode displayMode
)
    : m_lockKeys(lockKeys), m_showCapsLock(showCapsLock), m_showNumLock(showNumLock), m_showScrollLock(showScrollLock),
      m_hideWhenOff(hideWhenOff), m_displayMode(displayMode) {}

LockKeysWidget::DisplayMode LockKeysWidget::parseDisplayMode(const std::string& value) {
  return value == "full" ? DisplayMode::Full : DisplayMode::Short;
}

void LockKeysWidget::create() {
  auto rootNode = std::make_unique<Node>();

  rootNode->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "lock",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  rootNode->addChild(
      ui::label({
          .out = &m_capsLabel,
      })
  );

  rootNode->addChild(
      ui::label({
          .out = &m_numLabel,
      })
  );

  rootNode->addChild(
      ui::label({
          .out = &m_scrollLabel,
      })
  );

  setRoot(std::move(rootNode));
}

void LockKeysWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (root() == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;

  sync(renderer);

  if (!root()->visible()) {
    root()->setParticipatesInLayout(false);
    return;
  }

  constexpr float kSpacing = Style::spaceXs;
  const float spacing = kSpacing * m_contentScale;
  float x = 0.0f;
  float y = 0.0f;
  float h = 0.0f;
  float w = 0.0f;

  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
    m_glyph->measure(renderer);
    if (m_isVertical) {
      y += m_glyph->height() + spacing;
      w = std::max(w, m_glyph->width());
    } else {
      m_glyph->setPosition(0.0f, 0.0f);
      x += m_glyph->width() + spacing;
      h = std::max(h, m_glyph->height());
    }
  }

  auto layoutLabel = [&](Label* label) {
    if (label == nullptr || !label->visible()) {
      return;
    }
    label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
    label->setMaxWidth(m_isVertical ? containerWidth : 0.0f);
    label->measure(renderer);
    if (m_isVertical) {
      y += label->height() + spacing;
      w = std::max(w, label->width());
    } else {
      label->setPosition(x, 0.0f);
      x += label->width() + spacing;
      h = std::max(h, label->height());
    }
  };

  layoutLabel(m_capsLabel);
  layoutLabel(m_numLabel);
  layoutLabel(m_scrollLabel);

  if (m_isVertical) {
    if (y > 0.0f) {
      y -= spacing;
    }
    float cursorY = 0.0f;
    if (m_glyph != nullptr) {
      m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
      cursorY = m_glyph->height() + spacing;
    }
    auto placeLabel = [&](Label* label) {
      if (label == nullptr || !label->visible()) {
        return;
      }
      label->setPosition(std::round((w - label->width()) * 0.5f), cursorY);
      cursorY += label->height() + spacing;
    };
    placeLabel(m_capsLabel);
    placeLabel(m_numLabel);
    placeLabel(m_scrollLabel);
    root()->setSize(w, y);
  } else {
    if (x > 0.0f) {
      x -= spacing;
    }
    if (m_glyph != nullptr) {
      const float glyphY = std::round((h - m_glyph->height()) * 0.5f);
      m_glyph->setPosition(0.0f, glyphY);
    }
    auto centerLabel = [h](Label* label) {
      if (label == nullptr || !label->visible()) {
        return;
      }
      label->setPosition(label->x(), std::round((h - label->height()) * 0.5f));
    };
    centerLabel(m_capsLabel);
    centerLabel(m_numLabel);
    centerLabel(m_scrollLabel);
    root()->setSize(x, h);
  }
}

void LockKeysWidget::doUpdate(Renderer& renderer) { sync(renderer); }

void LockKeysWidget::sync(Renderer& renderer) {
  (void)renderer;

  const WaylandSeat::LockKeysState lockState =
      m_lockKeys != nullptr ? m_lockKeys->state() : WaylandSeat::LockKeysState{};

  const bool capsVisible = m_showCapsLock && (!m_hideWhenOff || lockState.capsLock);
  const bool numVisible = m_showNumLock && (!m_hideWhenOff || lockState.numLock);
  const bool scrollVisible = m_showScrollLock && (!m_hideWhenOff || lockState.scrollLock);
  const bool anyVisible = capsVisible || numVisible || scrollVisible;

  CachedState current{
      .capsLock = lockState.capsLock,
      .numLock = lockState.numLock,
      .scrollLock = lockState.scrollLock,
      .anyVisible = anyVisible,
  };

  if (m_hasState && current == m_cachedState) {
    return;
  }

  m_cachedState = current;
  m_hasState = true;

  if (auto* node = root(); node != nullptr) {
    const bool show = anyVisible || !m_hideWhenOff;
    node->setVisible(show);
    node->setParticipatesInLayout(show);
  }

  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  }

  const bool full = m_displayMode == DisplayMode::Full;
  configureLabel(
      m_capsLabel, full ? i18n::tr("bar.widgets.lock-keys.caps") : i18n::tr("bar.widgets.lock-keys.caps-short"),
      capsVisible, m_contentScale, labelFontWeight()
  );
  configureLabel(
      m_numLabel, full ? i18n::tr("bar.widgets.lock-keys.num") : i18n::tr("bar.widgets.lock-keys.num-short"),
      numVisible, m_contentScale, labelFontWeight()
  );
  configureLabel(
      m_scrollLabel, full ? i18n::tr("bar.widgets.lock-keys.scroll") : i18n::tr("bar.widgets.lock-keys.scroll-short"),
      scrollVisible, m_contentScale, labelFontWeight()
  );

  if (m_capsLabel != nullptr) {
    m_capsLabel->setColor(
        lockState.capsLock ? colorSpecFromRole(ColorRole::Primary)
                           : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant))
    );
  }
  if (m_numLabel != nullptr) {
    m_numLabel->setColor(
        lockState.numLock ? colorSpecFromRole(ColorRole::Primary)
                          : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant))
    );
  }
  if (m_scrollLabel != nullptr) {
    m_scrollLabel->setColor(
        lockState.scrollLock ? colorSpecFromRole(ColorRole::Primary)
                             : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant))
    );
  }

  if (auto* node = root(); node != nullptr) {
    node->markLayoutDirty();
  }
  requestRedraw();
}
