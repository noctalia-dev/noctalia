#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ConfigService;
class Checkbox;
class Button;
class Flex;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
struct xdg_toplevel;

struct SearchPickerPopupRequest {
  // Set to open over an xdg toplevel (the settings window). Unset opens over the layer surface
  // that received the last input, resolved through LayerPopupHostRegistry.
  std::optional<XdgPopupParent> parent;
  std::string title;
  std::vector<SearchPickerOption> options;
  std::string selectedValue;
  std::string placeholder;
  std::string emptyText;
  float scale = 1.0F;
  // Keep the popup open after a pick and mark the picked option as selected, so several options can be tried in a
  // row; Escape, the close button, or a click outside dismisses it.
  bool keepOpenOnSelect = false;
  std::string optionToggleLabel;
  bool optionToggleChecked = false;
  bool showCancelButton = false;
  bool explicitConfirmation = false;
  bool showAllApplicationsToggle = false;
  float preferredWidth = 0.0F;
  float preferredHeight = 0.0F;
  // Use a focused, resizable native toplevel instead of an xdg_popup.
  bool nativeWindow = false;
  xdg_toplevel* nativeParent = nullptr;
  wl_surface* nativeParentSurface = nullptr;
};

class SearchPickerPopup final : public DialogPopupHost {
public:
  using SelectCallback = std::function<void(const std::string& value)>;
  using SelectWithOptionCallback = std::function<void(const std::string& value, bool optionChecked)>;

  SearchPickerPopup() = default;
  ~SearchPickerPopup() override;

  void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);
  void initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts
  );

  void setOnSelect(SelectCallback callback);
  void setOnSelectWithOption(SelectWithOptionCallback callback);
  void setOnDismissed(std::function<void()> callback);

  void open(SearchPickerPopupRequest request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_nativeWindow != nullptr || DialogPopupHost::isOpen(); }
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;
  void onSheetClose() override;
  void prepareNativeFrame(bool update, bool layout);
  void buildNativeScene(std::uint32_t width, std::uint32_t height);
  void layoutNativeScene(std::uint32_t width, std::uint32_t height);
  void closeNativeWindow(bool restoreParent);
  [[nodiscard]] bool
  openNativeWindow(const SearchPickerPopupRequest& request, std::uint32_t width, std::uint32_t height);

private:
  // Guard token for deferred callbacks that run on the next main-loop tick.
  // Callbacks capture a weak_ptr so they can detect destruction without
  // relying on a raw this pointer staying valid.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  float m_scale = 1.0F;
  std::string m_title;
  std::vector<SearchPickerOption> m_options;
  std::string m_selectedValue;
  bool m_keepOpenOnSelect = false;
  std::string m_optionToggleLabel;
  bool m_optionToggleChecked = false;
  bool m_showCancelButton = false;
  bool m_explicitConfirmation = false;
  bool m_showAllApplicationsToggle = false;
  float m_preferredWidth = 0.0F;
  float m_preferredHeight = 0.0F;
  std::string m_placeholder;
  std::string m_emptyText;

  Flex* m_root = nullptr;
  SearchPicker* m_searchPicker = nullptr;
  Checkbox* m_optionToggle = nullptr;
  Checkbox* m_showAllToggle = nullptr;
  Button* m_openButton = nullptr;
  std::unique_ptr<class ToplevelSurface> m_nativeWindow;
  std::array<InputArea*, 8> m_resizeEdges{};
  wl_surface* m_nativeParentSurface = nullptr;
  bool m_restoreParentOnClose = true;

  SelectCallback m_onSelect;
  SelectWithOptionCallback m_onSelectWithOption;
  std::function<void()> m_onDismissed;
};

// App-wide entry point for surfaces without their own SearchPickerPopup (layer-shell panels).
// The application registers its layer-parented popup at startup.
class SearchPickerDialog {
public:
  static void setPopup(SearchPickerPopup* popup) noexcept;
  // Opens over the layer surface that received the last input; `request.parent` must be unset.
  // Returns false when no popup is registered or the popup could not open.
  static bool open(SearchPickerPopupRequest request, SearchPickerPopup::SelectCallback onSelect);
};
