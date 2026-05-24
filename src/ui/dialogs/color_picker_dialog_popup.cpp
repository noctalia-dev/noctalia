#include "ui/dialogs/color_picker_dialog_popup.h"

#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/controls/color_picker.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

ColorPickerDialogPopup::~ColorPickerDialogPopup() { destroyPopup(); }

void ColorPickerDialogPopup::initialize(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts
) {
  initializeBase(wayland, config, renderContext, popupHosts);
}

bool ColorPickerDialogPopup::openColorPicker() {
  const float scale = uiScale();
  const auto width = static_cast<std::uint32_t>(ColorPickerSheet::preferredDialogWidth(scale));
  const auto height =
      static_cast<std::uint32_t>(ColorPickerSheet::preferredDialogHeight(static_cast<float>(width), scale));
  return openPopup(width, height);
}

void ColorPickerDialogPopup::closeColorPickerWithoutResult() { destroyPopup(); }

void ColorPickerDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  auto sheet = std::make_unique<ColorPickerSheet>(uiScale());
  sheet->setTitle(ColorPickerDialog::currentOptions().title);
  if (const auto& initialColor = ColorPickerDialog::currentOptions().initialColor; initialColor.has_value()) {
    sheet->colorPicker()->setColor(*initialColor);
  } else {
    sheet->colorPicker()->setColor(colorForRole(ColorRole::Primary));
  }
  sheet->setOnCancel([this]() { DeferredCall::callLater([this]() { cancel(); }); });
  sheet->setOnApply([this](const Color& color) { DeferredCall::callLater([this, color]() { accept(color); }); });
  m_sheet = sheet.get();
  contentParent->addChild(std::move(sheet));
}

void ColorPickerDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_sheet == nullptr) {
    return;
  }
  const float sheetPadding = Style::spaceSm * uiScale();
  m_sheet->setPickerColumnWidth(std::max(160.0f, contentWidth - sheetPadding * 2.0f));
  m_sheet->setSize(contentWidth, contentHeight);
  m_sheet->layout(*renderContext());
}

void ColorPickerDialogPopup::cancelToFacade() { ColorPickerDialog::cancelIfPending(); }

InputArea* ColorPickerDialogPopup::initialFocusArea() {
  return m_sheet != nullptr ? m_sheet->initialFocusArea() : nullptr;
}

void ColorPickerDialogPopup::accept(const Color& result) {
  closeAfterAccept();
  ColorPickerDialog::complete(result);
}
