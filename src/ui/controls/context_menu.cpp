#include "ui/controls/context_menu.h"

#include "core/ui_phase.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <linux/input-event-codes.h>

namespace {

  constexpr float kMenuPadding = 6.0f;
  constexpr float kItemHeight = Style::controlHeightSm;
  constexpr float kSeparatorHeight = 10.0f;
  constexpr float kItemGap = 0.0f;
  constexpr float kMenuFontSize = Style::fontSizeCaption;
  constexpr float kMenuGlyphSize = Style::fontSizeCaption - 1.0f;

  float safeScale(float scale) noexcept { return std::max(0.1f, scale); }

  ColorSpec enabledItemColor() { return colorSpecFromRole(ColorRole::OnSurface); }

  ColorSpec disabledItemColor() { return colorSpecFromRole(ColorRole::OnSurface, 0.55f); }

  bool hasToggle(const ContextMenuControlEntry& entry) { return entry.checkmark || entry.radio; }

  std::string toggleGlyphName(const ContextMenuControlEntry& entry) {
    if (entry.toggleState == 2) {
      return "minus";
    }
    if (entry.radio) {
      return entry.toggleState == 1 ? "circle-dot" : "circle";
    }
    return entry.toggleState == 1 ? "check" : "";
  }

} // namespace

ContextMenuControl::ContextMenuControl() : Node(NodeType::Base) {}

void ContextMenuControl::setEntries(std::vector<ContextMenuControlEntry> entries) {
  m_entries = std::move(entries);
  m_highlightedIndex = firstInteractiveIndex();
  m_needsRebuild = true;
  markLayoutDirty();
}

void ContextMenuControl::setMaxVisible(std::size_t maxVisible) {
  m_maxVisible = std::max<std::size_t>(1, maxVisible);
  m_needsRebuild = true;
  markLayoutDirty();
}

void ContextMenuControl::setMenuWidth(float width) {
  m_menuWidth = std::max(1.0f, width);
  m_needsRebuild = true;
  markLayoutDirty();
}

void ContextMenuControl::setContentScale(float scale) {
  const float clamped = safeScale(scale);
  if (m_contentScale == clamped) {
    return;
  }
  m_contentScale = clamped;
  m_needsRebuild = true;
  markLayoutDirty();
}

void ContextMenuControl::setSubmenuDirection(ContextSubmenuDirection direction) {
  m_submenuDirection = direction;
  m_needsRebuild = true;
  markLayoutDirty();
}

void ContextMenuControl::setOnActivate(std::function<void(const ContextMenuControlEntry&)> onActivate) {
  m_onActivate = std::move(onActivate);
}

void ContextMenuControl::setOnSubmenuOpen(
    std::function<void(const ContextMenuControlEntry&, float rowCenterY)> onSubmenuOpen
) {
  m_onSubmenuOpen = std::move(onSubmenuOpen);
}

void ContextMenuControl::setRedrawCallback(std::function<void()> redrawCallback) {
  m_redrawCallback = std::move(redrawCallback);
}

std::size_t ContextMenuControl::firstInteractiveIndex() const noexcept {
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (m_entries[i].enabled && !m_entries[i].separator) {
      return i;
    }
  }
  return 0;
}

void ContextMenuControl::applyHighlightVisuals() {
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if (m_rows[i].apply) {
      m_rows[i].apply(i == m_highlightedIndex && m_rows[i].interactive);
    }
  }
}

void ContextMenuControl::setHighlightedIndex(std::size_t index) {
  if (m_entries.empty()) {
    m_highlightedIndex = 0;
    applyHighlightVisuals();
    return;
  }
  m_highlightedIndex = std::min(index, m_entries.size() - 1);
  if (m_highlightedIndex < m_rows.size() && !m_rows[m_highlightedIndex].interactive) {
    if (!moveHighlight(1) && !moveHighlight(-1)) {
      applyHighlightVisuals();
    }
    return;
  }
  applyHighlightVisuals();
  if (m_redrawCallback) {
    m_redrawCallback();
  }
}

bool ContextMenuControl::moveHighlight(int delta) {
  if (m_rows.empty() || delta == 0) {
    return false;
  }
  const std::size_t count = m_rows.size();
  std::size_t idx = m_highlightedIndex < count ? m_highlightedIndex : 0;
  for (std::size_t step = 0; step < count; ++step) {
    if (delta > 0) {
      idx = (idx + 1) % count;
    } else {
      idx = (idx + count - 1) % count;
    }
    if (m_rows[idx].interactive) {
      m_highlightedIndex = idx;
      applyHighlightVisuals();
      if (m_redrawCallback) {
        m_redrawCallback();
      }
      return true;
    }
  }
  return false;
}

