#include "shell/settings/settings_sheet_modal.h"

#include "core/deferred_call.h"
#include "render/core/renderer.h"
#include "shell/settings/settings_modal_host.h"

#include <algorithm>
#include <utility>

namespace settings {

  SettingsSheetModal::~SettingsSheetModal() {
    m_aliveGuard.reset();
    if (m_open) {
      close();
    }
  }

  void SettingsSheetModal::initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown) {
    m_host = &host;
    m_dismissSelectDropdown = std::move(dismissSelectDropdown);
  }

  void SettingsSheetModal::open(SettingsSheetRequest request) {
    if (m_host == nullptr) {
      return;
    }
    if (m_open) {
      close();
    }
    m_sheet.configure(std::move(request));
    const float scale = m_sheet.scale();
    const float padding = 12.0F * scale;
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_modalId = m_host->push(
        SettingsModalRequest{
            .build =
                [this, aliveGuard]() {
                  return m_sheet.build(
                      [this, aliveGuard]() {
                        if (!aliveGuard.expired()) {
                          close();
                        }
                      },
                      [this, aliveGuard]() {
                        if (!aliveGuard.expired() && m_dismissSelectDropdown) {
                          m_dismissSelectDropdown();
                        }
                      }
                  );
                },
            .measure =
                [this, padding](Renderer& renderer, const SettingsModalLayoutSpace& space) {
                  const float minPanelWidth = m_sheet.minWidth() * m_sheet.scale();
                  const float maxPanelWidth =
                      std::min(m_sheet.maxWidth() * m_sheet.scale(), space.maxContentWidth + 2.0F * padding);
                  const float preferredPanelWidth = m_sheet.parentFraction() * space.windowWidth;
                  const float panelWidth =
                      std::clamp(preferredPanelWidth, std::min(minPanelWidth, maxPanelWidth), maxPanelWidth);
                  const float contentWidth = std::max(1.0F, panelWidth - 2.0F * padding);
                  float contentHeight = m_sheet.naturalHeight(renderer, contentWidth);
                  if (m_sheet.fillParentHeight()) {
                    contentHeight = std::max(contentHeight, space.maxContentHeight);
                  }
                  return LayoutSize{
                      .width = contentWidth,
                      .height = std::min(contentHeight, space.maxContentHeight),
                  };
                },
            .arrange =
                [this](Renderer& renderer, float width, float height) { m_sheet.arrange(renderer, width, height); },
            .initialFocusArea = []() -> InputArea* { return nullptr; },
            .preDispatchKeyboard = [this](const KeyboardEvent& event) { return m_sheet.preDispatchKeyboard(event); },
            .requestClose =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    m_sheet.requestClose();
                  }
                },
            .onClosed =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    m_open = false;
                    m_modalId.reset();
                    m_sheet.clear();
                  }
                },
            .contentPadding = padding,
            .windowMargin = 24.0F * scale,
        }
    );
    m_open = m_modalId.has_value();
    if (!m_open) {
      m_sheet.clear();
    }
  }

  void SettingsSheetModal::close() {
    if (!m_open || m_host == nullptr) {
      return;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }
    if (!m_modalId.has_value() || !m_host->pop(*m_modalId)) {
      return;
    }
  }

  InputArea* SettingsSheetModal::focusedArea() const noexcept {
    return m_host != nullptr ? m_host->focusedArea() : nullptr;
  }

  void SettingsSheetModal::setSheetTitle(std::string title) {
    m_sheet.setTitle(std::move(title));
    requestLayout();
  }

  void SettingsSheetModal::setStatusMessage(std::string message, bool error) {
    m_sheet.setStatusMessage(std::move(message), error);
    requestLayout();
  }

  void SettingsSheetModal::clearStatusMessage() { setStatusMessage({}, false); }

  void SettingsSheetModal::rebuildBody() {
    if (!m_open || m_host == nullptr) {
      return;
    }
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    DeferredCall::callLater([this, aliveGuard]() {
      if (!aliveGuard.expired() && m_open && m_host != nullptr && m_modalId.has_value() && m_host->isTop(*m_modalId)) {
        m_host->rebuildTop();
      }
    });
  }

  void SettingsSheetModal::requestLayout() {
    if (m_open && m_host != nullptr) {
      m_host->requestLayout();
    }
  }

  void SettingsSheetModal::requestRedraw() { requestLayout(); }

} // namespace settings
