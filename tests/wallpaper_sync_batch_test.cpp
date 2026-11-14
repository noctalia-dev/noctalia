#include "shell/wallpaper/wallpaper_sync_batch.h"
#include "test_check.h"

#include <cstdint>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace {

  // arm() only null-checks surface/wallpaperNode; sentinel pointers are never dereferenced.
  inline const auto kFake = reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));

  struct Fixture {
    std::vector<std::unique_ptr<WallpaperInstance>> instances;
    std::map<std::string, std::string> paths;
    std::vector<std::string> started;
    int pickRolls = 0;
    WallpaperSyncBatch batch;

    Fixture() {
      batch.instances = &instances;
      batch.pathFor = [this](const std::string& connector) {
        const auto it = paths.find(connector);
        return it != paths.end() ? it->second : std::string{};
      };
      // Mirrors Wallpaper::startTransition: the fire path re-enters defer() and must fall through.
      batch.start = [this](WallpaperInstance& inst) {
        TEST_CHECK(!batch.defer(inst, true));
        started.push_back(inst.connectorName);
      };
      batch.rollPick = [this] { ++pickRolls; };
    }

    ~Fixture() {
      for (auto& inst : instances) {
        inst->surface.release();
      }
    }

    WallpaperInstance& add(const std::string& connector) {
      auto inst = std::make_unique<WallpaperInstance>();
      inst->connectorName = connector;
      inst->surface.reset(static_cast<LayerSurface*>(kFake));
      inst->wallpaperNode = static_cast<WallpaperNode*>(kFake);
      instances.push_back(std::move(inst));
      return *instances.back();
    }
  };

  // Two monitors, both decodes done: the second ready member fires BOTH starts in one pass.
  void armedBatchFiresTogether() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";

    f.batch.arm();
    TEST_CHECK(a.syncMember && b.syncMember);
    TEST_CHECK(f.batch.defer(a, true)); // B still decoding: nothing starts
    TEST_CHECK(f.started.empty());
    TEST_CHECK(f.batch.defer(b, true)); // B ready: batch fires
    TEST_CHECK((f.started == std::vector<std::string>{"A", "B"}));
    TEST_CHECK(!(f.batch.armed || a.syncMember || b.syncMember || a.syncReady || b.syncReady));
    TEST_CHECK(f.pickRolls == 1);
  }

  // A failed decode never reports ready; skip() must unblock the batch instead of stalling it.
  void decodeFailureUnblocksBatch() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";

    f.batch.arm();
    TEST_CHECK(f.batch.defer(a, true));
    f.batch.skip(b);
    TEST_CHECK((f.started == std::vector<std::string>{"A"}));
    TEST_CHECK(!f.batch.armed);
  }

  // A new state-change event flushes waiters from the previous one so nothing stalls across events.
  void reArmFlushesOldWaiters() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";

    f.batch.arm();
    TEST_CHECK(f.batch.defer(a, true)); // B never becomes ready
    f.batch.arm();                      // second event: A starts now, batch re-arms
    TEST_CHECK((f.started == std::vector<std::string>{"A"}));
    TEST_CHECK(f.batch.armed && a.syncMember);
  }

  // Startup: first-load transitions self-register and fire together once every monitor began loading.
  void startupWaveFiresTogether() {
    Fixture f;
    f.add("A");
    f.add("B");
    f.paths["A"] = "/a";
    f.paths["B"] = "/b";

    TEST_CHECK(f.batch.defer(*f.instances[0], true)); // B hasn't started its load yet
    TEST_CHECK(f.started.empty());
    TEST_CHECK(f.batch.defer(*f.instances[1], true)); // last member registers: wave fires
    TEST_CHECK((f.started == std::vector<std::string>{"A", "B"}));
    TEST_CHECK(f.pickRolls == 1);
  }

  // A startup monitor whose load failed must not hold the wave.
  void startupWaveSkipsFailedLoad() {
    Fixture f;
    f.add("A");
    f.add("B");
    f.paths["A"] = "/a";
    f.paths["B"] = "/b";

    TEST_CHECK(f.batch.defer(*f.instances[0], true));
    f.batch.skip(*f.instances[1]);
    TEST_CHECK((f.started == std::vector<std::string>{"A"}));
  }

  // No path change and syncTransitions disabled: defer() never holds, never registers.
  void nonMembersFallThrough() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/a";
    f.paths["B"] = "/b";
    a.currentPath = "/a";

    TEST_CHECK(!f.batch.defer(a, true));
    TEST_CHECK(!f.batch.defer(b, false)); // syncTransitions off
    TEST_CHECK(f.started.empty());
    TEST_CHECK(!(a.syncMember || b.syncMember));
  }

  // A monitor whose new wallpaper is queued behind its running transition joins the batch as a
  // late member: the fresh monitor's start waits for the queued drain, then both fire together.
  void queuedDrainFiresInLockstep() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";
    a.transitioning = true; // A is mid-transition: arm() excludes it, the queuedPath write enlists it

    f.batch.arm();
    f.batch.enlist(a);
    TEST_CHECK(a.syncMember && !a.syncSkipped);
    TEST_CHECK(f.batch.defer(b, true)); // B ready, but A must drain first
    TEST_CHECK(f.started.empty());
    a.transitioning = false;            // A's old transition ends, drain starts
    TEST_CHECK(f.batch.defer(a, true)); // A ready: batch fires A and B together
    TEST_CHECK((f.started == std::vector<std::string>{"A", "B"}));
  }

  // A cancelled queued drain must not stall the armed batch.
  void cancelledDrainUnblocksBatch() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";

    f.batch.arm();
    f.batch.enlist(a);
    TEST_CHECK(f.batch.defer(b, true));
    f.batch.skip(a); // runQueuedWallpaper found no drain to run
    TEST_CHECK((f.started == std::vector<std::string>{"B"}));
    TEST_CHECK(!f.batch.armed);
  }

  // A member applied immediately (first wallpaper without startup transition) skips: the batch
  // that armed it must not wait forever on a member that never calls defer().
  void immediateApplyMemberUnblocksBatch() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    // b.currentPath is empty: this change applies directly, no transition, no defer().
    f.batch.arm();
    TEST_CHECK(f.batch.defer(a, true));
    f.batch.skip(b);
    TEST_CHECK((f.started == std::vector<std::string>{"A"}));
    TEST_CHECK(!f.batch.armed);
  }

  // A member erased mid-batch (output disconnect / override / reload) retires: members that
  // already deferred must not hang on a member that can no longer report.
  void retiringMemberReleasesBatch() {
    Fixture f;
    auto& a = f.add("A");
    auto& b = f.add("B");
    f.paths["A"] = "/new-a";
    f.paths["B"] = "/new-b";
    a.currentPath = "/a";
    b.currentPath = "/b";
    f.batch.arm();
    TEST_CHECK(f.batch.defer(a, true));
    f.batch.retire(b);
    TEST_CHECK((f.started == std::vector<std::string>{"A"}));
    TEST_CHECK(!f.batch.armed);
  }

  // enlist() is a no-op while no batch is armed (drains outside a change event start alone).
  void enlistOutsideArmedBatchIsNoop() {
    Fixture f;
    auto& a = f.add("A");
    f.batch.enlist(a);
    TEST_CHECK(!a.syncMember);
  }

} // namespace

int main() {
  armedBatchFiresTogether();
  decodeFailureUnblocksBatch();
  reArmFlushesOldWaiters();
  startupWaveFiresTogether();
  startupWaveSkipsFailedLoad();
  nonMembersFallThrough();
  queuedDrainFiresInLockstep();
  cancelledDrainUnblocksBatch();
  immediateApplyMemberUnblocksBatch();
  retiringMemberReleasesBatch();
  enlistOutsideArmedBatchIsNoop();
  std::println("wallpaper_sync_batch_test: OK");
  return 0;
}
