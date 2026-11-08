#pragma once

#include <functional>
#include <utility>

namespace noctalia::theme {

  // Header-only state machine behind Application::queueTemplateApply: template regeneration
  // must not run while a wallpaper transition is fading, so a pending application is held
  // (latest wins) until the transition goes idle. Callbacks are injected so it is testable
  // without the compositor.
  class TemplateApplyGate {
  public:
    // apply(changed) performs the regeneration; paletteChanged flags of superseded requests
    // are OR-ed into the one application that finally runs.
    void enqueue(
        bool paletteChanged, std::function<void(bool)> apply, std::function<bool()> transitionActive,
        std::function<void(std::function<void()>)> schedule
    ) {
      m_pendingChanged = m_pendingChanged || paletteChanged;
      m_pendingApply = std::move(apply);
      m_transitionActive = std::move(transitionActive);
      m_schedule = std::move(schedule);
      drain();
    }

    bool hasPending() const noexcept { return static_cast<bool>(m_pendingApply); }
    void cancel() noexcept {
      m_pendingApply = nullptr;
      m_pendingChanged = false;
    }

  private:
    void drain() {
      if (!m_pendingApply) {
        return;
      }
      if (!m_transitionActive()) {
        auto apply = std::move(m_pendingApply);
        const bool changed = m_pendingChanged;
        m_pendingChanged = false;
        apply(changed);
        return;
      }
      m_schedule([this] { drain(); });
    }

    std::function<void(bool)> m_pendingApply;
    std::function<bool()> m_transitionActive;
    std::function<void(std::function<void()>)> m_schedule;
    bool m_pendingChanged = false;
  };

} // namespace noctalia::theme
