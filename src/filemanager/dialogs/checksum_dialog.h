#pragma once

#include "core/timer_manager.h"
#include "filemanager/checksum.h"
#include "filemanager/admin_session_client.h"
#include "ui/controls/context_menu_popup.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Button;
class Flex;
class Glyph;
class Input;
class Label;
class NativeDataTransfer;
class ProgressBar;
class WaylandConnection;

namespace filemanager {

  class ModalDialogHost;

  class ChecksumDialogController {
  public:
    static constexpr int kCalculateMenuId = 30104;
    static constexpr int kVerifyMenuId = 30105;

    struct Callbacks {
      std::function<void(std::string text)> writeClipboardText;
      std::function<bool()> isPickerOpen;
      std::function<void()> closePicker;
    };

    ChecksumDialogController(
        ModalDialogHost& dialogHost, NativeDataTransfer& transfer, WaylandConnection& wayland, Callbacks callbacks
    );
    ~ChecksumDialogController();

    [[nodiscard]] static std::array<ContextMenuControlEntry, 2> menuEntries(bool enabled);
    using ProtectedChecksum = std::function<std::optional<AdminSessionClient::OperationTransport>(checksum::Algorithm)>;
    void show(const std::filesystem::path& target, bool verify, ProtectedChecksum protectedChecksum = {});
    void dismiss();
    [[nodiscard]] bool isOpen() const noexcept { return m_dialog != nullptr; }

  private:
    struct ChecksumJob;
    struct DialogState {
      enum class StatusTone : std::uint8_t { Neutral, Error, Success, Mismatch };
      std::filesystem::path target;
      std::filesystem::path manifest;
      checksum::Algorithm algorithm = checksum::Algorithm::Sha256;
      bool verify = false;
      ProtectedChecksum protectedChecksum;
      StatusTone statusTone = StatusTone::Neutral;
      std::shared_ptr<ChecksumJob> job;
      Timer progressTimer;
      Timer copyFeedbackTimer;
      std::array<Button*, 3> algorithms{};
      Button* browse = nullptr;
      Button* paste = nullptr;
      Button* run = nullptr;
      Button* copy = nullptr;
      Button* close = nullptr;
      Input* expectedInput = nullptr;
      Input* calculated = nullptr;
      Label* manifestLabel = nullptr;
      Label* warning = nullptr;
      Glyph* statusIcon = nullptr;
      Label* status = nullptr;
      ProgressBar* progress = nullptr;
    };

    void buildFields(Flex& root, float width);
    void setStatus(DialogState::StatusTone tone, std::string_view text);
    void chooseManifest();
    void pasteExpected();
    void setInput(std::string_view value);
    void resetResult();
    void updateControls();
    void start();
    void stop();
    bool handleResponse(int response);

    ModalDialogHost& m_dialogHost;
    NativeDataTransfer& m_transfer;
    WaylandConnection& m_wayland;
    Callbacks m_callbacks;
    std::unique_ptr<DialogState> m_dialog;
    std::uint64_t m_inputRevision = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
