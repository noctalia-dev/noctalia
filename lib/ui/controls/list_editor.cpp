#include "ui/controls/list_editor.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

  constexpr float kLabelCellWidth = 200.0F;

  struct ListDragState {
    bool active = false;
    bool moved = false;
    float startLocalX = 0.0F;
    float startLocalY = 0.0F;
    std::optional<std::size_t> targetInsertionIndex;
  };

  std::unique_ptr<Label> makeListLabel(std::string_view text, float scale) {
    return ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
  }

  std::size_t insertionIndexForSceneY(float sceneY, const std::vector<Flex*>& itemNodes) {
    for (std::size_t i = 0; i < itemNodes.size(); ++i) {
      const auto* item = itemNodes[i];
      if (item == nullptr) {
        continue;
      }
      float ignoredX = 0.0F;
      float itemY = 0.0F;
      Node::absolutePosition(item, ignoredX, itemY);
      if (sceneY < itemY + item->height() * 0.5F) {
        return i;
      }
    }
    return itemNodes.size();
  }

  void updateDropIndicator(
      Box& indicator, const Flex& list, const std::vector<Flex*>& itemNodes, std::size_t insertionIndex, float scale
  ) {
    if (insertionIndex > itemNodes.size()) {
      indicator.setVisible(false);
      return;
    }

    const float x = Style::spaceXs * scale;
    const float width = std::max(1.0F, list.width() - Style::spaceXs * scale * 2.0F);
    const float gapHalf = Style::spaceXs * scale * 0.5F;
    float y = 0.0F;
    if (!itemNodes.empty()) {
      if (insertionIndex == itemNodes.size()) {
        const auto* target = itemNodes.back();
        y = target != nullptr ? target->y() + target->height() + gapHalf : y;
      } else {
        const auto* target = itemNodes[insertionIndex];
        y = target != nullptr ? target->y() - gapHalf : y;
      }
    }

    indicator.setPosition(x, y);
    indicator.setFrameSize(width, std::max(2.0F, 3.0F * scale));
    indicator.setVisible(true);
  }

} // namespace

ListEditor::ListEditor() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceXs);
}

ListEditorBinding wireListEditor(
    ListEditor& editor, std::vector<std::string> items,
    const std::function<void(const std::vector<std::string>&)>& onChanged
) {
  auto list = std::make_shared<std::vector<std::string>>(std::move(items));
  editor.setItems(*list);

  auto add = [list, onChanged](std::string value) {
    if (value.empty() || std::ranges::contains(*list, value)) {
      return;
    }
    list->push_back(std::move(value));
    onChanged(*list);
  };

  editor.setOnAddRequested(add);
  editor.setOnRemoveRequested([list, onChanged](std::size_t index) {
    if (index >= list->size()) {
      return;
    }
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
    onChanged(*list);
  });
  editor.setOnMoveRequested([list, onChanged](std::size_t from, std::size_t to) {
    if (from >= list->size() || to > list->size() || to == from || to == from + 1) {
      return;
    }
    auto value = std::move((*list)[from]);
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(from));
    if (to > from) {
      --to;
    }
    list->insert(list->begin() + static_cast<std::ptrdiff_t>(to), std::move(value));
    onChanged(*list);
  });

  return {.items = std::move(list), .add = std::move(add)};
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
  m_scale = std::max(0.1F, scale);
  setGap(Style::spaceXs * m_scale);
  rebuildRows();
}

void ListEditor::setMaxItems(std::size_t maxItems) {
  m_maxItems = maxItems;
  rebuildRows();
}

void ListEditor::setOnAddRequested(std::function<void(std::string)> callback) {
  m_onAddRequested = std::move(callback);
}

void ListEditor::setOnAddPickerRequested(std::function<void()> callback) {
  m_onAddPickerRequested = std::move(callback);
}

void ListEditor::setOnRemoveRequested(std::function<void(std::size_t)> callback) {
  m_onRemoveRequested = std::move(callback);
}

void ListEditor::setOnMoveRequested(std::function<void(std::size_t, std::size_t)> callback) {
  m_onMoveRequested = std::move(callback);
}

const ListEditorOption* ListEditor::optionForValue(std::string_view value) const {
  for (const auto& opt : m_suggestedOptions) {
    if (opt.value == value) {
      return &opt;
    }
  }
  return nullptr;
}

bool ListEditor::hasRemainingOptions() const {
  return std::ranges::any_of(m_suggestedOptions, [this](const ListEditorOption& opt) {
    return !std::ranges::contains(m_items, opt.value);
  });
}

