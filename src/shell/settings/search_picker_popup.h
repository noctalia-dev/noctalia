#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class Flex;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;

namespace settings {

  struct SearchPickerPopupRequest {
    XdgPopupParent parent;
    std::string title;
    std::vector<SearchPickerOption> options;
    std::string selectedValue;
    std::string placeholder;
    std::string emptyText;
    float scale = 1.0f;
  };

  class SearchPickerPopup final : public DialogPopupHost {
  public:
    using SelectCallback = std::function<void(const std::string& value)>;

    SearchPickerPopup() = default;
    ~SearchPickerPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void setOnSelect(SelectCallback callback);
    void setOnDismissed(std::function<void()> callback);

    void open(SearchPickerPopupRequest request);
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
    // Guard token for deferred callbacks that run on the next main-loop tick.
    // Callbacks capture a weak_ptr so they can detect destruction without
    // relying on a raw this pointer staying valid.
    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

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
