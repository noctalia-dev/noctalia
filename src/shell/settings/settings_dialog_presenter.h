#pragma once

#include "shell/settings/settings_modal_host.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog_view.h"
#include "ui/dialogs/glyph_picker_dialog.h"

#include <functional>
#include <memory>
#include <optional>

class AnimationManager;
class ColorPickerSheet;
class GlyphPicker;

namespace settings {

  // Routes dialogs opened from Settings into its in-scene modal stack while
  // preserving the existing popup presenters for every other shell surface.
  class SettingsDialogPresenter final : public ColorPickerDialogPresenter,
                                        public GlyphPickerDialogPresenter,
                                        public FileDialogPresenter,
                                        public FileDialogHost {
  public:
    SettingsDialogPresenter() = default;
    ~SettingsDialogPresenter() override;

    SettingsDialogPresenter(const SettingsDialogPresenter&) = delete;
    SettingsDialogPresenter& operator=(const SettingsDialogPresenter&) = delete;

    void initialize(
        SettingsModalHost& host, AnimationManager& animations, ThumbnailService& thumbnails,
        ColorPickerDialogPresenter& colorFallback, GlyphPickerDialogPresenter& glyphFallback,
        FileDialogPresenter& fileFallback, std::function<bool()> shouldUseModal, std::function<float()> uiScale,
        std::function<void()> dismissSelectDropdown
    );
    void shutdown();

    [[nodiscard]] bool openColorPicker() override;
    void closeColorPickerWithoutResult() override;
    [[nodiscard]] bool openGlyphPicker() override;
    void closeGlyphPickerWithoutResult() override;
    [[nodiscard]] bool openFileDialog() override;
    void closeFileDialogWithoutResult() override;

    void requestUpdateOnly() override;
    void requestLayout() override;
    void requestRedraw() override;
    void focusArea(InputArea* area) override;
    [[nodiscard]] InputArea* focusedArea() const override;
    void accept(std::optional<std::filesystem::path> result) override;
    void cancel() override;

  private:
    enum class Route : std::uint8_t {
      None,
      Modal,
      Popup,
    };

    [[nodiscard]] bool useModal() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] bool openColorModal();
    [[nodiscard]] bool openGlyphModal();
    [[nodiscard]] bool openFileModal();
    void acceptColor(const Color& color);
    void cancelColor();
    void acceptGlyph(const GlyphPickerResult& result);
    void cancelGlyph();
    void discardColorPresentation();
    void discardGlyphPresentation();
    void discardFilePresentation();

    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
    SettingsModalHost* m_host = nullptr;
    AnimationManager* m_animations = nullptr;
    ThumbnailService* m_thumbnails = nullptr;
    ColorPickerDialogPresenter* m_colorFallback = nullptr;
    GlyphPickerDialogPresenter* m_glyphFallback = nullptr;
    FileDialogPresenter* m_fileFallback = nullptr;
    std::function<bool()> m_shouldUseModal;
    std::function<float()> m_uiScale;
    std::function<void()> m_dismissSelectDropdown;

    Route m_colorRoute = Route::None;
    Route m_glyphRoute = Route::None;
    Route m_fileRoute = Route::None;
    std::optional<SettingsModalHost::ModalId> m_colorModal;
    std::optional<SettingsModalHost::ModalId> m_glyphModal;
    std::optional<SettingsModalHost::ModalId> m_fileModal;
    ColorPickerSheet* m_colorSheet = nullptr;
    GlyphPicker* m_glyphSheet = nullptr;
    std::unique_ptr<FileDialogView> m_fileDialog;
  };

} // namespace settings
