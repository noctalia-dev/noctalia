#include "ui/dialogs/file_dialog_panel.h"

#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "wayland/wayland_connection.h"

#include <utility>

FileDialogPanel::FileDialogPanel(ThumbnailService* thumbnails, WaylandConnection* wayland)
    : m_thumbnails(thumbnails), m_wayland(wayland) {}

FileDialogPanel::~FileDialogPanel() = default;

bool FileDialogPanel::openFileDialog() {
  // Only the portal asks for a standalone surface. Anything raised from settings
  // keeps its existing modal, which is anchored to the panel that opened it.
  if (!FileDialog::currentOptions().standalone) {
    return m_attached != nullptr && m_attached->openFileDialog();
  }
  if (m_thumbnails == nullptr) {
    return false;
  }

  // Built before the panel opens: PanelManager asks for preferredWidth/Height
  // while sizing the surface, which the view has to answer.
  m_dialog = std::make_unique<FileDialogView>(m_thumbnails);
  m_dialog->setHost(this);
  m_dialog->setContentScale(contentScale());

  m_completing = false;
  PanelManager::instance().openPanel(kPanelId);
  return true;
}

void FileDialogPanel::closeFileDialogWithoutResult() {
  if (m_dialog == nullptr) {
    if (m_attached != nullptr) {
      m_attached->closeFileDialogWithoutResult();
    }
    return;
  }
  m_completing = true; // the facade is already finishing this request
  closeSelf();
}

void FileDialogPanel::create() {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->setContentScale(contentScale());
  m_dialog->setAnimationManager(m_animations);
  m_dialog->create();
  m_dialog->onOpen({});
  if (m_dialog->root() != nullptr) {
    setRoot(m_dialog->releaseRoot());
  }
}

void FileDialogPanel::onClose() {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->onClose();
  m_dialog.reset();
  clearReleasedRoot();
  // Dismissed without an answer -- Escape, or the compositor closing us. The
  // portal caller is blocked on a reply, so it has to hear about it.
  if (!m_completing) {
    FileDialog::cancelIfPending();
  }
  m_completing = false;
}

void FileDialogPanel::closeSelf() { PanelManager::instance().closePanelById(kPanelId); }

float FileDialogPanel::preferredWidth() const {
  return m_dialog != nullptr ? m_dialog->preferredWidth() : scaled(800.0F);
}

float FileDialogPanel::preferredHeight() const {
  return m_dialog != nullptr ? m_dialog->preferredHeight() : scaled(560.0F);
}

InputArea* FileDialogPanel::initialFocusArea() const {
  return m_dialog != nullptr ? m_dialog->initialFocusArea() : nullptr;
}

bool FileDialogPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  return m_dialog != nullptr && m_dialog->handleGlobalKey(sym, modifiers, pressed, preedit);
}

void FileDialogPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_dialog != nullptr) {
    m_dialog->layout(renderer, width, height);
  }
}

void FileDialogPanel::doUpdate(Renderer& renderer) {
  if (m_dialog != nullptr) {
    m_dialog->update(renderer);
  }
}

void FileDialogPanel::requestUpdateOnly() { PanelManager::instance().requestUpdateOnly(); }

void FileDialogPanel::requestLayout() { PanelManager::instance().requestLayout(); }

void FileDialogPanel::requestRedraw() { PanelManager::instance().requestRedraw(); }

void FileDialogPanel::focusArea(InputArea* area) { PanelManager::instance().inputDispatcher().setFocus(area); }

InputArea* FileDialogPanel::focusedArea() const { return PanelManager::instance().inputDispatcher().focusedArea(); }

std::uint32_t FileDialogPanel::currentModifiers() const {
  return m_wayland != nullptr ? m_wayland->keyboardModifiers() : 0U;
}

void FileDialogPanel::accept(std::optional<std::filesystem::path> result) {
  m_completing = true;
  closeSelf();
  FileDialog::complete(std::move(result));
}

void FileDialogPanel::acceptMultiple(std::vector<std::filesystem::path> results) {
  m_completing = true;
  closeSelf();
  FileDialog::completeMultiple(std::move(results));
}

void FileDialogPanel::cancel() { closeSelf(); }
