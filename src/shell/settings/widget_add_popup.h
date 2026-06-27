#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

  struct WidgetAddPopupRequest {
    XdgPopupParent parent;
    std::vector<std::string> lanePath;
    const Config& config;
    float scale = 1.0f;
  };

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

    void open(WidgetAddPopupRequest request);
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
    std::unordered_set<std::string> m_pluginEntries; // picker values that are plugin entry ids (one-click add)
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
    void refreshBodyState(bool adjustFocus = true);
    void beginCreateFlow(const SearchPickerOption& option);
    void finishCreateFlow();
    void reopenForCurrentMode();
    [[nodiscard]] std::string instanceFormTitle() const;
    [[nodiscard]] std::pair<float, float> popupSize() const;
    [[nodiscard]] std::string suggestedInstanceId(std::string_view type) const;
    [[nodiscard]] bool canCreateInstanceId(std::string_view id) const;

    XdgPopupParent m_parent;
    bool m_internalReopen = false;
    SelectCallback m_onSelect;
    std::function<void()> m_onDismissed;
  };

} // namespace settings
