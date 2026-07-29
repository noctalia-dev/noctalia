#pragma once

#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>
#include <string>

class Flex;
class Label;
class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
class SelectDropdownPopup;

namespace settings {

  struct SettingsSheetPopupRequest {
    XdgPopupParent parent;
    std::string sheetTitle;
    std::function<void()> removeAction;
    std::function<std::unique_ptr<Node>()> createHeaderAction;
    std::function<void(Flex& sheetBody)> populateSheetBody;
    float scale = 1.0f;
    float minWidth = 640.0f;
    float maxWidth = 820.0f;
    float parentFraction = 0.75f;
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

  class SettingsSheetPopup final : public DialogPopupHost {
  public:
    SettingsSheetPopup() = default;
    ~SettingsSheetPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void open(SettingsSheetPopupRequest request);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    void onKeyboardEvent(const KeyboardEvent& event);
    [[nodiscard]] wl_surface* wlSurface() const noexcept;
    [[nodiscard]] bool ownsSelectDropdownSurface(wl_surface* surface) const noexcept;
    [[nodiscard]] bool isSelectDropdownOpen() const noexcept;
    [[nodiscard]] InputArea* focusedArea() noexcept;

    void setSheetTitle(std::string title);
    void setStatusMessage(std::string message, bool error);
    void clearStatusMessage();

    // Re-run the populate callback to rebuild the sheet body in place (e.g. after an edit that
    // changes which controls are shown). Re-measures and resizes the popup. No-op if not open.
    void rebuildBody();

  protected:
    void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
    void layoutSheet(float contentWidth, float contentHeight) override;
    void cancelToFacade() override;
    [[nodiscard]] InputArea* initialFocusArea() override;
    [[nodiscard]] bool preDispatchKeyboard(const KeyboardEvent& event) override;
    void onSheetClose() override;

  private:
    void dismissOpenSelectDropdown();

    // Guard token for deferred callbacks that run on the next main-loop tick.
    // Callbacks capture a weak_ptr so they can detect destruction without
    // relying on a raw this pointer staying valid.
    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

    float m_scale = 1.0f;
    float m_minWidth = 640.0f;
    float m_maxWidth = 820.0f;
    float m_parentFraction = 0.75f;
    bool m_fillParentHeight = false;
    bool m_scrollableBody = true;
    std::function<bool()> m_onCloseRequested;
    std::function<bool(const KeyboardEvent&)> m_preDispatchKeyboard;
    std::string m_sheetTitle;
    Label* m_sheetTitleLabel = nullptr;
    std::string m_statusMessage;
    bool m_statusIsError = false;
    Flex* m_statusBanner = nullptr;
    Label* m_statusLabel = nullptr;
    std::function<void()> m_removeAction;
    std::function<std::unique_ptr<Node>()> m_createHeaderAction;
    std::function<void(Flex&)> m_populateSheetBody;

    Flex* m_root = nullptr;
    Flex* m_header = nullptr;
    // The body content flex, whether or not it is wrapped in m_scrollView. Used by layout to
    // measure the body. m_scrollView is null when scrollableBody is false.
    Flex* m_body = nullptr;
    ScrollView* m_scrollView = nullptr;
    ScrollViewState m_scrollState;
    std::uint32_t m_parentWidth = 0;
    std::uint32_t m_parentHeight = 0;

    std::unique_ptr<SelectDropdownPopup> m_selectPopup;
    wl_output* m_parentOutput = nullptr;
  };

} // namespace settings
