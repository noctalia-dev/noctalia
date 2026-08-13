#pragma once

#include "shell/settings/settings_sheet.h"
#include "ui/dialogs/dialog_popup_host.h"

#include <memory>

class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
class SelectDropdownPopup;

namespace settings {
  class SettingsSheetPopup final : public DialogPopupHost {
  public:
    SettingsSheetPopup() = default;
    ~SettingsSheetPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void open(SettingsSheetRequest request);
    void close();
    void requestClose();

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

    SettingsSheet m_sheet;
    std::uint32_t m_parentWidth = 0;
    std::uint32_t m_parentHeight = 0;

    std::unique_ptr<SelectDropdownPopup> m_selectPopup;
    wl_output* m_parentOutput = nullptr;
  };

} // namespace settings