bool ContextMenuControl::activateHighlighted() {
  if (m_highlightedIndex >= m_entries.size() || m_highlightedIndex >= m_rows.size()) {
    return false;
  }
  const ContextMenuControlEntry& entry = m_entries[m_highlightedIndex];
  if (!entry.enabled || entry.separator) {
    return false;
  }
  if (entry.hasSubmenu) {
    if (m_onSubmenuOpen) {
      const float centerY = m_rows[m_highlightedIndex].y + m_rows[m_highlightedIndex].height * 0.5f;
      m_onSubmenuOpen(entry, centerY);
    }
    return true;
  }
  if (m_onActivate) {
    m_onActivate(entry);
  }
  return true;
}

float ContextMenuControl::rowTop(std::size_t index) const noexcept {
  return index < m_rows.size() ? m_rows[index].y : 0.0f;
}

float ContextMenuControl::rowBottom(std::size_t index) const noexcept {
  if (index >= m_rows.size()) {
    return 0.0f;
  }
  return m_rows[index].y + m_rows[index].height;
}

float ContextMenuControl::preferredHeight() const { return preferredHeight(m_entries, m_maxVisible, m_contentScale); }

float ContextMenuControl::preferredHeight(
    const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible, float scale
) {
  scale = safeScale(scale);
  const std::size_t visibleEntries = std::min(entries.size(), std::max<std::size_t>(1, maxVisible));
  if (visibleEntries == 0) {
    return kMenuPadding * scale * 2.0f;
  }

  float contentHeight = 0.0f;
  for (std::size_t i = 0; i < visibleEntries; ++i) {
    contentHeight += (entries[i].separator ? kSeparatorHeight : kItemHeight) * scale;
  }
  return kMenuPadding * scale * 2.0f + contentHeight + kItemGap * scale * static_cast<float>(visibleEntries - 1);
}

void ContextMenuControl::doLayout(Renderer& renderer) {
  if (m_needsRebuild) {
    rebuild(renderer);
  }
  Node::doLayout(renderer);
}

void ContextMenuControl::rebuild(Renderer& renderer) {
  uiAssertNotRendering("ContextMenuControl::rebuild");
  while (!children().empty()) {
    removeChild(children().back().get());
  }

  setSize(m_menuWidth, preferredHeight());

  addChild(
      ui::box({
          .configure = [this](Box& bg) {
            bg.setCardStyle(m_contentScale, 1.0f, Style::popupBordersEnabled());
            bg.setRadius(Style::scaledRadiusLg(m_contentScale));
            bg.setFrameSize(width(), height());
          },
      })
  );

  rebuildRows(renderer);
  m_needsRebuild = false;
}

