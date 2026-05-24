#pragma once

#include "ui/dialogs/dialog_popup_host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Flex;
class RadioButton;
class RenderContext;
class WaylandConnection;
struct wl_output;
struct wl_surface;
struct xdg_surface;

namespace settings {

  enum class ConfigExportMode : std::uint8_t {
    MergedUser,
    FullEffective,
  };

  class ConfigExportDialogPopup final : public DialogPopupHost {
  public:
    using ExportCallback = std::function<void(ConfigExportMode mode)>;

    ConfigExportDialogPopup() = default;
    ~ConfigExportDialogPopup();

    void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);

    void open(
        xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
        std::uint32_t parentWidth, std::uint32_t parentHeight, float scale, ExportCallback callback
    );
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

    float m_scale = 1.0f;
    ConfigExportMode m_mode = ConfigExportMode::MergedUser;
    ExportCallback m_callback;
    Flex* m_root = nullptr;
    RadioButton* m_mergedRadio = nullptr;
    RadioButton* m_fullRadio = nullptr;
    std::uint32_t m_parentHeight = 0;
  };

} // namespace settings
