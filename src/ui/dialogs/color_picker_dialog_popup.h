#pragma once

#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/dialog_popup_host.h"

class ColorPickerSheet;

class ColorPickerDialogPopup final : public DialogPopupHost, public ColorPickerDialogPresenter {
public:
  ~ColorPickerDialogPopup() override;

  void initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts
  );

  [[nodiscard]] bool openColorPicker() override;
  void closeColorPickerWithoutResult() override;

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;

private:
  void accept(const Color& result);

  ColorPickerSheet* m_sheet = nullptr;
};
