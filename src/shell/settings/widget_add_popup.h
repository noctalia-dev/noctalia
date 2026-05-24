#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Node;
class ConfigService;
class RenderContext;
class WaylandConnection;
class Flex;
class Input;
class Label;
struct Config;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
struct wl_output;
struct xdg_surface;

namespace settings {

  class WidgetAddPopup final : public DialogPopupHost {
  public:
    using SelectCallback = std::function<void(
        const std::vector<std::string>& lanePath, const std::string& value, const std::string& newInstanceType,
        const std::string& newInstanceId, const std::vector<std::pair<std::string, std::string>>& initialSettings
    )>;

    WidgetAddPopup() = default;
    ~WidgetAddPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void setOnSelect(SelectCallback callback);
    void setOnDismissed(std::function<void()> callback);

    void open(
        xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
        std::uint32_t parentWidth, std::uint32_t parentHeight, const std::vector<std::string>& lanePath,
        const Config& config, float scale
    );
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    void onKeyboardEvent(const KeyboardEvent& event);
    [[nodiscard]] wl_surface* wlSurface() const noexcept;
    void requestLayout();
    void requestRedraw();

  protected:
    void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
    void layoutSheet(float contentWidth, float contentHeight) override;
    void cancelToFacade() override;
    [[nodiscard]] InputArea* initialFocusArea() override;
    void onSheetClose() override;

  private:
    std::vector<SearchPickerOption> m_normalOptions;
    std::vector<SearchPickerOption> m_instanceOptions;
    std::unordered_map<std::string, std::string> m_presetScripts; // picker value -> asset script path
    float m_scale = 1.0f;
    const Config* m_config = nullptr;
    std::vector<std::string> m_lanePath;
    Flex* m_root = nullptr;
    Flex* m_createActions = nullptr;
    SearchPicker* m_searchPicker = nullptr;
    Label* m_instanceDescription = nullptr;
    Input* m_instanceInput = nullptr;
    bool m_instanceModeEnabled = false;
    bool m_createFormVisible = false;
    std::string m_createType;
    std::string m_createLabel;

    void refreshPickerOptions();
    void refreshBodyState();
    void beginCreateFlow(const SearchPickerOption& option);
    void finishCreateFlow();
    void reopenForCurrentMode();
    [[nodiscard]] std::string instanceFormTitle() const;
    [[nodiscard]] std::pair<float, float> popupSize() const;
    [[nodiscard]] std::string suggestedInstanceId(std::string_view type) const;
    [[nodiscard]] bool canCreateInstanceId(std::string_view id) const;

    xdg_surface* m_parentXdgSurface = nullptr;
    wl_surface* m_parentWlSurface = nullptr;
    wl_output* m_output = nullptr;
    std::uint32_t m_serial = 0;
    std::uint32_t m_parentWidth = 0;
    std::uint32_t m_parentHeight = 0;
    bool m_internalReopen = false;
    SelectCallback m_onSelect;
    std::function<void()> m_onDismissed;
  };

} // namespace settings
