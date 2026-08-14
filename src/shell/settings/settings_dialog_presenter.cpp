#include "shell/settings/settings_dialog_presenter.h"

#include "core/deferred_call.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "ui/controls/color_picker.h"
#include "ui/controls/glyph_picker.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>

namespace settings {

  SettingsDialogPresenter::~SettingsDialogPresenter() { shutdown(); }

  void SettingsDialogPresenter::shutdown() {
    discardFilePresentation();
    discardGlyphPresentation();
    discardColorPresentation();
    m_aliveGuard.reset();
    m_host = nullptr;
    m_animations = nullptr;
    m_thumbnails = nullptr;
    m_colorFallback = nullptr;
    m_glyphFallback = nullptr;
    m_fileFallback = nullptr;
    m_shouldUseModal = {};
    m_uiScale = {};
    m_dismissSelectDropdown = {};
  }

  void SettingsDialogPresenter::initialize(
      SettingsModalHost& host, AnimationManager& animations, ThumbnailService& thumbnails,
      ColorPickerDialogPresenter& colorFallback, GlyphPickerDialogPresenter& glyphFallback,
      FileDialogPresenter& fileFallback, std::function<bool()> shouldUseModal, std::function<float()> uiScale,
      std::function<void()> dismissSelectDropdown
  ) {
    m_host = &host;
    m_animations = &animations;
    m_thumbnails = &thumbnails;
    m_colorFallback = &colorFallback;
    m_glyphFallback = &glyphFallback;
    m_fileFallback = &fileFallback;
    m_shouldUseModal = std::move(shouldUseModal);
    m_uiScale = std::move(uiScale);
    m_dismissSelectDropdown = std::move(dismissSelectDropdown);
  }

  bool SettingsDialogPresenter::useModal() const { return m_shouldUseModal && m_shouldUseModal(); }

  float SettingsDialogPresenter::scale() const { return std::max(0.1F, m_uiScale ? m_uiScale() : 1.0F); }

  bool SettingsDialogPresenter::openColorPicker() {
    discardColorPresentation();
    if (useModal() && openColorModal()) {
      return true;
    }
    if (m_colorFallback != nullptr && m_colorFallback->openColorPicker()) {
      m_colorRoute = Route::Popup;
      return true;
    }
    return false;
  }

  void SettingsDialogPresenter::closeColorPickerWithoutResult() { discardColorPresentation(); }

  bool SettingsDialogPresenter::openGlyphPicker() {
    discardGlyphPresentation();
    if (useModal() && openGlyphModal()) {
      return true;
    }
    if (m_glyphFallback != nullptr && m_glyphFallback->openGlyphPicker()) {
      m_glyphRoute = Route::Popup;
      return true;
    }
    return false;
  }

  void SettingsDialogPresenter::closeGlyphPickerWithoutResult() { discardGlyphPresentation(); }

  bool SettingsDialogPresenter::openFileDialog() {
    discardFilePresentation();
    if (useModal() && openFileModal()) {
      return true;
    }
    if (m_fileFallback != nullptr && m_fileFallback->openFileDialog()) {
      m_fileRoute = Route::Popup;
      return true;
    }
    return false;
  }

  void SettingsDialogPresenter::closeFileDialogWithoutResult() { discardFilePresentation(); }

