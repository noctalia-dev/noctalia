#pragma once

#include "ui/dialogs/dialog_popup_host.h"
#include "ui/popup_parent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Flex;
class RadioButton;
class RenderContext;
class WaylandConnection;
struct wl_surface;

namespace settings {

  enum class ConfigExportMode : std::uint8_t {
    MergedUser,
    FullEffective,
  };

  struct ConfigExportDialogPopupRequest {
    XdgPopupParent parent;
    float scale = 1.0f;
    std::function<void(ConfigExportMode mode)> callback;
  };

  class ConfigExportDialogPopup final : public DialogPopupHost {
  public:
    using ExportCallback = std::function<void(ConfigExportMode mode)>;

    ConfigExportDialogPopup() = default;
    ~ConfigExportDialogPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void open(ConfigExportDialogPopupRequest request);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] wl_surface* wlSurface() const noexcept;

  protected:
    void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
    void layoutSheet(float contentWidth, float contentHeight) override;
    void cancelToFacade() override;
    [[nodiscard]] InputArea* initialFocusArea() override;

  private:
    void setMode(ConfigExportMode mode);
    void accept();
    [[nodiscard]] std::unique_ptr<Flex>
    makeOption(ConfigExportMode mode, const std::string& title, const std::string& description);

    // Guard token for deferred callbacks that run on the next main-loop tick.
    // Callbacks capture a weak_ptr so they can detect destruction without
    // relying on a raw this pointer staying valid.
    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

    float m_scale = 1.0f;
    ConfigExportMode m_mode = ConfigExportMode::MergedUser;
    ExportCallback m_callback;
    Flex* m_root = nullptr;
    RadioButton* m_mergedRadio = nullptr;
    RadioButton* m_fullRadio = nullptr;
    std::uint32_t m_parentHeight = 0;
  };

} // namespace settings
