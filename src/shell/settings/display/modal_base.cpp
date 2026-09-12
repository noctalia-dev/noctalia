#include "shell/settings/display/modal_base.h"

#include "render/core/renderer.h"
#include "ui/controls/flex.h"

#include <algorithm>

namespace settings {

  ModalBase::~ModalBase() {
    if (m_open) {
      close();
    }
  }

  void ModalBase::pushModal(SettingsModalHost& host, float contentPadding, float windowMargin) {
    m_host = &host;
    if (m_open) {
      close();
    }
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_modalId = m_host->push(
        SettingsModalRequest{
            .build = [this, aliveGuard]() -> std::unique_ptr<Node> {
              return aliveGuard.expired() ? nullptr : buildContent();
            },
            .measure = [this](
                           Renderer& renderer, const SettingsModalLayoutSpace& space
                       ) { return measureContent(renderer, space); },
            .arrange =
                [this](Renderer& renderer, float width, float height) { arrangeContent(renderer, width, height); },
            .update = [this](Renderer& renderer) { updateContent(renderer); },
            .requestClose =
                [this, aliveGuard]() {
                  if (!aliveGuard.expired()) {
                    close();
                  }
                },
            .onClosed =
                [this, aliveGuard]() {
                  if (aliveGuard.expired()) {
                    return;
                  }
                  m_open = false;
                  m_modalId.reset();
                  resetRoot();
                  onClosed();
                },
            .contentPadding = contentPadding,
            .windowMargin = windowMargin,
        }
    );
    m_open = m_modalId.has_value();
  }

  void ModalBase::close() {
    if (!m_open || m_host == nullptr || !m_modalId.has_value()) {
      return;
    }
    (void)m_host->pop(*m_modalId);
  }

  void ModalBase::requestLayout() {
    if (m_open && m_host != nullptr) {
      m_host->requestLayout();
    }
  }

  void ModalBase::rebuild() {
    if (m_open && m_host != nullptr && isTop()) {
      m_host->rebuildTop();
    }
  }

  bool ModalBase::isTop() const {
    return m_open && m_host != nullptr && m_modalId.has_value() && m_host->isTop(*m_modalId);
  }

  LayoutSize
  ModalBase::measureFixedWidth(Renderer& renderer, const SettingsModalLayoutSpace& space, float width) const {
    if (m_root == nullptr) {
      return {.width = 1.0F, .height = 1.0F};
    }
    const float resolvedWidth = std::min(width, space.maxContentWidth);
    LayoutConstraints constraints;
    constraints.setExactWidth(resolvedWidth);
    const float height = std::min(m_root->measure(renderer, constraints).height, space.maxContentHeight);
    return {.width = resolvedWidth, .height = height};
  }

  void ModalBase::arrangeRoot(Renderer& renderer, float width, float height) {
    if (m_root != nullptr) {
      m_root->arrange(renderer, {.x = 0.0F, .y = 0.0F, .width = width, .height = height});
    }
  }

  void ModalBase::updateContent(Renderer& /*renderer*/) {}
  void ModalBase::onClosed() {}

} // namespace settings
