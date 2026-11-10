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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

  constexpr float kLabelCellWidth = 200.0F;
  constexpr float kFreeformInputWidth = 190.0F;
  constexpr float kItemRowHeight = 26.0F;
  constexpr float kSuggestedAddHeight = 30.0F;
  constexpr float kVerticalGap = 2.0F;

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
        .fontSize = Style::fontSizeCaption * scale,
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
    const float gapHalf = kVerticalGap * scale * 0.5F;
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
  m_scale = std::max(0.1F, scale);
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

void ListEditor::setOnAddPickerRequested(std::function<void()> callback) {
  m_onAddPickerRequested = std::move(callback);
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

std::string ListEditor::iconForValue(std::string_view value) const {
  for (const auto& opt : m_suggestedOptions) {
    if (opt.value == value) {
      return opt.icon;
    }
  }
  return {};
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
      if (!remainingOptions().empty()) {
        addRow->addChild(
            ui::button({
                .text = m_addPlaceholder,
                .fontSize = Style::fontSizeCaption * m_scale,
                .enabled = !atCapacity,
                .contentAlign = ButtonContentAlign::Start,
                .variant = ButtonVariant::Default,
                .minWidth = labelCellWidth,
                .minHeight = suggestedAddHeight,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceSm * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this] {
                  if (m_onAddPickerRequested) {
                    m_onAddPickerRequested();
                  }
                },
            })
        );
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

  if (!m_items.empty()) {
    auto itemsColumn = ui::column({
        .align = FlexAlign::Stretch,
        .gap = kVerticalGap * m_scale,
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

    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto itemRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * m_scale,
          .minHeight = itemRowHeight,
      });
      auto* itemRowPtr = itemRow.get();

      Button* dragBtnPtr = nullptr;
      auto dragBtn = ui::button({
          .out = &dragBtnPtr,
          .glyph = "menu-2",
          .glyphSize = Style::fontSizeCaption * m_scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("settings.controls.list.drag"),
          .minWidth = kItemRowHeight * m_scale,
          .minHeight = kItemRowHeight * m_scale,
          .padding = Style::spaceXs * m_scale,
          .radius = Style::scaledRadiusSm(m_scale),
          .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
      });

      auto dragState = std::make_shared<ListDragState>();
      dragBtn->setOnPress([this, dragState, itemRowPtr, dropIndicatorPtr, itemNodes,
                           itemIndex = i](float localX, float localY, bool pressed) {
        if (pressed) {
          dragState->active = true;
          dragState->moved = false;
          dragState->startLocalX = localX;
          dragState->startLocalY = localY;
          dragState->targetInsertionIndex = std::nullopt;
          itemRowPtr->setOpacity(0.72F);
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

      if (const std::string icon = iconForValue(m_items[i]); !icon.empty()) {
        itemRow->addChild(
            ui::glyph({
                .glyph = icon,
                .glyphSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            })
        );
      }

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

      itemNodes->push_back(itemRowPtr);
      itemsColumn->addChild(std::move(itemRow));
    }

    addChild(std::move(itemsColumn));
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
