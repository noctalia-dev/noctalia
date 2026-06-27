#include "ui/controls/list_editor.h"

#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

  constexpr float kLabelCellWidth = 200.0f;
  constexpr float kFreeformInputWidth = 190.0f;
  constexpr float kItemRowHeight = 26.0f;
  constexpr float kSuggestedAddHeight = 30.0f;
  constexpr float kVerticalGap = 2.0f;

  std::unique_ptr<Label> makeListLabel(std::string_view text, float scale) {
    return ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
  }

} // namespace

ListEditor::ListEditor() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(kVerticalGap);
}

void ListEditor::setItems(std::vector<std::string> items) {
  m_items = std::move(items);
  rebuildRows();
}

void ListEditor::setSuggestedOptions(std::vector<ListEditorOption> options) {
  m_suggestedOptions = std::move(options);
  rebuildRows();
}

void ListEditor::setAddPlaceholder(std::string_view placeholder) {
  m_addPlaceholder = std::string(placeholder);
  rebuildRows();
}

void ListEditor::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  setGap(kVerticalGap * m_scale);
  rebuildRows();
}

void ListEditor::setMaxItems(std::size_t maxItems) {
  m_maxItems = maxItems;
  rebuildRows();
}

void ListEditor::setOnAddRequested(std::function<void(std::string)> callback) {
  m_onAddRequested = std::move(callback);
}

void ListEditor::setOnRemoveRequested(std::function<void(std::size_t)> callback) {
  m_onRemoveRequested = std::move(callback);
}

void ListEditor::setOnMoveRequested(std::function<void(std::size_t, std::size_t)> callback) {
  m_onMoveRequested = std::move(callback);
}

std::string ListEditor::labelForValue(std::string_view value) const {
  for (const auto& opt : m_suggestedOptions) {
    if (opt.value == value) {
      return opt.label;
    }
  }
  return std::string(value);
}

std::vector<ListEditorOption> ListEditor::remainingOptions() const {
  std::vector<ListEditorOption> remaining;
  remaining.reserve(m_suggestedOptions.size());
  for (const auto& opt : m_suggestedOptions) {
    if (!std::ranges::contains(m_items, opt.value)) {
      remaining.push_back(opt);
    }
  }
  return remaining;
}

