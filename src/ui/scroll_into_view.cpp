#include "ui/scroll_into_view.h"

#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>

void scrollNodeIntoScrollView(ScrollView& scrollView, ScrollViewState* state, const Node& target, float margin) {
  Flex* content = scrollView.content();
  if (content == nullptr) {
    return;
  }

  const float viewportHeight = std::max(0.0f, scrollView.height() - scrollView.viewportPaddingV() * 2.0f);
  if (viewportHeight <= 0.0f) {
    return;
  }

  float targetX = 0.0f;
  float targetY = 0.0f;
  float contentX = 0.0f;
  float contentY = 0.0f;
  Node::absolutePosition(&target, targetX, targetY);
  Node::absolutePosition(content, contentX, contentY);
  (void)targetX;
  (void)contentX;

  const float targetTop = std::max(0.0f, targetY - contentY - margin);
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

ScrollView* findEnclosingScrollView(Node* node) {
  for (Node* current = node; current != nullptr; current = current->parent()) {
    Node* parent = current->parent();
    if (parent == nullptr) {
      continue;
    }
    if (auto* scrollView = dynamic_cast<ScrollView*>(parent)) {
      if (scrollView->content() == current) {
        return scrollView;
      }
    }
  }
  return nullptr;
}
