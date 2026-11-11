#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ListEditorOption {
  std::string value;
  std::string label;
  std::string icon;
};

class ListEditor : public Flex {
public:
  ListEditor();

  void setItems(std::vector<std::string> items);
  void setSuggestedOptions(std::vector<ListEditorOption> options);
  void setAddPlaceholder(std::string_view placeholder);
  void setScale(float scale);
  void setMaxItems(std::size_t maxItems);
  void setOnAddRequested(std::function<void(std::string)> callback);
  void setOnAddPickerRequested(std::function<void()> callback);
  void setOnRemoveRequested(std::function<void(std::size_t)> callback);
  // `to` is an insertion index in [0, size]. No-op when to == from || to == from + 1.
  void setOnMoveRequested(std::function<void(std::size_t from, std::size_t to)> callback);

private:
  const ListEditorOption* optionForValue(std::string_view value) const;
  bool hasRemainingOptions() const;
  void rebuildRows();
  void addGhostIconButton(Flex& row, std::string_view glyph, float size, std::function<void()> callback);

  std::vector<std::string> m_items;
  std::vector<ListEditorOption> m_suggestedOptions;
  std::string m_addPlaceholder;
  std::size_t m_maxItems = 0;
  std::function<void(std::string)> m_onAddRequested;
  std::function<void()> m_onAddPickerRequested;
  std::function<void(std::size_t)> m_onRemoveRequested;
  std::function<void(std::size_t, std::size_t)> m_onMoveRequested;
  float m_scale = 1.0F;
};

// Shared list state for a ListEditor; `add` can also be reused as a picker callback.
struct ListEditorBinding {
  std::shared_ptr<std::vector<std::string>> items;
  std::function<void(std::string)> add;
};

ListEditorBinding wireListEditor(
    ListEditor& editor, std::vector<std::string> items,
    const std::function<void(const std::vector<std::string>&)>& onChanged
);