void ListEditor::rebuildRows() {
  while (!children().empty()) {
    removeChild(children().back().get());
  }

  const float labelCellWidth = kLabelCellWidth * m_scale;
  const float itemRowHeight = kItemRowHeight * m_scale;
  const float suggestedAddHeight = kSuggestedAddHeight * m_scale;

  std::unique_ptr<Flex> addRow;
  const bool atCapacity = m_maxItems > 0 && m_items.size() >= m_maxItems;
  if (!atCapacity || !m_suggestedOptions.empty()) {
    addRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });

    if (!m_suggestedOptions.empty()) {
      const auto remaining = remainingOptions();
      if (!remaining.empty()) {
        std::vector<std::string> remainingLabels;
        remainingLabels.reserve(remaining.size());
        for (const auto& opt : remaining) {
          remainingLabels.push_back(opt.label);
        }

        Select* selectPtr = nullptr;
        auto select = ui::select({
            .out = &selectPtr,
            .options = std::move(remainingLabels),
            .placeholder = m_addPlaceholder,
            .fontSize = Style::fontSizeCaption * m_scale,
            .controlHeight = suggestedAddHeight,
            .glyphSize = Style::fontSizeCaption * m_scale,
            .enabled = !atCapacity,
            .width = labelCellWidth,
            .height = suggestedAddHeight,
        });

        auto addBtn = ui::button({
            .glyph = "add",
            .glyphSize = Style::fontSizeCaption * m_scale,
            .enabled = !atCapacity,
            .variant = ButtonVariant::Ghost,
            .minWidth = suggestedAddHeight,
            .minHeight = suggestedAddHeight,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusSm(m_scale),
            .onClick = [this, selectPtr, remaining] {
              const std::size_t index = selectPtr->selectedIndex();
              if (index < remaining.size() && m_onAddRequested) {
                m_onAddRequested(remaining[index].value);
              }
            },
        });

        addRow->addChild(std::move(select));
        addRow->addChild(std::move(addBtn));
      } else {
        addRow.reset();
      }
    } else if (!atCapacity) {
      Input* addInputPtr = nullptr;
      auto addInput = ui::input({
          .out = &addInputPtr,
          .placeholder = m_addPlaceholder,
          .fontSize = Style::fontSizeBody * m_scale,
          .controlHeight = Style::controlHeight * m_scale,
          .horizontalPadding = Style::spaceSm * m_scale,
          .width = kFreeformInputWidth * m_scale,
          .height = Style::controlHeight * m_scale,
          .onSubmit = [this](const std::string& text) {
            if (!text.empty() && m_onAddRequested) {
              m_onAddRequested(text);
            }
          },
      });

      auto addBtn = ui::button({
          .glyph = "add",
          .glyphSize = Style::fontSizeBody * m_scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeight * m_scale,
          .minHeight = Style::controlHeight * m_scale,
          .padding = Style::spaceSm * m_scale,
          .radius = Style::scaledRadiusMd(m_scale),
          .onClick = [this, addInputPtr] {
            const auto& text = addInputPtr->value();
            if (!text.empty() && m_onAddRequested) {
              m_onAddRequested(text);
            }
          },
      });

      addRow->addChild(std::move(addInput));
      addRow->addChild(std::move(addBtn));
    }
  }

  if (m_maxItems > 0) {
    auto capacityLabel = ui::label({
        .text = std::to_string(m_items.size()) + "/" + std::to_string(m_maxItems),
        .fontSize = Style::fontSizeCaption * m_scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });
    if (addRow) {
      addRow->addChild(std::move(capacityLabel));
    } else {
      auto wrapper = ui::row({.align = FlexAlign::Center});
      wrapper->addChild(std::move(capacityLabel));
      addRow = std::move(wrapper);
    }
  }

  if (addRow) {
    addChild(std::move(addRow));
  }

  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto itemRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * m_scale,
        .minHeight = itemRowHeight,
    });

    auto labelCell = ui::row(
        {
            .align = FlexAlign::Center,
            .minWidth = labelCellWidth,
        },
        makeListLabel(labelForValue(m_items[i]), m_scale)
    );
    itemRow->addChild(std::move(labelCell));

    addGhostIconButton(*itemRow, "close", Style::fontSizeCaption * m_scale, [this, i] {
      if (m_onRemoveRequested) {
        m_onRemoveRequested(i);
      }
    });

    if (i > 0) {
      addGhostIconButton(*itemRow, "chevron-up", Style::fontSizeCaption * m_scale, [this, i] {
        if (m_onMoveRequested) {
          m_onMoveRequested(i, i - 1);
        }
      });
    }
    if (i + 1 < m_items.size()) {
      addGhostIconButton(*itemRow, "chevron-down", Style::fontSizeCaption * m_scale, [this, i] {
        if (m_onMoveRequested) {
          m_onMoveRequested(i, i + 1);
        }
      });
    }

    addChild(std::move(itemRow));
  }

  markLayoutDirty();
}

void ListEditor::addGhostIconButton(Flex& row, std::string_view glyph, float size, std::function<void()> callback) {
  row.addChild(
      ui::button({
          .glyph = std::string(glyph),
          .glyphSize = size,
          .variant = ButtonVariant::Ghost,
          .minWidth = kItemRowHeight * m_scale,
          .minHeight = kItemRowHeight * m_scale,
          .padding = Style::spaceXs * m_scale,
          .radius = Style::scaledRadiusSm(m_scale),
          .onClick = std::move(callback),
      })
  );
}
