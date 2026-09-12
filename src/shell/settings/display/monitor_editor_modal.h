#pragma once

#include "compositors/display_backend.h"
#include "core/timer_manager.h"
#include "shell/settings/settings_sheet_modal.h"

#include <functional>
#include <memory>
#include <string>

class Flex;

namespace settings::display {

  class DisplayService;
  class RevertDialogModal;

  struct MonitorEditorRequest {
    float scale = 1.0F;
    DisplayService* display = nullptr;
    std::function<void(const std::string&)> onOpenEdid;
    std::function<void()> onClosed;
  };

  class MonitorEditorModal final {
  public:
    void initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown);
    void open(MonitorEditorRequest request);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const std::string& selectedOutput() const { return m_selectedOutput; }
    void setSelectedOutput(std::string name);
    void markDirty();

  private:
    void populateBody(Flex& body);
    [[nodiscard]] std::unique_ptr<Flex> buildOutputCard(const compositors::display::OutputState& output);
    void syncRevertDialog();

    SettingsSheetModal m_sheet;
    SettingsModalHost* m_dialogHost = nullptr;
    std::function<void()> m_dismissSelectDropdown;
    float m_scale = 1.0F;
    DisplayService* m_display = nullptr;
    std::function<void(const std::string&)> m_onOpenEdid;
    std::function<void()> m_onClosed;
    std::string m_selectedOutput;
    Timer m_scaleDebounceTimer;
    std::string m_scaleDebounceOutput;
    std::unique_ptr<RevertDialogModal> m_revertDialog;
  };

} // namespace settings::display
