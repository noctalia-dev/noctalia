#include "ui/scroll_into_view.h"

#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>

void scrollNodeIntoScrollView(ScrollView& scrollView, ScrollViewState* state, const Node& target, float margin) {
  Flex* content = scrollView.content();
  if (content == nullptr) {
    return;
  }

  const float viewportHeight = std::max(0.0F, scrollView.height() - scrollView.viewportPaddingV() * 2.0F);
  if (viewportHeight <= 0.0F) {
    return;
  }

  float targetX = 0.0F;
  float targetY = 0.0F;
  float contentX = 0.0F;
  float contentY = 0.0F;
  Node::absolutePosition(&target, targetX, targetY);
  Node::absolutePosition(content, contentX, contentY);
  (void)targetX;
  (void)contentX;

  const float targetTop = std::max(0.0F, targetY - contentY - margin);
  const float targetBottom = targetY - contentY + target.height() + margin;
  const float currentTop = scrollView.scrollOffset();
  const float currentBottom = currentTop + viewportHeight;

  float desiredOffset = currentTop;
  if (targetBottom - targetTop >= viewportHeight) {
    desiredOffset = targetTop;
  } else if (targetTop < currentTop) {
    desiredOffset = targetTop;
  } else if (targetBottom > currentBottom) {
    desiredOffset = targetBottom - viewportHeight;
  }

  scrollView.setScrollOffset(desiredOffset);
  if (state != nullptr) {
    state->offset = scrollView.scrollOffset();
  }
}

void scrollNodeToScrollViewTop(ScrollView& scrollView, const Node& target, float margin) {
  Flex* content = scrollView.content();
  if (content == nullptr) {
    return;
  }

  float targetX = 0.0F;
  float targetY = 0.0F;
  float contentX = 0.0F;
  float contentY = 0.0F;
  Node::absolutePosition(&target, targetX, targetY);
  Node::absolutePosition(content, contentX, contentY);
  (void)targetX;
  (void)contentX;

  scrollView.requestScrollToOffset(std::max(0.0F, targetY - contentY - margin));
}

ScrollView* findEnclosingScrollView(Node* node) {
  for (Node* ancestor = node != nullptr ? node->parent() : nullptr; ancestor != nullptr;
       ancestor = ancestor->parent()) {
    auto* scrollView = dynamic_cast<ScrollView*>(ancestor);
    if (scrollView == nullptr) {
      continue;
    }
    // Only the content() subtree scrolls; the scrollbar and background do not.
    for (const Node* inner = node; inner != scrollView; inner = inner->parent()) {
      if (inner == scrollView->content()) {
        return scrollView;
      }
    }
  }
  return nullptr;
}
