#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Button;
class InputArea;
class Node;

enum class RovingListNavAxis : std::uint8_t {
  Vertical,
  Horizontal,
};

enum class RovingListNavMode : std::uint8_t {
  Roving,
  FollowFocus,
};

class RovingListNavController {
public:
  struct Options {
    RovingListNavAxis axis = RovingListNavAxis::Vertical;
    RovingListNavMode mode = RovingListNavMode::Roving;
    bool keepItemsInTabOrder = false;
    bool wrap = false;
    std::function<void(const Node*)> scrollIntoView;
    std::function<std::size_t()> syncIndexFromSelection;
  };

  void setOptions(Options options);
  void bindFocusArea(InputArea* area);
  void registerItem(Button* button, std::function<void()> activate);
  void clearItems();
  void notifyExternalSelectionChanged();

  void onFocusGain();
  void onFocusLoss();
  void handleKey(std::uint32_t sym, std::uint32_t modifiers);
  void layoutOverlay(float width, float height);

private:
  struct Item {
    Button* button = nullptr;
    std::function<void()> activate;
  };

  [[nodiscard]] bool isNavigable(const Item& item) const;
  [[nodiscard]] std::optional<std::size_t> firstNavigableIndex() const;
  [[nodiscard]] std::optional<std::size_t> findNavigableIndex(std::size_t from, int direction) const;
  void syncKeyboardIndexToSelection();
  void setKeyboardIndex(std::size_t index, bool scrollIntoView);
  void applyKeyboardHints();
  void clearKeyboardHints();
  void scrollKeyboardItemIntoView();
  void moveBy(int direction);
  void activateKeyboardIndex();

  Options m_options;
  InputArea* m_focusArea = nullptr;
  std::vector<Item> m_items;
  std::size_t m_keyboardIndex = 0;
};

class RovingListNavHost : public Flex {
public:
  explicit RovingListNavHost(RovingListNavController::Options options);

  void setTabFocusKey(std::string key);
  void registerItem(Button* button, std::function<void()> activate);
  void clearItems();
  void notifyExternalSelectionChanged();

  [[nodiscard]] InputArea* focusArea() const noexcept { return m_focusArea; }
  [[nodiscard]] RovingListNavController& controller() noexcept { return m_controller; }

  void doLayout(Renderer& renderer) override;

private:
  RovingListNavController m_controller;
  InputArea* m_focusArea = nullptr;
};