void ContextMenuControl::rebuildRows(Renderer& renderer) {
  const float scale = m_contentScale;
  const float menuPadding = kMenuPadding * scale;
  const float itemHeight = kItemHeight * scale;
  const float separatorHeight = kSeparatorHeight * scale;
  const float itemGap = kItemGap * scale;
  const std::size_t visibleItems = std::min(m_entries.size(), m_maxVisible);
  const float rowWidth = width() - menuPadding * 2.0f;
  // Concentric with the container: the highlight is inset by menuPadding, so its
  // radius tracks the container radius minus that inset at any corner roundness.
  const float highlightRadius = std::max(0.0f, Style::scaledRadiusLg(scale) - menuPadding);
  float currentY = menuPadding;
  m_rows.clear();
  m_rows.reserve(visibleItems);

  for (std::size_t i = 0; i < visibleItems; ++i) {
    const ContextMenuControlEntry& entry = m_entries[i];
    const bool interactive = entry.enabled && !entry.separator;
    const bool separator = entry.separator;
    const float rowHeight = separator ? separatorHeight : itemHeight;

    auto row = std::make_unique<InputArea>();
    row->setFrameSize(rowWidth, rowHeight);
    row->setPosition(menuPadding, currentY);
    row->setEnabled(interactive);

    Box* rowBgPtr = nullptr;
    Label* labelPtr = nullptr;
    Glyph* togglePtr = nullptr;
    Glyph* chevronPtr = nullptr;

    const float rowCenterY = currentY + rowHeight * 0.5f;
    row->setOnClick([this, entry, rowCenterY](const InputArea::PointerData& data) {
      if (!entry.enabled || entry.separator || data.button != BTN_LEFT) {
        return;
      }
      if (entry.hasSubmenu) {
        if (m_onSubmenuOpen) {
          m_onSubmenuOpen(entry, rowCenterY);
        }
      } else {
        if (m_onActivate) {
          m_onActivate(entry);
        }
      }
    });

    if (!entry.separator) {
      row->addChild(
          ui::box({
              .out = &rowBgPtr,
              .fill = clearColorSpec(),
              .radius = highlightRadius,
              .width = rowWidth,
              .height = rowHeight,
          })
      );

      const bool toggleVisible = hasToggle(entry);
      const float toggleSlot = toggleVisible ? 22.0f * scale : 0.0f;
      const std::string toggleGlyph = toggleGlyphName(entry);
      if (!toggleGlyph.empty()) {
        auto glyph = ui::glyph({
            .out = &togglePtr,
            .glyph = toggleGlyph,
            .glyphSize = kMenuGlyphSize * scale,
            .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
        });
        glyph->measure(renderer);
        glyph->setPosition(8.0f * scale, (rowHeight - glyph->height()) * 0.5f);
        row->addChild(std::move(glyph));
      }

      auto label = ui::label({
          .out = &labelPtr,
          .text = entry.label,
          .fontSize = kMenuFontSize * scale,
          .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
          .maxWidth =
              entry.hasSubmenu ? (rowWidth - 30.0f * scale - toggleSlot) : (rowWidth - 16.0f * scale - toggleSlot),
          .maxLines = 1,
          .ellipsize = entry.ellipsize,
      });
      label->measure(renderer);
      label->setPosition(8.0f * scale + toggleSlot, (rowHeight - label->height()) * 0.5f);
      row->addChild(std::move(label));

      if (entry.hasSubmenu) {
        auto chevron = ui::glyph({
            .out = &chevronPtr,
            .glyph = m_submenuDirection == ContextSubmenuDirection::Right ? "chevron-right" : "chevron-left",
            .glyphSize = kMenuGlyphSize * scale,
            .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
        });
        chevron->measure(renderer);
        chevron->setPosition(rowWidth - 8.0f * scale - chevron->width(), (rowHeight - chevron->height()) * 0.5f);
        row->addChild(std::move(chevron));
      }
    } else {
      row->addChild(
          ui::box({
              .out = &rowBgPtr,
              .fill = clearColorSpec(),
              .radius = highlightRadius,
              .width = rowWidth,
              .height = rowHeight,
          })
      );

      row->addChild(
          ui::label({
              .out = &labelPtr,
              .text = "",
              .fontSize = kMenuFontSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      const float separatorThickness = std::max(1.0f, scale);

      row->addChild(
          ui::separator({
              .orientation = SeparatorOrientation::HorizontalRule,
              .width = rowWidth,
              .height = separatorThickness,
              .configure = [rowHeight, separatorThickness](Separator& sep) {
                sep.setThickness(separatorThickness);
                sep.setPosition(0.0f, (rowHeight - separatorThickness) * 0.5f);
              },
          })
      );
    }

    RowVisual visual{
        .y = currentY,
        .height = rowHeight,
        .interactive = interactive,
    };
    if (rowBgPtr != nullptr && labelPtr != nullptr) {
      visual.apply = [rowBgPtr, labelPtr, togglePtr, chevronPtr, interactive, separator](bool highlighted) {
        rowBgPtr->setFill(highlighted ? colorSpecFromRole(ColorRole::Hover) : clearColorSpec());
        if (separator) {
          labelPtr->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        } else {
          labelPtr->setColor(
              highlighted ? colorSpecFromRole(ColorRole::OnHover)
                          : (interactive ? enabledItemColor() : disabledItemColor())
          );
        }
        if (togglePtr != nullptr) {
          togglePtr->setColor(
              highlighted ? colorSpecFromRole(ColorRole::OnHover)
                          : (interactive ? enabledItemColor() : disabledItemColor())
          );
        }
        if (chevronPtr != nullptr) {
          chevronPtr->setColor(
              highlighted ? colorSpecFromRole(ColorRole::OnHover)
                          : (interactive ? enabledItemColor() : disabledItemColor())
          );
        }
      };

      row->setOnEnter([this, i](const InputArea::PointerData& /*data*/) { setHighlightedIndex(i); });
      row->setOnLeave([this]() {
        applyHighlightVisuals();
        if (m_redrawCallback) {
          m_redrawCallback();
        }
      });
      row->setOnPress([this, i, interactive](const InputArea::PointerData& /*data*/) {
        if (!interactive) {
          return;
        }
        setHighlightedIndex(i);
      });
    }
    m_rows.push_back(std::move(visual));

    addChild(std::move(row));
    currentY += rowHeight + itemGap;
  }

  if (m_highlightedIndex >= m_rows.size()) {
    m_highlightedIndex = firstInteractiveIndex();
  }
  applyHighlightVisuals();
}
