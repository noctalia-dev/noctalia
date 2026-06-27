#pragma once

#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>
#include <string>

class Flex;
class Label;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
class SelectDropdownPopup;

namespace settings {

  struct SettingsEditorSheetPopupRequest {
    XdgPopupParent parent;
    std::string sheetTitle;
    std::function<void()> removeAction;
    std::function<void(Flex& sheetBody)> populateSheetBody;
    float scale = 1.0f;
  };

  class SettingsEditorSheetPopup final : public DialogPopupHost {
  public:
    SettingsEditorSheetPopup() = default;
    ~SettingsEditorSheetPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void open(SettingsEditorSheetPopupRequest request);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    void onKeyboardEvent(const KeyboardEvent& event);
    [[nodiscard]] wl_surface* wlSurface() const noexcept;
    [[nodiscard]] bool ownsSelectDropdownSurface(wl_surface* surface) const noexcept;
    [[nodiscard]] bool isSelectDropdownOpen() const noexcept;

    void setSheetTitle(std::string title);

    // Re-run the populate callback to rebuild the sheet body in place (e.g. after an edit that
    // changes which controls are shown). Re-measures and resizes the popup. No-op if not open.
    void rebuildBody();

  protected:
    void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
    void layoutSheet(float contentWidth, float contentHeight) override;
    void cancelToFacade() override;
    [[nodiscard]] InputArea* initialFocusArea() override;
    void onSheetClose() override;

  private:
    void dismissOpenSelectDropdown();

    float m_scale = 1.0f;
    std::string m_sheetTitle;
    Label* m_sheetTitleLabel = nullptr;
    std::function<void()> m_removeAction;
    std::function<void(Flex&)> m_populateSheetBody;

    Flex* m_root = nullptr;
    Flex* m_header = nullptr;
    ScrollView* m_scrollView = nullptr;
    ScrollViewState m_scrollState;
    std::uint32_t m_parentWidth = 0;
    std::uint32_t m_parentHeight = 0;

    std::unique_ptr<SelectDropdownPopup> m_selectPopup;
    wl_output* m_parentOutput = nullptr;
  };

} // namespace settings
