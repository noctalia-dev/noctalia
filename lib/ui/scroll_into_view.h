#pragma once

#include <cstdint>

class Node;
class ScrollView;

struct ScrollViewState;

void scrollNodeIntoScrollView(ScrollView& scrollView, ScrollViewState* state, const Node& target, float margin);
void scrollNodeToScrollViewTop(ScrollView& scrollView, const Node& target, float margin);

ScrollView* findEnclosingScrollView(Node* node);
