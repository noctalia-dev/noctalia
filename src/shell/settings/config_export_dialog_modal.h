#pragma once

#include "config/config_credentials.h"
#include "shell/settings/settings_modal_host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class Button;
class Flex;
class Label;
class RadioButton;
class Renderer;

namespace settings {

  enum class ConfigExportMode : std::uint8_t {
    MergedUser,
    FullEffective,
  };

  struct ConfigExportDialogRequest {
    float scale = 1.0F;
    std::function<void(ConfigExportMode mode, noctalia::config::CredentialExportMode credentials)> callback;
  };

  class ConfigExportDialogModal {
  public:
    using ExportCallback =
        std::function<void(ConfigExportMode mode, noctalia::config::CredentialExportMode credentials)>;

    ConfigExportDialogModal() = default;
    ~ConfigExportDialogModal();

    void initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown);
    void open(ConfigExportDialogRequest request);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void requestLayout();
    void requestRedraw();

  private:
    [[nodiscard]] std::unique_ptr<Node> build();
    [[nodiscard]] LayoutSize measure(Renderer& renderer, const SettingsModalLayoutSpace& space);
    void arrange(Renderer& renderer, float width, float height);
    void setMode(ConfigExportMode mode);
    void setCredentials(noctalia::config::CredentialExportMode credentials);
    void accept();
    [[nodiscard]] std::unique_ptr<Flex>
    makeOption(ConfigExportMode mode, const std::string& title, const std::string& description);
    void clearNodePointers();

    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
    SettingsModalHost* m_host = nullptr;
    std::optional<SettingsModalHost::ModalId> m_modalId;
    std::function<void()> m_dismissSelectDropdown;
    float m_scale = 1.0F;
    ConfigExportMode m_mode = ConfigExportMode::MergedUser;
    noctalia::config::CredentialExportMode m_credentials = noctalia::config::CredentialExportMode::Keep;
    ExportCallback m_callback;
    Flex* m_root = nullptr;
    RadioButton* m_mergedRadio = nullptr;
    RadioButton* m_fullRadio = nullptr;
    Label* m_credentialsDescription = nullptr;
    Button* m_exportButton = nullptr;
    bool m_open = false;
  };

} // namespace settings
