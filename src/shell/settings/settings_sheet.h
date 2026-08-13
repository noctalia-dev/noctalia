#pragma once

#include "ui/controls/scroll_view.h"

#include <functional>
#include <memory>
#include <string>

class Flex;
class InputArea;
class Label;
class Node;
class Renderer;
struct KeyboardEvent;

namespace settings {

  struct SettingsSheetRequest {
    std::string sheetTitle;
    std::function<void()> removeAction;
    std::function<std::unique_ptr<Node>()> createHeaderAction;
    std::function<void(Flex& sheetBody)> populateSheetBody;
    float scale = 1.0F;
    float minWidth = 640.0F;
    float maxWidth = 820.0F;
    float parentFraction = 0.75F;
    bool fillParentHeight = false;
    // When false, the body is placed directly in the sheet without the outer ScrollView. Use this
    // when the body provides its own scrolling (e.g. a VirtualGridView) — nesting it in the sheet
    // scroll would trap the inner scroller. The body is then responsible for fitting/scrolling.
    bool scrollableBody = true;
    // When set, called instead of close(). Return true to consume (prevent close).
    std::function<bool()> onCloseRequested;
    // Optional keyboard pre-dispatch (e.g. plugin-store grid navigation). Return true to consume.
    std::function<bool(const KeyboardEvent&)> preDispatchKeyboard;
  };

  // Surface-independent retained content for a Settings editor sheet. Hosts own the surrounding
  // surface or scene layer and provide close/layout/dropdown callbacks.
  class SettingsSheet {
  public:
    void configure(SettingsSheetRequest request);
    [[nodiscard]] std::unique_ptr<Node>
    build(std::function<void()> closeAction, std::function<void()> dismissSelectDropdown);
    void clear();

    void requestClose();
    void setTitle(std::string title);
    void setStatusMessage(std::string message, bool error);
    void clearStatusMessage();

    [[nodiscard]] bool preDispatchKeyboard(const KeyboardEvent& event) const;
    [[nodiscard]] float naturalHeight(Renderer& renderer, float width) const;
    void arrange(Renderer& renderer, float width, float height);

    [[nodiscard]] float scale() const noexcept { return m_scale; }
    [[nodiscard]] float minWidth() const noexcept { return m_minWidth; }
    [[nodiscard]] float maxWidth() const noexcept { return m_maxWidth; }
    [[nodiscard]] float parentFraction() const noexcept { return m_parentFraction; }
    [[nodiscard]] bool fillParentHeight() const noexcept { return m_fillParentHeight; }

  private:
    void clearNodePointers();

    float m_scale = 1.0F;
    float m_minWidth = 640.0F;
    float m_maxWidth = 820.0F;
    float m_parentFraction = 0.75F;
    bool m_fillParentHeight = false;
    bool m_scrollableBody = true;
    std::function<bool()> m_onCloseRequested;
    std::function<bool(const KeyboardEvent&)> m_preDispatchKeyboard;
    std::string m_title;
    Label* m_titleLabel = nullptr;
    std::string m_statusMessage;
    bool m_statusIsError = false;
    Flex* m_statusBanner = nullptr;
    Label* m_statusLabel = nullptr;
    std::function<void()> m_removeAction;
    std::function<std::unique_ptr<Node>()> m_createHeaderAction;
    std::function<void(Flex&)> m_populateBody;
    std::function<void()> m_closeAction;
    std::function<void()> m_dismissSelectDropdown;

    Flex* m_root = nullptr;
    Flex* m_header = nullptr;
    Flex* m_body = nullptr;
    ScrollView* m_scrollView = nullptr;
    ScrollViewState m_scrollState;
  };

} // namespace settings
