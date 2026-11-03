#include "core/ui_phase.h"

#ifndef NDEBUG
#include "core/log.h"

#include <cstdlib>
#endif

namespace {

  thread_local UiPhase g_currentUiPhase = UiPhase::Idle;
#ifndef NDEBUG
  constexpr Logger kLog{"ui_phase"};
#endif

} // namespace

UiPhase currentUiPhase() noexcept { return g_currentUiPhase; }

const char* uiPhaseName(UiPhase phase) noexcept {
  switch (phase) {
  case UiPhase::Idle:
    return "Idle";
  case UiPhase::PrepareFrame:
    return "PrepareFrame";
  case UiPhase::Update:
    return "Update";
  case UiPhase::Layout:
    return "Layout";
  case UiPhase::Render:
    return "Render";
  }
  return "Unknown";
}

UiPhaseScope::UiPhaseScope(UiPhase phase) noexcept : m_previous(g_currentUiPhase) { g_currentUiPhase = phase; }

UiPhaseScope::~UiPhaseScope() { g_currentUiPhase = m_previous; }

void uiAssertSceneMutationAllowed(const char* operation) {
#ifndef NDEBUG
  if (g_currentUiPhase == UiPhase::Render) {
    kLog.error("UI phase violation: {} is not allowed during {}", operation, uiPhaseName(g_currentUiPhase));
    std::abort();
  }
#else
  (void)operation;
#endif
}

void uiAssertNotRendering(const char* operation) {
#ifndef NDEBUG
  if (g_currentUiPhase == UiPhase::Render) {
    kLog.error("UI phase violation: {} is not allowed during {}", operation, uiPhaseName(g_currentUiPhase));
    std::abort();
  }
#else
  (void)operation;
#endif
}
