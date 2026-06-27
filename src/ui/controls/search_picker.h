#pragma once

#include "core/timer_manager.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/flex.h"
#include "ui/controls/virtual_list_view.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Input;
class InputArea;
class Label;

struct SearchPickerOption {
  std::string value;
  std::string label;
  std::string description;
  bool enabled = true;
  std::string icon;
  ColorSwatchPreview preview = {};
};

class SearchPicker : public Flex, private VirtualListAdapter {
public:
  SearchPicker();

  void setOptions(std::vector<SearchPickerOption> options);
  void setPlaceholder(std::string_view placeholder);
  void setEmptyText(std::string_view text);
  void setSelectedValue(std::string_view value);
  void setOnActivated(std::function<void(const SearchPickerOption&)> callback);
  void setOnCancel(std::function<void()> callback);

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  [[nodiscard]] const std::string& filter() const noexcept { return m_filter; }
  [[nodiscard]] InputArea* filterInputArea() const noexcept;

private:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  [[nodiscard]] std::size_t itemCount() const override;
  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override;
  [[nodiscard]] bool itemInteractive(std::size_t index) const override;
  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
  [[nodiscard]] std::unique_ptr<Node> createItem() override;
  void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
  void onActivate(std::size_t index) override;
  void applyFilter();
  void setHighlightedVisibleIndex(std::size_t index);
  void moveHighlight(int delta);
  void activateHighlighted();
  void ensureHighlightedVisible();
  void notifyHighlightedChanged(std::size_t previous, std::size_t next);
  [[nodiscard]] double matchScore(const SearchPickerOption& option, std::string_view query) const;

  Input* m_input = nullptr;
  Label* m_emptyLabel = nullptr;
  VirtualListView* m_list = nullptr;
  std::vector<SearchPickerOption> m_options;
  std::vector<std::size_t> m_visible;
  std::string m_filter;
  std::string m_emptyText;
  std::string m_selectedValue;
  std::size_t m_highlightedVisibleIndex = 0;
  std::uint64_t m_revision = 0;
  Timer m_filterDebounceTimer;
  std::function<void(const SearchPickerOption&)> m_onActivated;
  std::function<void()> m_onCancel;
  bool m_enabled = true;
};