void ListEditor::rebuildRows() {
  while (!children().empty()) {
    removeChild(children().back().get());
  }

  const float labelCellWidth = kLabelCellWidth * m_scale;
  const float itemRowHeight = Style::controlHeightSm * m_scale;
  const float iconBtnSize = Style::controlHeightSm * m_scale;

  if (!m_items.empty()) {
    auto itemsColumn = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * m_scale,
        .fillWidth = true,
    });
    auto* itemsColumnPtr = itemsColumn.get();

    Box* dropIndicatorPtr = nullptr;
    auto dropIndicator = ui::box({
        .out = &dropIndicatorPtr,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .radius = std::max(1.0F, 1.5F * m_scale),
        .visible = false,
        .participatesInLayout = false,
        .configure = [](Box& box) { box.setZIndex(10); },
    });
    itemsColumn->addChild(std::move(dropIndicator));

    auto itemNodes = std::make_shared<std::vector<Flex*>>();
    itemNodes->reserve(m_items.size());

    const std::string dragTooltip = i18n::tr("settings.controls.list.drag");

    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto itemRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * m_scale,
          .minHeight = itemRowHeight,
      });
      auto* itemRowPtr = itemRow.get();

      Button* dragBtnPtr = nullptr;
      auto dragBtn = ui::button({
          .out = &dragBtnPtr,
          .glyph = "menu-2",
          .glyphSize = Style::fontSizeCaption * m_scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = dragTooltip,
          .minWidth = iconBtnSize,
          .minHeight = iconBtnSize,
          .padding = Style::spaceXs * m_scale,
          .radius = Style::scaledRadiusSm(m_scale),
          .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
      });

      auto dragState = std::make_shared<ListDragState>();
      dragBtn->setOnPress([this, dragState, dragBtnPtr, dragTooltip, itemRowPtr, dropIndicatorPtr, itemNodes,
                           itemIndex = i](float localX, float localY, bool pressed) {
        if (pressed) {
          dragState->active = true;
          dragState->moved = false;
          dragState->startLocalX = localX;
          dragState->startLocalY = localY;
          dragState->targetInsertionIndex = std::nullopt;
          itemRowPtr->setOpacity(0.72F);
          if (dragBtnPtr != nullptr) {
            dragBtnPtr->setTooltip({});
          }
          if (dropIndicatorPtr != nullptr) {
            dropIndicatorPtr->setVisible(false);
          }
          return;
        }
        if (!dragState->active) {
          return;
        }
        dragState->active = false;
        itemRowPtr->setOpacity(1.0F);
        if (dragBtnPtr != nullptr) {
          dragBtnPtr->setTooltip(dragTooltip);
        }
        if (dropIndicatorPtr != nullptr) {
          dropIndicatorPtr->setVisible(false);
        }
        if (!dragState->moved || !dragState->targetInsertionIndex.has_value() || !m_onMoveRequested) {
          return;
        }
        m_onMoveRequested(itemIndex, *dragState->targetInsertionIndex);
      });
      dragBtn->setOnPointerMotion([dragState, dragBtnPtr, itemsColumnPtr, dropIndicatorPtr, itemNodes, itemIndex = i,
                                   scale = m_scale](float localX, float localY) {
        if (!dragState->active) {
          return;
        }
        if (std::hypot(localX - dragState->startLocalX, localY - dragState->startLocalY)
            >= Style::dragStartThreshold * scale) {
          dragState->moved = true;
        }
        if (!dragState->moved || itemsColumnPtr == nullptr || dropIndicatorPtr == nullptr || itemNodes == nullptr) {
          return;
        }

        float ignoredX = 0.0F;
        float absY = 0.0F;
        Node::absolutePosition(dragBtnPtr, ignoredX, absY);
        const float sceneY = absY + localY;
        const std::size_t insertion = insertionIndexForSceneY(sceneY, *itemNodes);
        if (insertion == itemIndex || insertion == itemIndex + 1) {
          dragState->targetInsertionIndex = std::nullopt;
          dropIndicatorPtr->setVisible(false);
          return;
        }
        dragState->targetInsertionIndex = insertion;
        updateDropIndicator(*dropIndicatorPtr, *itemsColumnPtr, *itemNodes, insertion, scale);
      });

      itemRow->addChild(std::move(dragBtn));

      const ListEditorOption* option = optionForValue(m_items[i]);
      const std::string_view icon = option != nullptr ? std::string_view(option->icon) : std::string_view{};
      if (!icon.empty()) {
        itemRow->addChild(
            ui::glyph({
                .glyph = std::string(icon),
                .glyphSize = Style::fontSizeBody * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            })
        );
      }

      const std::string_view label = option != nullptr ? std::string_view(option->label) : std::string_view(m_items[i]);
      auto labelCell = ui::row(
          {
              .align = FlexAlign::Center,
              .minWidth = labelCellWidth,
              .flexGrow = 1.0F,
          },
          makeListLabel(label, m_scale)
      );
      itemRow->addChild(std::move(labelCell));

      addGhostIconButton(*itemRow, "close", Style::fontSizeCaption * m_scale, [this, i] {
        if (m_onRemoveRequested) {
          m_onRemoveRequested(i);
        }
      });

      itemNodes->push_back(itemRowPtr);
      itemsColumn->addChild(std::move(itemRow));
    }

    addChild(std::move(itemsColumn));
  }

  std::unique_ptr<Flex> addRow;
  const bool atCapacity = m_maxItems > 0 && m_items.size() >= m_maxItems;
  const bool hasRemaining = hasRemainingOptions();
  const std::string capacityBadge =
      m_maxItems > 0 ? std::to_string(m_items.size()) + "/" + std::to_string(m_maxItems) : std::string{};

  if (!m_suggestedOptions.empty()) {
    if (hasRemaining || atCapacity) {
      addRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * m_scale,
          .fillWidth = true,
      });
      addRow->addChild(
          ui::button({
              .text = i18n::tr("settings.controls.list.add-entry"),
              .glyph = "add",
              .fontSize = Style::fontSizeBody * m_scale,
              .glyphSize = Style::fontSizeBody * m_scale,
              .enabled = !atCapacity,
              .variant = ButtonVariant::Default,
              .badge = capacityBadge,
              .minHeight = Style::controlHeight * m_scale,
              .paddingV = Style::spaceSm * m_scale,
              .paddingH = Style::spaceMd * m_scale,
              .radius = Style::scaledRadiusMd(m_scale),
              .flexGrow = 1.0F,
              .onClick = [this] {
                if (m_onAddPickerRequested) {
                  m_onAddPickerRequested();
                }
              },
          })
      );
    }
  } else if (!atCapacity) {
    addRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
        .fillWidth = true,
    });

    Input* addInputPtr = nullptr;
    auto addInput = ui::input({
        .out = &addInputPtr,
        .placeholder = m_addPlaceholder,
        .fontSize = Style::fontSizeBody * m_scale,
        .controlHeight = Style::controlHeight * m_scale,
        .horizontalPadding = Style::spaceSm * m_scale,
        .height = Style::controlHeight * m_scale,
        .flexGrow = 1.0F,
        .onSubmit = [this](const std::string& text) {
          if (!text.empty() && m_onAddRequested) {
            m_onAddRequested(text);
          }
        },
    });

    auto addBtn = ui::button({
        .glyph = "add",
        .glyphSize = Style::fontSizeCaption * m_scale,
        .variant = ButtonVariant::Ghost,
        .badge = capacityBadge,
        .minWidth = iconBtnSize,
        .minHeight = iconBtnSize,
        .padding = Style::spaceXs * m_scale,
        .radius = Style::scaledRadiusSm(m_scale),
        .onClick = [this, addInputPtr] {
          const auto& text = addInputPtr->value();
          if (!text.empty() && m_onAddRequested) {
            m_onAddRequested(text);
          }
        },
    });

    addRow->addChild(std::move(addInput));
    addRow->addChild(std::move(addBtn));
  } else {
    addRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
        .fillWidth = true,
    });
    addRow->addChild(
        ui::button({
            .text = i18n::tr("settings.controls.list.add-entry"),
            .glyph = "add",
            .fontSize = Style::fontSizeBody * m_scale,
            .glyphSize = Style::fontSizeBody * m_scale,
            .enabled = false,
            .variant = ButtonVariant::Default,
            .badge = capacityBadge,
            .minHeight = Style::controlHeight * m_scale,
            .paddingV = Style::spaceSm * m_scale,
            .paddingH = Style::spaceMd * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .flexGrow = 1.0F,
        })
    );
  }

  if (addRow) {
    addChild(std::move(addRow));
  }

  markLayoutDirty();
}

void ListEditor::addGhostIconButton(Flex& row, std::string_view glyph, float size, std::function<void()> callback) {
  const float iconBtnSize = Style::controlHeightSm * m_scale;
  row.addChild(
      ui::button({
          .glyph = std::string(glyph),
          .glyphSize = size,
          .variant = ButtonVariant::Ghost,
          .minWidth = iconBtnSize,
          .minHeight = iconBtnSize,
          .padding = Style::spaceXs * m_scale,
          .radius = Style::scaledRadiusSm(m_scale),
          .onClick = std::move(callback),
      })
  );
}
