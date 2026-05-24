#pragma once

#include "ui/dialogs/dialog_popup_host.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/file_dialog_view.h"

#include <filesystem>
#include <memory>
#include <optional>

class ThumbnailService;

class FileDialogPopup final : public DialogPopupHost, public FileDialogHost, public FileDialogPresenter {
public:
  ~FileDialogPopup() override;

  void initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts, ThumbnailService& thumbnails
  );

  [[nodiscard]] bool openFileDialog() override;
  void closeFileDialogWithoutResult() override;

  // FileDialogHost overrides. The base provides the actual behavior; these
  // exist solely to satisfy the FileDialogHost vtable slots, since the base
  // methods are not part of FileDialogHost's vtable through the multiple
  // inheritance.
  void requestUpdateOnly() override { DialogPopupHost::requestUpdateOnly(); }
  void requestLayout() override { DialogPopupHost::requestLayout(); }
  void requestRedraw() override { DialogPopupHost::requestRedraw(); }
  void focusArea(InputArea* area) override;
  [[nodiscard]] InputArea* focusedArea() const override;
  void accept(std::optional<std::filesystem::path> result) override;
  void cancel() override { DialogPopupHost::cancel(); }

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;

  [[nodiscard]] float computePadding(float scale) const override;
  void runUpdatePhase() override;
  [[nodiscard]] bool preDispatchKeyboard(const KeyboardEvent& event) override;
  void onSheetClose() override;

private:
  ThumbnailService* m_thumbnails = nullptr;
  std::unique_ptr<FileDialogView> m_dialog;
};
