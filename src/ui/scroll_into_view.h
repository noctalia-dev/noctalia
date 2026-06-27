#pragma once

#include <cstdint>

class Node;
class ScrollView;

struct ScrollViewState;

void scrollNodeIntoScrollView(ScrollView& scrollView, ScrollViewState* state, const Node& target, float margin);

[[nodiscard]] ScrollView* findEnclosingScrollView(Node* node);
