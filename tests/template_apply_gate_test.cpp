#include "test_check.h"
#include "theme/template_apply_gate.h"

#include <functional>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace {

  struct Fixture {
    bool active = false;
    std::vector<int> scheduled; // one tick per schedule() call; drain() runs pending m_ticks
    std::vector<std::pair<std::string, bool>> applied;
    int enqueuesWhileDraining = 0;

    void enqueue(std::string id, bool changed) {
      m_gate.enqueue(
          changed, [this, id = std::move(id)](bool appliedChanged) { applied.emplace_back(id, appliedChanged); },
          [this] { return active; },
          [this](std::function<void()> tick) {
            scheduled.push_back(static_cast<int>(scheduled.size()));
            m_ticks.emplace_back(std::move(tick));
          }
      );
    }

    // Fires the most recently scheduled retry tick (the real timer holds one at a time too).
    void runTick() {
      if (m_ticks.empty()) {
        return;
      }
      auto tick = std::move(m_ticks.back());
      m_ticks.clear();
      tick();
    }

    bool hasPending() const { return m_gate.hasPending(); }

  private:
    noctalia::theme::TemplateApplyGate m_gate;
    std::vector<std::function<void()>> m_ticks;
  };

  // Idle session: the application runs immediately, synchronously.
  void idleAppliesImmediately() {
    Fixture f;
    f.enqueue("A", true);
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"A", true}}));
    TEST_CHECK(f.scheduled.empty());
    TEST_CHECK(!f.hasPending());
  }

  // Mid-fade: nothing is applied; one retry is scheduled; drain applies once the fade ends.
  void transitionDefersUntilIdle() {
    Fixture f;
    f.active = true;
    f.enqueue("A", true);
    TEST_CHECK(f.applied.empty());
    TEST_CHECK(f.hasPending());
    TEST_CHECK(f.scheduled.size() == 1);
    f.runTick(); // still fading
    TEST_CHECK(f.applied.empty());
    f.active = false;
    f.runTick();
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"A", true}}));
    TEST_CHECK(!f.hasPending());
  }

  // Rapid changes during a fade collapse into the latest payload; changed flags OR together.
  void supersededRequestsCollapseToLatest() {
    Fixture f;
    f.active = true;
    f.enqueue("A", false);
    f.enqueue("B", true);
    f.enqueue("C", false);
    f.active = false;
    f.runTick();
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"C", true}}));
    TEST_CHECK(!f.hasPending());
  }

  // A false-only sequence applies with changed=false (no spurious colors_changed hook).
  void unchangedAppliesWithFalse() {
    Fixture f;
    f.enqueue("A", false);
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"A", false}}));
  }

  // After a completed application the m_gate is fresh: the changed flag does not carry over.
  void reEnqueueAfterDrainStartsNewCycle() {
    Fixture f;
    f.active = true;
    f.enqueue("A", true);
    TEST_CHECK(f.hasPending());
    f.active = false;
    f.runTick();
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"A", true}}));
    f.active = true;
    f.enqueue("B", false);
    f.active = false;
    f.runTick();
    TEST_CHECK((f.applied == std::vector<std::pair<std::string, bool>>{{"A", true}, {"B", false}}));
  }

} // namespace

int main() {
  idleAppliesImmediately();
  transitionDefersUntilIdle();
  supersededRequestsCollapseToLatest();
  unchangedAppliesWithFalse();
  reEnqueueAfterDrainStartsNewCycle();
  std::println("template_apply_gate_test: OK");
  return 0;
}
