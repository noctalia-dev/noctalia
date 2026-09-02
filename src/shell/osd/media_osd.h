#pragma once

#include "core/timer_manager.h"
#include "shell/osd/osd_overlay.h"

#include <optional>
#include <string>
#include <unordered_map>

class MprisService;

class MediaOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void onMprisChanged(const MprisService& service);

private:
  struct PlayerState {
    // Track signature this player has already been accounted for. Empty means no titled snapshot
    // has been seen yet; a real signature is never empty.
    std::string trackSignature;
    double volume = 1.0;
  };

  struct PendingTrack {
    std::string busName;
    std::string signature;
    OsdContent content;
  };

  void scheduleTrackOsd(PendingTrack pending);
  void cancelTrackOsd(const std::string& busName);
  void showPendingTrackOsd();

  OsdOverlay* m_overlay = nullptr;
  std::unordered_map<std::string, PlayerState> m_players;
  std::optional<PendingTrack> m_pendingTrack;
  Timer m_trackSettleTimer;
};