  bool SettingsDialogPresenter::openColorModal() {
    if (m_host == nullptr) {
      return false;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }
    const float dialogScale = scale();
    const float padding = 12.0F * dialogScale;
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_colorModal = m_host->push(
        SettingsModalRequest{
            .build = [this, aliveGuard, dialogScale]() -> std::unique_ptr<Node> {
              if (aliveGuard.expired()) {
                return nullptr;
              }
              auto sheet = std::make_unique<ColorPickerSheet>(dialogScale);
              sheet->setTitle(ColorPickerDialog::currentOptions().title);
              if (const auto& initial = ColorPickerDialog::currentOptions().initialColor; initial.has_value()) {
                sheet->colorPicker()->setColor(*initial);
              } else {
                sheet->colorPicker()->setColor(colorForRole(ColorRole::Primary));
              }
              sheet->setOnCancel([this, aliveGuard]() {
                DeferredCall::callLater([this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    cancelColor();
                  }
                });
              });
              sheet->setOnApply([this, aliveGuard](const Color& color) {
                DeferredCall::callLater([this, aliveGuard, color]() {
                  if (!aliveGuard.expired()) {
                    acceptColor(color);
                  }
                });
              });
              m_colorSheet = sheet.get();
              return sheet;
            },
            .measure =
                [dialogScale, padding](Renderer& /*renderer*/, const SettingsModalLayoutSpace& space) {
                  const float panelWidth = std::min(
                      ColorPickerSheet::preferredDialogWidth(dialogScale), space.maxContentWidth + 2.0F * padding
                  );
                  const float panelHeight = std::min(
                      ColorPickerSheet::preferredDialogHeight(panelWidth, dialogScale),
                      space.maxContentHeight + 2.0F * padding
                  );
                  return LayoutSize{
                      .width = std::max(1.0F, panelWidth - 2.0F * padding),
                      .height = std::max(1.0F, panelHeight - 2.0F * padding),
                  };
                },
            .arrange =
                [this, dialogScale](Renderer& renderer, float width, float height) {
                  if (m_colorSheet == nullptr) {
                    return;
                  }
                  const float sheetPadding = Style::spaceSm * dialogScale;
                  m_colorSheet->setPickerColumnWidth(std::max(160.0F, width - 2.0F * sheetPadding));
                  m_colorSheet->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = width, .height = height});
                },
            .initialFocusArea =
                [this]() { return m_colorSheet != nullptr ? m_colorSheet->initialFocusArea() : nullptr; },
            .requestClose =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    cancelColor();
                  }
                },
            .onClosed =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    m_colorSheet = nullptr;
                    m_colorModal.reset();
                    m_colorRoute = Route::None;
                  }
                },
            .contentPadding = padding,
            .windowMargin = 24.0F * dialogScale,
        }
    );
    if (!m_colorModal.has_value()) {
      m_colorSheet = nullptr;
      return false;
    }
    m_colorRoute = Route::Modal;
    return true;
  }

  bool SettingsDialogPresenter::openGlyphModal() {
    if (m_host == nullptr) {
      return false;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }
    const float dialogScale = scale();
    const float padding = 12.0F * dialogScale;
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_glyphModal = m_host->push(
        SettingsModalRequest{
            .build = [this, aliveGuard, dialogScale]() -> std::unique_ptr<Node> {
              if (aliveGuard.expired()) {
                return nullptr;
              }
              auto sheet = std::make_unique<GlyphPicker>(dialogScale);
              sheet->setTitle(GlyphPickerDialog::currentOptions().title);
              sheet->setInitialGlyph(GlyphPickerDialog::currentOptions().initialGlyph);
              sheet->setOnCancel([this, aliveGuard]() {
                DeferredCall::callLater([this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    cancelGlyph();
                  }
                });
              });
              sheet->setOnApply([this, aliveGuard](const GlyphPickerResult& result) {
                DeferredCall::callLater([this, aliveGuard, result]() {
                  if (!aliveGuard.expired()) {
                    acceptGlyph(result);
                  }
                });
              });
              m_glyphSheet = sheet.get();
              return sheet;
            },
            .measure =
                [dialogScale, padding](Renderer& /*renderer*/, const SettingsModalLayoutSpace& space) {
                  const float panelWidth =
                      std::min(GlyphPicker::preferredDialogWidth(dialogScale), space.maxContentWidth + 2.0F * padding);
                  const float panelHeight = std::min(
                      GlyphPicker::preferredDialogHeight(dialogScale), space.maxContentHeight + 2.0F * padding
                  );
                  return LayoutSize{
                      .width = std::max(1.0F, panelWidth - 2.0F * padding),
                      .height = std::max(1.0F, panelHeight - 2.0F * padding),
                  };
                },
            .arrange =
                [this](Renderer& renderer, float width, float height) {
                  if (m_glyphSheet != nullptr) {
                    m_glyphSheet->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = width, .height = height});
                  }
                },
            .initialFocusArea =
                [this]() { return m_glyphSheet != nullptr ? m_glyphSheet->initialFocusArea() : nullptr; },
            .requestClose =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    cancelGlyph();
                  }
                },
            .onClosed =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    if (m_dismissSelectDropdown) {
                      m_dismissSelectDropdown();
                    }
                    m_glyphSheet = nullptr;
                    m_glyphModal.reset();
                    m_glyphRoute = Route::None;
                  }
                },
            .contentPadding = padding,
            .windowMargin = 24.0F * dialogScale,
        }
    );
    if (!m_glyphModal.has_value()) {
      m_glyphSheet = nullptr;
      return false;
    }
    m_glyphRoute = Route::Modal;
    return true;
  }

  bool SettingsDialogPresenter::openFileModal() {
    if (m_host == nullptr || m_thumbnails == nullptr || m_animations == nullptr) {
      return false;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }
    const float dialogScale = scale();
    const float padding = 12.0F * dialogScale;
    m_fileDialog = std::make_unique<FileDialogView>(m_thumbnails);
    m_fileDialog->setHost(this);
    m_fileDialog->setAnimationManager(m_animations);
    m_fileDialog->setContentScale(dialogScale);
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_fileModal = m_host->push(
        SettingsModalRequest{
            .build = [this, aliveGuard]() -> std::unique_ptr<Node> {
              if (aliveGuard.expired() || m_fileDialog == nullptr) {
                return nullptr;
              }
              m_fileDialog->create();
              m_fileDialog->onOpen({});
              return m_fileDialog->releaseRoot();
            },
            .measure =
                [this, padding](Renderer& /*renderer*/, const SettingsModalLayoutSpace& space) {
                  const float panelWidth =
                      std::min(m_fileDialog->preferredWidth(), space.maxContentWidth + 2.0F * padding);
                  const float panelHeight =
                      std::min(m_fileDialog->preferredHeight(), space.maxContentHeight + 2.0F * padding);
                  return LayoutSize{
                      .width = std::max(1.0F, panelWidth - 2.0F * padding),
                      .height = std::max(1.0F, panelHeight - 2.0F * padding),
                  };
                },
            .arrange =
                [this](Renderer& renderer, float width, float height) {
                  if (m_fileDialog != nullptr) {
                    m_fileDialog->layout(renderer, width, height);
                  }
                },
            .update =
                [this](Renderer& renderer) {
                  if (m_fileDialog != nullptr) {
                    m_fileDialog->update(renderer);
                  }
                },
            .initialFocusArea =
                [this]() { return m_fileDialog != nullptr ? m_fileDialog->initialFocusArea() : nullptr; },
            .preDispatchKeyboard =
                [this](const KeyboardEvent& event) {
                  return m_fileDialog != nullptr
                      && m_fileDialog->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit);
                },
            .requestClose =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    cancel();
                  }
                },
            .onClosed =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    if (m_fileDialog != nullptr) {
                      m_fileDialog->onClose();
                      m_fileDialog.reset();
                    }
                    m_fileModal.reset();
                    m_fileRoute = Route::None;
                  }
                },
            .contentPadding = padding,
            .windowMargin = 24.0F * dialogScale,
        }
    );
    if (!m_fileModal.has_value()) {
      m_fileDialog.reset();
      return false;
    }
    m_fileRoute = Route::Modal;
    return true;
  }

  void SettingsDialogPresenter::acceptColor(const Color& color) {
    if (m_host == nullptr || !m_colorModal.has_value() || !m_host->pop(*m_colorModal)) {
      return;
    }
    ColorPickerDialog::complete(color);
  }

  void SettingsDialogPresenter::cancelColor() {
    if (m_host == nullptr || !m_colorModal.has_value() || !m_host->pop(*m_colorModal)) {
      return;
    }
    ColorPickerDialog::cancelIfPending();
  }

  void SettingsDialogPresenter::acceptGlyph(const GlyphPickerResult& result) {
    if (m_host == nullptr || !m_glyphModal.has_value() || !m_host->pop(*m_glyphModal)) {
      return;
    }
    GlyphPickerDialog::complete(result);
  }

  void SettingsDialogPresenter::cancelGlyph() {
    if (m_host == nullptr || !m_glyphModal.has_value() || !m_host->pop(*m_glyphModal)) {
      return;
    }
    GlyphPickerDialog::cancelIfPending();
  }

  void SettingsDialogPresenter::discardColorPresentation() {
    if (m_colorRoute == Route::Modal && m_host != nullptr && m_colorModal.has_value()) {
      (void)m_host->pop(*m_colorModal);
    } else if (m_colorRoute == Route::Popup && m_colorFallback != nullptr) {
      m_colorFallback->closeColorPickerWithoutResult();
      m_colorRoute = Route::None;
    }
  }

  void SettingsDialogPresenter::discardGlyphPresentation() {
    if (m_glyphRoute == Route::Modal && m_host != nullptr && m_glyphModal.has_value()) {
      (void)m_host->pop(*m_glyphModal);
    } else if (m_glyphRoute == Route::Popup && m_glyphFallback != nullptr) {
      m_glyphFallback->closeGlyphPickerWithoutResult();
      m_glyphRoute = Route::None;
    }
  }

  void SettingsDialogPresenter::discardFilePresentation() {
    if (m_fileRoute == Route::Modal && m_host != nullptr && m_fileModal.has_value()) {
      (void)m_host->pop(*m_fileModal);
    } else if (m_fileRoute == Route::Popup && m_fileFallback != nullptr) {
      m_fileFallback->closeFileDialogWithoutResult();
      m_fileRoute = Route::None;
    }
  }

  void SettingsDialogPresenter::requestUpdateOnly() {
    if (m_host != nullptr) {
      m_host->requestUpdateOnly();
    }
  }

  void SettingsDialogPresenter::requestLayout() {
    if (m_host != nullptr) {
      m_host->requestLayout();
    }
  }

  void SettingsDialogPresenter::requestRedraw() { requestLayout(); }

  void SettingsDialogPresenter::focusArea(InputArea* area) {
    if (m_host != nullptr) {
      m_host->focusArea(area);
    }
  }

  InputArea* SettingsDialogPresenter::focusedArea() const {
    return m_host != nullptr ? m_host->focusedArea() : nullptr;
  }

  void SettingsDialogPresenter::accept(std::optional<std::filesystem::path> result) {
    if (m_host == nullptr || !m_fileModal.has_value() || !m_host->pop(*m_fileModal)) {
      return;
    }
    FileDialog::complete(std::move(result));
  }

  void SettingsDialogPresenter::cancel() {
    if (m_host == nullptr || !m_fileModal.has_value() || !m_host->pop(*m_fileModal)) {
      return;
    }
    FileDialog::cancelIfPending();
  }

} // namespace settings
