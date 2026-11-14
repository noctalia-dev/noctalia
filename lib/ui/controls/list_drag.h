#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Box;
class Button;
class Flex;

// Hidden drop-line indicator for a reorderable list; add it to the list column before the rows.
std::unique_ptr<Box> makeListDropIndicator(float scale, Box** out);

// Drag handle for one row of a reorderable list. `onMove` receives the dragged row index and an
// insertion index in [0, itemNodes.size()].
std::unique_ptr<Button> makeListDragHandle(
    Flex& row, Flex& list, Box& indicator, std::shared_ptr<std::vector<Flex*>> itemNodes, std::size_t itemIndex,
    float scale, const std::string& tooltip, std::function<void(std::size_t from, std::size_t insertion)> onMove
);

// Returns false when `from` is out of range or the move is a no-op
// (`insertion == from` or `insertion == from + 1`).
template <typename T> bool moveListItem(std::vector<T>& items, std::size_t from, std::size_t insertion) {
  if (from >= items.size() || insertion > items.size() || insertion == from || insertion == from + 1) {
    return false;
  }
  auto value = std::move(items[from]);
  items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
  if (insertion > from) {
    --insertion;
  }
  items.insert(items.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(value));
  return true;
}
