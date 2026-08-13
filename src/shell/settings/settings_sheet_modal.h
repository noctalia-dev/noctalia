#pragma once

#include "shell/settings/settings_modal_host.h"
#include "shell/settings/settings_sheet.h"

#include <memory>
#include <optional>

class InputArea;

namespace settings {

  class SettingsSheetModal {
  public:
    SettingsSheetModal() = default;
    ~SettingsSheetModal();

    void initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown);
    void open(SettingsSheetRequest request);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] InputArea* focusedArea() const noexcept;

    void setSheetTitle(std::string title);
    void setStatusMessage(std::string message, bool error);
    void clearStatusMessage();
    void rebuildBody();
    void requestLayout();
    void requestRedraw();

  private:
    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
    SettingsModalHost* m_host = nullptr;
    std::optional<SettingsModalHost::ModalId> m_modalId;
    std::function<void()> m_dismissSelectDropdown;
    SettingsSheet m_sheet;
    bool m_open = false;
  };

} // namespace settings
