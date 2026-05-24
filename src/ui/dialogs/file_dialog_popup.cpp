#include "ui/dialogs/file_dialog_popup.h"

#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/wayland_seat.h"

#include <utility>

FileDialogPopup::~FileDialogPopup() { destroyPopup(); }

void FileDialogPopup::initialize(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts,
    ThumbnailService& thumbnails
) {
  initializeBase(wayland, config, renderContext, popupHosts);
  m_thumbnails = &thumbnails;
}

bool FileDialogPopup::openFileDialog() {
  if (m_thumbnails == nullptr) {
    return false;
  }

  // FileDialogView is constructed before openPopup so we can query its
  // preferred size and so that buildScene -> populateContent has a live
  // m_dialog to plug into the scene.
  auto dialog = std::make_unique<FileDialogView>(m_thumbnails);
  dialog->setHost(this);
  dialog->setContentScale(uiScale());

  const auto width = static_cast<std::uint32_t>(dialog->preferredWidth());
  const auto height = static_cast<std::uint32_t>(dialog->preferredHeight());

  m_dialog = std::move(dialog);
  if (!openPopup(width, height)) {
    m_dialog.reset();
    return false;
  }
  return true;
}

void FileDialogPopup::closeFileDialogWithoutResult() { destroyPopup(); }

void FileDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->setAnimationManager(&animations());
  m_dialog->create();
  m_dialog->onOpen({});
  if (m_dialog->root() != nullptr) {
    contentParent->addChild(m_dialog->releaseRoot());
  }
}

void FileDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->layout(*renderContext(), contentWidth, contentHeight);
}

void FileDialogPopup::cancelToFacade() { FileDialog::cancelIfPending(); }

InputArea* FileDialogPopup::initialFocusArea() { return m_dialog != nullptr ? m_dialog->initialFocusArea() : nullptr; }

float FileDialogPopup::computePadding(float /*scale*/) const {
  // Track the dialog's own contentScale to match prior behavior; this
  // happens to equal uiScale() in practice (set in openFileDialog) but
  // staying consistent with FileDialogView's view of the world.
  return m_dialog != nullptr ? m_dialog->contentScale() * 12.0f : 0.0f;
}

void FileDialogPopup::runUpdatePhase() {
  if (m_dialog != nullptr) {
    m_dialog->update(*renderContext());
  }
}

bool FileDialogPopup::preDispatchKeyboard(const KeyboardEvent& event) {
  if (m_dialog == nullptr) {
    return false;
  }
  return m_dialog->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit);
}

void FileDialogPopup::onSheetClose() {
  if (m_dialog != nullptr) {
    m_dialog->onClose();
    m_dialog.reset();
  }
}

void FileDialogPopup::focusArea(InputArea* area) { inputDispatcher().setFocus(area); }

InputArea* FileDialogPopup::focusedArea() const {
  // const_cast is safe: focusedArea() is logically const (read-only) on the
  // dispatcher; the dispatcher's accessor is non-const-correct but the
  // returned pointer is what we want to expose.
  return const_cast<FileDialogPopup*>(this)->inputDispatcher().focusedArea();
}

void FileDialogPopup::accept(std::optional<std::filesystem::path> result) {
  closeAfterAccept();
  FileDialog::complete(std::move(result));
}
