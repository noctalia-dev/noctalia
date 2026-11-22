#pragma once

#include "filemanager/file_operation_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class Flex;
class Label;

namespace filemanager {

  class ModalDialogHost;

  class TrashFallbackDialogController {
  public:
    static constexpr int kCancelResponse = 0;
    static constexpr int kMoveToHomeResponse = 1;
    static constexpr int kPermanentDeleteResponse = 2;

    explicit TrashFallbackDialogController(ModalDialogHost& dialogHost);
    ~TrashFallbackDialogController();

    void show(
        const FileOperationManager::TrashFallbackOffer& offer,
        std::function<void(std::vector<std::string>)> onMoveToHome,
        std::function<void(std::vector<std::string>)> onPermanentDelete
    );
    void dismiss();
    [[nodiscard]] bool isOpen() const noexcept { return m_state != nullptr; }
    [[nodiscard]] bool calculationFinished() const noexcept;
    [[nodiscard]] bool moveToHomeEnabled() const noexcept;

  private:
    struct SizeJob;
    struct DialogState {
      FileOperationManager::TrashFallbackOffer offer;
      std::function<void(std::vector<std::string>)> onMoveToHome;
      std::function<void(std::vector<std::string>)> onPermanentDelete;
      std::shared_ptr<SizeJob> job;
      Label* sizeLabel = nullptr;
      Label* freeSpaceLabel = nullptr;
      Label* warningLabel = nullptr;
      Button* moveButton = nullptr;
      bool calculated = false;
      bool partialSize = false;
      bool sufficientSpace = false;
      std::uint64_t estimatedBytes = 0;
      std::uint64_t estimatedItems = 0;
      std::optional<std::uint64_t> availableBytes;
    };

    void buildFields(Flex& root, float width);
    void startCalculation();
    void updateLabels();
    bool interceptResponse(int response);

    ModalDialogHost& m_dialogHost;
    std::unique_ptr<DialogState> m_state;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
