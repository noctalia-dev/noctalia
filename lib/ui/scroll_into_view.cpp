#include "ui/scroll_into_view.h"

#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>

void scrollNodeIntoScrollView(ScrollView& scrollView, ScrollViewState* state, const Node& target, float margin) {
  Flex* content = scrollView.content();
  if (content == nullptr) {
    return;
  }

  const bool horizontal = scrollView.orientation() == ScrollOrientation::Horizontal;
  const float viewportExtent = horizontal ? scrollView.contentViewportWidth()
                                          : std::max(0.0F, scrollView.height() - scrollView.viewportPaddingV() * 2.0F);
  if (viewportExtent <= 0.0F) {
    return;
  }

  float targetX = 0.0F;
  float targetY = 0.0F;
  float contentX = 0.0F;
  float contentY = 0.0F;
  Node::absolutePosition(&target, targetX, targetY);
  Node::absolutePosition(content, contentX, contentY);
  const float position = horizontal ? targetX - contentX : targetY - contentY;
  const float targetStart = std::max(0.0F, position - margin);
  const float targetEnd = position + (horizontal ? target.width() : target.height()) + margin;
  const float currentStart = scrollView.scrollOffset();
  const float currentEnd = currentStart + viewportExtent;

  float desiredOffset = currentStart;
  if (targetEnd - targetStart >= viewportExtent || targetStart < currentStart) {
    desiredOffset = targetStart;
  } else if (targetEnd > currentEnd) {
    desiredOffset = targetEnd - viewportExtent;
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
