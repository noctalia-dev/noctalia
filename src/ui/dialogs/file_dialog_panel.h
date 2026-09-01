#pragma once

#include "shell/panel/panel.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/file_dialog_view.h"

#include <memory>

class ThumbnailService;
class WaylandConnection;

/// The file chooser hosted the way the launcher is: its own layer surface with
/// Exclusive keyboard interactivity.
///
/// The settings modal opens the dialog as an xdg_popup parented to the settings
/// panel, which already holds the keyboard, so it types and reads modifiers
/// correctly. The portal has no panel to hang off -- its popup parent resolves to
/// whatever was last touched, normally the bar, whose keyboard interactivity is
/// None -- and a popup inherits that. Rather than reach over and mutate the bar,
/// a standalone request gets a surface that owns its keyboard outright.
///
/// Both hosts are kept: a dialog raised from settings belongs to settings.
class FileDialogPanel final : public Panel, public FileDialogHost, public FileDialogPresenter {
public:
  static constexpr const char* kPanelId = "file-dialog";

  FileDialogPanel(ThumbnailService* thumbnails, WaylandConnection* wayland);
  ~FileDialogPanel() override;

  /// Where a non-standalone request goes; normally the settings modal.
  void setAttachedPresenter(FileDialogPresenter* presenter) noexcept { m_attached = presenter; }

  // Panel
  void create() override;
  void onClose() override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  /// A file chooser is answered or cancelled deliberately, never by clicking past
  /// it -- the caller is blocked on a reply either way.
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }

  // FileDialogPresenter
  [[nodiscard]] bool openFileDialog() override;
  void closeFileDialogWithoutResult() override;

  // FileDialogHost
  void requestUpdateOnly() override;
  void requestLayout() override;
  void requestRedraw() override;
  void focusArea(InputArea* area) override;
  [[nodiscard]] InputArea* focusedArea() const override;
  void accept(std::optional<std::filesystem::path> result) override;
  void acceptMultiple(std::vector<std::filesystem::path> results) override;
  [[nodiscard]] std::uint32_t currentModifiers() const override;
  void cancel() override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void closeSelf();

  ThumbnailService* m_thumbnails = nullptr;
  WaylandConnection* m_wayland = nullptr;
  FileDialogPresenter* m_attached = nullptr;
  std::unique_ptr<FileDialogView> m_dialog;
  /// Set while answering, so the close that follows does not also report a
  /// cancellation over the same pending request.
  bool m_completing = false;
};
