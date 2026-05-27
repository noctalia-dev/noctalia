#include "ui/controls/context_menu.h"

#include "core/ui_phase.h"
#include "render/core/renderer.h"
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

float ContextMenuControl::preferredHeight() const { return preferredHeight(m_entries, m_maxVisible); }

float ContextMenuControl::preferredHeight(const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible) {
  const std::size_t visibleEntries = std::min(entries.size(), std::max<std::size_t>(1, maxVisible));
  if (visibleEntries == 0) {
    return kMenuPadding * 2.0f;
  }

  float contentHeight = 0.0f;
  for (std::size_t i = 0; i < visibleEntries; ++i) {
    contentHeight += entries[i].separator ? kSeparatorHeight : kItemHeight;
  }
  return (kMenuPadding * 2.0f) + contentHeight + (kItemGap * static_cast<float>(visibleEntries - 1));
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
            bg.setCardStyle();
            bg.setRadius(Style::scaledRadiusLg());
            bg.setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
            bg.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
            bg.setFrameSize(width(), height());
          },
      })
  );

  rebuildRows(renderer);
  m_needsRebuild = false;
}

void ContextMenuControl::rebuildRows(Renderer& renderer) {
  const std::size_t visibleItems = std::min(m_entries.size(), m_maxVisible);
  const float rowWidth = width() - (kMenuPadding * 2.0f);
  float currentY = kMenuPadding;

  for (std::size_t i = 0; i < visibleItems; ++i) {
    const ContextMenuControlEntry& entry = m_entries[i];
    const bool interactive = entry.enabled && !entry.separator;
    const bool separator = entry.separator;
    const float rowHeight = separator ? kSeparatorHeight : kItemHeight;

    auto row = std::make_unique<InputArea>();
    row->setFrameSize(rowWidth, rowHeight);
    row->setPosition(kMenuPadding, currentY);
    row->setEnabled(interactive);

    Box* rowBgPtr = nullptr;
    Label* labelPtr = nullptr;
    Glyph* togglePtr = nullptr;
    Glyph* chevronPtr = nullptr;

    const float rowCenterY = currentY + (rowHeight * 0.5f);
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
              .radius = Style::scaledRadiusSm(),
              .width = rowWidth,
              .height = rowHeight,
          })
      );

      const bool toggleVisible = hasToggle(entry);
      const float toggleSlot = toggleVisible ? 22.0f : 0.0f;
      const std::string toggleGlyph = toggleGlyphName(entry);
      if (!toggleGlyph.empty()) {
        auto glyph = ui::glyph({
            .out = &togglePtr,
            .glyph = toggleGlyph,
            .glyphSize = Style::fontSizeBody - 1.0f,
            .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
        });
        glyph->measure(renderer);
        glyph->setPosition(8.0f, (rowHeight - glyph->height()) * 0.5f);
        row->addChild(std::move(glyph));
      }

      auto label = ui::label({
          .out = &labelPtr,
          .text = entry.label,
          .fontSize = Style::fontSizeBody,
          .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
          .maxWidth = entry.hasSubmenu ? (rowWidth - 30.0f - toggleSlot) : (rowWidth - 16.0f - toggleSlot),
      });
      label->measure(renderer);
      label->setPosition(8.0f + toggleSlot, (rowHeight - label->height()) * 0.5f);
      row->addChild(std::move(label));

      if (entry.hasSubmenu) {
        auto chevron = ui::glyph({
            .out = &chevronPtr,
            .glyph = m_submenuDirection == ContextSubmenuDirection::Right ? "chevron-right" : "chevron-left",
            .glyphSize = Style::fontSizeBody - 1.0f,
            .color = entry.enabled ? enabledItemColor() : disabledItemColor(),
        });
        chevron->measure(renderer);
        chevron->setPosition(rowWidth - 8.0f - chevron->width(), (rowHeight - chevron->height()) * 0.5f);
        row->addChild(std::move(chevron));
      }
    } else {
      row->addChild(
          ui::box({
              .out = &rowBgPtr,
              .fill = clearColorSpec(),
              .radius = Style::scaledRadiusSm(),
              .width = rowWidth,
              .height = rowHeight,
          })
      );

      row->addChild(
          ui::label({
              .out = &labelPtr,
              .text = "",
              .fontSize = Style::fontSizeBody,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      row->addChild(
          ui::separator({
              .orientation = SeparatorOrientation::HorizontalRule,
              .width = rowWidth,
              .height = 1.0f,
              .configure = [rowHeight](Separator& sep) { sep.setPosition(0.0f, (rowHeight - 1.0f) * 0.5f); },
          })
      );
    }

    if (rowBgPtr != nullptr && labelPtr != nullptr) {
      const auto applyRowState = [rowBgPtr, labelPtr, togglePtr, chevronPtr, interactive, separator](bool highlighted) {
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

      row->setOnEnter([this, applyRowState](const InputArea::PointerData& /*data*/) {
        applyRowState(true);
        if (m_redrawCallback) {
          m_redrawCallback();
        }
      });
      row->setOnLeave([this, applyRowState]() {
        applyRowState(false);
        if (m_redrawCallback) {
          m_redrawCallback();
        }
      });
      row->setOnPress([this, applyRowState, interactive](const InputArea::PointerData& /*data*/) {
        if (!interactive) {
          return;
        }
        applyRowState(true);
        if (m_redrawCallback) {
          m_redrawCallback();
        }
      });
    }

    addChild(std::move(row));
    currentY += rowHeight + kItemGap;
  }
}
