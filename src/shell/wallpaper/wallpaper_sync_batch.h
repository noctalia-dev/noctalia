#pragma once

#include "shell/wallpaper/wallpaper_instance.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Lockstep coordination (syncTransitions) over the syncMember/syncReady/syncSkipped instance
// flags. An armed batch serves wallpaper state-change events; the un-armed wave serves startup
// first-loads. `start` re-enters Wallpaper::startTransition, so flags must be cleared in exactly
// the order below or the re-entry re-defers.
struct WallpaperSyncBatch {
  std::vector<std::unique_ptr<WallpaperInstance>>* instances = nullptr;
  std::function<std::string(const std::string&)> pathFor; // connector -> configured wallpaper path
  std::function<void(WallpaperInstance&)> start;          // actually starts the transition
  std::function<void()> rollPick;                         // batch/wave rolls its own shared effect pick
  bool armed = false;

  // Marks every instance that will start a fresh transition for this event. Flushes waiters from
  // the previous event first so nothing can stall across events.
  void arm() {
    release();

    armed = true;
    rollPick(); // each batch rolls its own shared effect/params
    for (auto& inst : *instances) {
      if (!inst->surface || !inst->wallpaperNode) {
        continue;
      }
      const auto newPath = pathFor(inst->connectorName);
      // Same "will start a fresh transition" conditions as the onStateChange() loop.
      if (newPath.empty() || inst->transitioning) {
        continue;
      }
      if (newPath == inst->currentPath || newPath == inst->loadingPath) {
        continue;
      }
      inst->syncMember = true;
      inst->syncSkipped = false; // a fresh batch re-evaluates this instance from scratch
    }
  }

  // Called from the top of startTransition(). True = transition deferred, caller must return.
  bool defer(WallpaperInstance& instance, bool syncEnabled) {
    if (armed && instance.syncMember) {
      instance.syncReady = true;
      maybeFire();
      return true;
    }

    // Startup lockstep: first-load transitions register themselves and wait until every monitor
    // has started its own initial load, then all start together. Self-registration (instead of a
    // batch armed at configure time) survives outputs being enumerated after the first image is
    // ready.
    if (!armed && !instance.syncMember && syncEnabled && instance.currentPath.empty()) {
      instance.syncMember = true;
      instance.syncReady = true;
      fireWave(); // no-op while other monitors are still pending their initial load
      return true;
    }
    return false;
  }

  // A failed load never reports ready; mark it skipped so neither an armed batch nor a pending
  // startup wave waits for it. Harmless when nothing is waiting (cleared on the next member mark).
  void skip(WallpaperInstance& instance) {
    instance.syncSkipped = true;
    maybeFire();
    fireWave();
  }

  // The instance is about to be erased (output disconnected, disabled by override, reload):
  // mark it skipped and re-evaluate, or already-deferred members wait for a report that can
  // never arrive. Mirrors skip(); harmless when nothing is waiting.
  void retire(WallpaperInstance& instance) {
    instance.syncSkipped = true;
    maybeFire();
    fireWave();
  }

  // Full teardown: forget the batch without firing start(); every instance is going away.
  void reset() {
    armed = false;
    for (auto& inst : *instances) {
      inst->syncMember = false;
      inst->syncReady = false;
      inst->syncSkipped = false;
    }
  }

  // A monitor whose new wallpaper is queued behind its running transition joins the armed batch
  // as a late member; it reports ready when finishTransition() drains the queue (or skip() when
  // the drain is cancelled), so queued drains fire in lockstep with fresh starts.
  void enlist(WallpaperInstance& instance) {
    if (armed) {
      instance.syncMember = true;
      instance.syncSkipped = false;
    }
  }

private:
  void release() {
    for (auto& inst : *instances) {
      if (!inst->syncMember) {
        continue;
      }
      const bool ready = std::exchange(inst->syncReady, false);
      inst->syncSkipped = false;
      inst->syncMember = false; // cleared before startTransition so it proceeds to animate
      if (ready) {
        start(*inst);
      }
    }
    armed = false;
  }

  void maybeFire() {
    if (!armed) {
      return;
    }
    for (auto& inst : *instances) {
      if (inst->syncMember && !inst->syncReady && !inst->syncSkipped) {
        return; // a member's image is still decoding
      }
    }
    release();
  }

  void fireWave() {
    if (armed) {
      return; // an armed state-change batch takes precedence over the startup wave
    }
    bool anyMember = false;
    for (auto& inst : *instances) {
      anyMember |= inst->syncMember;
    }
    if (!anyMember) {
      return;
    }
    for (auto& inst : *instances) {
      // A monitor that hasn't started its initial load yet keeps the wave pending.
      if (!inst->syncMember
          && !inst->transitioning
          && !inst->syncSkipped
          && inst->currentPath.empty()
          && !pathFor(inst->connectorName).empty()) {
        return;
      }
    }
    rollPick(); // each startup wave rolls its own shared effect/params
    for (auto& inst : *instances) {
      if (!inst->syncMember || !std::exchange(inst->syncReady, false)) {
        continue;
      }
      start(*inst); // syncMember stays set so the re-entry skips the startup block
      inst->syncMember = false;
    }
  }
};
