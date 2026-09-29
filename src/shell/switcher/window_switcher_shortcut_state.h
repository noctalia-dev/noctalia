#pragma once

#include "core/input/key_modifiers.h"

#include <cstdint>

class WindowSwitcherShortcutState {
public:
  static constexpr std::uint32_t kModifierMask = KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super;

  void reset() noexcept {
    m_shortcutModifiers = 0;
    m_pendingReleaseModifiers = 0;
    m_currentModifiers = 0;
    m_releaseCheckPending = false;
  }

  void capture(std::uint32_t modifiers) noexcept {
    if (m_shortcutModifiers == 0) {
      m_shortcutModifiers = modifiers & kModifierMask;
    }
  }

  [[nodiscard]] std::uint32_t modifiers() const noexcept { return m_shortcutModifiers; }

  [[nodiscard]] bool noteRelease(std::uint32_t releasedModifier, std::uint32_t currentModifiers) noexcept {
    capture(currentModifiers);
    capture(releasedModifier);
    const std::uint32_t matchingRelease = releasedModifier & m_shortcutModifiers;
    if (matchingRelease == 0) {
      return false;
    }
    m_pendingReleaseModifiers |= matchingRelease;
    updateModifiers(currentModifiers);
    return true;
  }

  void updateModifiers(std::uint32_t modifiers) noexcept { m_currentModifiers = modifiers & kModifierMask; }

  [[nodiscard]] bool hasPendingRelease() const noexcept { return m_pendingReleaseModifiers != 0; }

  [[nodiscard]] bool beginReleaseCheck() noexcept {
    if (m_releaseCheckPending || !hasPendingRelease()) {
      return false;
    }
    m_releaseCheckPending = true;
    return true;
  }

  [[nodiscard]] bool completeReleaseCheck(std::uint32_t modifiers) noexcept {
    if (!m_releaseCheckPending) {
      return false;
    }
    m_releaseCheckPending = false;
    updateModifiers(modifiers);
    return (m_pendingReleaseModifiers & ~m_currentModifiers) != 0;
  }

private:
  std::uint32_t m_shortcutModifiers = 0;
  std::uint32_t m_pendingReleaseModifiers = 0;
  std::uint32_t m_currentModifiers = 0;
  bool m_releaseCheckPending = false;
};
