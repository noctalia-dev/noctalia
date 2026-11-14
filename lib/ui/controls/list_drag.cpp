#include "ui/controls/list_drag.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

  struct ListDragState {
    bool active = false;
    bool moved = false;
    float startLocalX = 0.0F;
    float startLocalY = 0.0F;
    std::optional<std::size_t> targetInsertionIndex;
  };

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

  void wireListRowDrag(
      Button& handle, Flex& row, Flex& list, Box& indicator, std::shared_ptr<std::vector<Flex*>> itemNodes,
      std::size_t itemIndex, float scale, std::string tooltip,
      std::function<void(std::size_t from, std::size_t insertion)> onMove
  ) {
    auto dragState = std::make_shared<ListDragState>();
    handle.setOnPress([dragState, handlePtr = &handle, rowPtr = &row, indicatorPtr = &indicator, itemIndex,
                       tooltip = std::move(tooltip),
                       onMove = std::move(onMove)](float localX, float localY, bool pressed) {
      if (pressed) {
        dragState->active = true;
        dragState->moved = false;
        dragState->startLocalX = localX;
        dragState->startLocalY = localY;
        dragState->targetInsertionIndex = std::nullopt;
        rowPtr->setOpacity(0.72F);
        handlePtr->setTooltip({});
        indicatorPtr->setVisible(false);
        return;
      }
      if (!dragState->active) {
        return;
      }
      dragState->active = false;
      rowPtr->setOpacity(1.0F);
      handlePtr->setTooltip(tooltip);
      indicatorPtr->setVisible(false);
      if (!dragState->moved || !dragState->targetInsertionIndex.has_value() || !onMove) {
        return;
      }
      onMove(itemIndex, *dragState->targetInsertionIndex);
    });
    handle.setOnPointerMotion([dragState, handlePtr = &handle, listPtr = &list, indicatorPtr = &indicator,
                               itemNodes = std::move(itemNodes), itemIndex, scale](float localX, float localY) {
      if (!dragState->active) {
        return;
      }
      if (std::hypot(localX - dragState->startLocalX, localY - dragState->startLocalY)
          >= Style::dragStartThreshold * scale) {
        dragState->moved = true;
      }
      if (!dragState->moved || itemNodes == nullptr) {
        return;
      }

      float ignoredX = 0.0F;
      float absY = 0.0F;
      Node::absolutePosition(handlePtr, ignoredX, absY);
      const float sceneY = absY + localY;
      const std::size_t insertion = insertionIndexForSceneY(sceneY, *itemNodes);
      if (insertion == itemIndex || insertion == itemIndex + 1) {
        dragState->targetInsertionIndex = std::nullopt;
        indicatorPtr->setVisible(false);
        return;
      }
      dragState->targetInsertionIndex = insertion;
      updateDropIndicator(*indicatorPtr, *listPtr, *itemNodes, insertion, scale);
    });
  }

} // namespace

std::unique_ptr<Box> makeListDropIndicator(float scale, Box** out) {
  return ui::box({
      .out = out,
      .fill = colorSpecFromRole(ColorRole::Primary),
      .radius = std::max(1.0F, 1.5F * scale),
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Box& box) { box.setZIndex(10); },
  });
}

std::unique_ptr<Button> makeListDragHandle(
    Flex& row, Flex& list, Box& indicator, std::shared_ptr<std::vector<Flex*>> itemNodes, std::size_t itemIndex,
    float scale, const std::string& tooltip, std::function<void(std::size_t from, std::size_t insertion)> onMove
) {
  auto handle = ui::button({
      .glyph = "menu-2",
      .glyphSize = Style::fontSizeCaption * scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = tooltip,
      .minWidth = Style::controlHeightSm * scale,
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusSm(scale),
      .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
  });
  wireListRowDrag(*handle, row, list, indicator, std::move(itemNodes), itemIndex, scale, tooltip, std::move(onMove));
  return handle;
}
