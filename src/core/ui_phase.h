#pragma once

enum class UiPhase : unsigned char {
  Idle,
  PrepareFrame,
  Update,
  Layout,
  Render,
};

UiPhase currentUiPhase() noexcept;
const char* uiPhaseName(UiPhase phase) noexcept;

class UiPhaseScope {
public:
  explicit UiPhaseScope(UiPhase phase) noexcept;
  ~UiPhaseScope();

  UiPhaseScope(const UiPhaseScope&) = delete;
  UiPhaseScope& operator=(const UiPhaseScope&) = delete;

private:
  UiPhase m_previous = UiPhase::Idle;
};

void uiAssertSceneMutationAllowed(const char* operation);
void uiAssertNotRendering(const char* operation);
