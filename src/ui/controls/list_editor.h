#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ListEditorOption {
  std::string value;
  std::string label;
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
  void setOnRemoveRequested(std::function<void(std::size_t)> callback);
  void setOnMoveRequested(std::function<void(std::size_t, std::size_t)> callback);

  [[nodiscard]] const std::vector<std::string>& items() const noexcept { return m_items; }
  [[nodiscard]] const std::vector<ListEditorOption>& suggestedOptions() const noexcept { return m_suggestedOptions; }

private:
  [[nodiscard]] std::string labelForValue(std::string_view value) const;
  [[nodiscard]] std::vector<ListEditorOption> remainingOptions() const;
  void rebuildRows();
  void addGhostIconButton(Flex& row, std::string_view glyph, float size, std::function<void()> callback);

  std::vector<std::string> m_items;
  std::vector<ListEditorOption> m_suggestedOptions;
  std::string m_addPlaceholder;
  std::size_t m_maxItems = 0;
  std::function<void(std::string)> m_onAddRequested;
  std::function<void(std::size_t)> m_onRemoveRequested;
  std::function<void(std::size_t, std::size_t)> m_onMoveRequested;
  float m_scale = 1.0F;
};
