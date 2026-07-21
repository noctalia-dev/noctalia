#include "ui/dialogs/glyph_picker_dialog_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/controls/glyph_picker.h"
#include "ui/controls/select_dropdown_popup.h"
#include "wayland/wayland_seat.h"

#include <memory>

GlyphPickerDialogPopup::~GlyphPickerDialogPopup() { destroyPopup(); }

void GlyphPickerDialogPopup::initialize(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts
) {
  initializeBase(wayland, config, renderContext, popupHosts);
}

bool GlyphPickerDialogPopup::openGlyphPicker() {
  const float scale = uiScale();
  const auto width = static_cast<std::uint32_t>(GlyphPicker::preferredDialogWidth(scale));
  const auto height = static_cast<std::uint32_t>(GlyphPicker::preferredDialogHeight(scale));
  return openPopup(width, height);
}

void GlyphPickerDialogPopup::closeGlyphPickerWithoutResult() { destroyPopup(); }

bool GlyphPickerDialogPopup::onPointerEvent(const PointerEvent& event) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }
  return DialogPopupHost::onPointerEvent(event);
}

void GlyphPickerDialogPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }
  DialogPopupHost::onKeyboardEvent(event);
}

bool GlyphPickerDialogPopup::ownsSelectDropdownSurface(wl_surface* surface) const noexcept {
  return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
}

void GlyphPickerDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  auto sheet = std::make_unique<GlyphPicker>(uiScale());
  sheet->setTitle(GlyphPickerDialog::currentOptions().title);
  sheet->setInitialGlyph(GlyphPickerDialog::currentOptions().initialGlyph);
  sheet->setOnCancel([this]() { DeferredCall::callLater([this]() { cancel(); }); });
  sheet->setOnApply([this](const GlyphPickerResult& result) {
    DeferredCall::callLater([this, result]() { accept(result); });
  });
  m_sheet = sheet.get();
  contentParent->addChild(std::move(sheet));

  if (wayland() != nullptr && renderContext() != nullptr && xdgSurface() != nullptr) {
    if (m_selectPopup == nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(*wayland(), *renderContext());
    }
    if (config() != nullptr) {
      m_selectPopup->setShadowConfig(config()->config().shell.shadow);
    }
    m_selectPopup->setParent(xdgSurface(), wlSurface(), nullptr);
    contentParent->setPopupContext(m_selectPopup.get());
  }
}

void GlyphPickerDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_sheet == nullptr) {
    return;
  }
  m_sheet->setSize(contentWidth, contentHeight);
  m_sheet->layout(*renderContext());
}

void GlyphPickerDialogPopup::cancelToFacade() { GlyphPickerDialog::cancelIfPending(); }

InputArea* GlyphPickerDialogPopup::initialFocusArea() {
  return m_sheet != nullptr ? m_sheet->initialFocusArea() : nullptr;
}

void GlyphPickerDialogPopup::onSheetClose() {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
}

void GlyphPickerDialogPopup::accept(const GlyphPickerResult& result) {
  closeAfterAccept();
  GlyphPickerDialog::complete(result);
}
