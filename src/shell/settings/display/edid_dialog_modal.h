#pragma once

#include "shell/settings/settings_sheet_modal.h"

#include <functional>
#include <string>

class ClipboardService;
class Flex;

namespace settings::display {

  class DisplayService;

  struct EdidDialogRequest {
    float scale = 1.0F;
    std::string outputName;
    DisplayService* display = nullptr;
    ClipboardService* clipboard = nullptr;
  };

  class EdidDialogModal final {
  public:
    void initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown);
    void open(EdidDialogRequest request);
    void close();

    [[nodiscard]] bool isOpen() const;
    void markDirty();

  private:
    void populateBody(Flex& body);
    [[nodiscard]] std::string contentText() const;
    [[nodiscard]] std::string copyPayload() const;

    SettingsSheetModal m_sheet;
    DisplayService* m_display = nullptr;
    ClipboardService* m_clipboard = nullptr;
    std::string m_outputName;
    float m_scale = 1.0F;
    bool m_showRaw = false;
  };

} // namespace settings::display
