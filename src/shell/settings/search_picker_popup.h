#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"

#include <functional>
#include <string>
#include <vector>

class ConfigService;
class Flex;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
struct xdg_surface;

namespace settings {

  class SearchPickerPopup final : public DialogPopupHost {
  public:
    using SelectCallback = std::function<void(const std::string& value)>;

    SearchPickerPopup() = default;
    ~SearchPickerPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void setOnSelect(SelectCallback callback);
    void setOnDismissed(std::function<void()> callback);

    void open(
        xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
        std::uint32_t parentWidth, std::uint32_t parentHeight, const std::string& title,
        const std::vector<SearchPickerOption>& options, const std::string& selectedValue,
        const std::string& placeholder, const std::string& emptyText, float scale
    );
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    void onKeyboardEvent(const KeyboardEvent& event);
    [[nodiscard]] wl_surface* wlSurface() const noexcept;

  protected:
    void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
    void layoutSheet(float contentWidth, float contentHeight) override;
    void cancelToFacade() override;
    [[nodiscard]] InputArea* initialFocusArea() override;
    void onSheetClose() override;

  private:
    float m_scale = 1.0f;
    std::string m_title;
    std::vector<SearchPickerOption> m_options;
    std::string m_selectedValue;
    std::string m_placeholder;
    std::string m_emptyText;

    Flex* m_root = nullptr;
    SearchPicker* m_searchPicker = nullptr;

    SelectCallback m_onSelect;
    std::function<void()> m_onDismissed;
  };

} // namespace settings
