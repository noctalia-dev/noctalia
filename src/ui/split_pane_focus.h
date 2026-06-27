#pragma once

#include <cstdint>

class InputArea;
class InputDispatcher;
class Node;

struct SplitPaneFocusConfig {
  InputArea* sidebarFocus = nullptr;
  const Node* sidebarRoot = nullptr;
  const Node* contentRoot = nullptr;
  InputArea* headerFocus = nullptr;
};

enum class SplitPaneFocusResult : std::uint8_t {
  NotHandled,
  Consumed,
};

[[nodiscard]] bool isNodeInSubtree(const Node* node, const Node* ancestor) noexcept;

[[nodiscard]] SplitPaneFocusResult handleSplitPaneFocusNavigation(
    InputDispatcher& dispatcher, const SplitPaneFocusConfig& config, std::uint32_t sym, std::uint32_t modifiers,
    bool pressed, bool preedit
);
