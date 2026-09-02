#include "shell/osd/media_osd.h"

#include "core/log.h"
#include "dbus/mpris/mpris_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace {

  constexpr Logger kLog("osd");

  constexpr double kVolumeChangeEpsilon = 0.003;

  // Seeking or buffering makes players republish metadata several times, and some of those
  // snapshots describe a track the user never lands on. Announce a track only once its identity
  // has held still for this long, so one scrub produces at most one popup.
  constexpr auto kTrackSettleDelay = std::chrono::milliseconds{400};

  OsdContent makeTrackContent(const MprisPlayerInfo& player) {
    const std::string artist = joinedArtists(player.artists);
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = artist.empty() ? player.title : player.title + " — " + artist,
        .showProgress = false,
    };
  }

  // Text-only, like the track OSD: the value carries a player identity of arbitrary length, which the
  // overlay only ellipsizes to the card interior when there is no progress bar beside it.
  OsdContent makeVolumeContent(const std::string& playerName, double volume) {
    const int percent = static_cast<int>(std::round(std::max(0.0, volume) * 100.0));
    const std::string level = std::to_string(percent) + "%";
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = playerName.empty() ? level : playerName + " — " + level,
        .showProgress = false,
        .overLimit = percent > 100,
    };
  }

} // namespace

void MediaOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void MediaOsd::onMprisChanged(const MprisService& service) {
  // listPlayers() is the blacklist-filtered view; activePlayer() picks from the same set. State is
  // kept per player so that the active player changing is never mistaken for a track change, and
  // volume is tracked for every player rather than the active one only: turning down a background
  // player is exactly when its name matters.
  const auto players = service.listPlayers();
  const auto activePlayer = service.activePlayer();
  const std::string activeBusName = activePlayer.has_value() ? activePlayer->busName : std::string{};

  for (const auto& player : players) {
    const auto [it, inserted] = m_players.try_emplace(player.busName);
    PlayerState& state = it->second;

    if (inserted) {
      state.volume = player.volume;
    } else if (std::abs(player.volume - state.volume) > kVolumeChangeEpsilon) {
      state.volume = player.volume;
      if (m_overlay != nullptr) {
        m_overlay->show(makeVolumeContent(player.identity, player.volume));
      }
    }

    // A titleless snapshot is a loading or seeking placeholder, not a track. Leaving the signature
    // untouched means the real metadata coming back does not read as a change.
    if (player.title.empty()) {
      continue;
    }

    const std::string signature = logicalTrackSignature(player);
    if (state.trackSignature.empty() || player.busName != activeBusName) {
      // First titled snapshot seeds the baseline, and background players are accounted for
      // silently: only the active player pops up.
      state.trackSignature = signature;
      cancelTrackOsd(player.busName);
      continue;
    }
    if (state.trackSignature == signature) {
      // Metadata flapped back to what the user was already told about.
      cancelTrackOsd(player.busName);
      continue;
    }
    if (player.playbackStatus != "Playing") {
      // Queueing a track while paused is not a transition; it announces itself on play.
      continue;
    }
    scheduleTrackOsd({
        .busName = player.busName,
        .signature = signature,
        .content = makeTrackContent(player),
    });
  }

  std::erase_if(m_players, [&players](const auto& entry) {
    return std::ranges::none_of(players, [&entry](const MprisPlayerInfo& player) {
      return player.busName == entry.first;
    });
  });
}

void MediaOsd::scheduleTrackOsd(PendingTrack pending) {
  // Restart the settle window only when the track being waited on actually changed; position
  // updates re-run the whole scan roughly once a second and must not starve the popup.
  if (m_pendingTrack.has_value()
      && m_pendingTrack->busName == pending.busName
      && m_pendingTrack->signature == pending.signature) {
    return;
  }
  m_pendingTrack = std::move(pending);
  m_trackSettleTimer.start(kTrackSettleDelay, [this]() { showPendingTrackOsd(); });
}

void MediaOsd::cancelTrackOsd(const std::string& busName) {
  if (m_pendingTrack.has_value() && m_pendingTrack->busName == busName) {
    m_pendingTrack.reset();
    m_trackSettleTimer.stop();
  }
}

void MediaOsd::showPendingTrackOsd() {
  if (!m_pendingTrack.has_value()) {
    return;
  }
  const PendingTrack pending = std::move(*m_pendingTrack);
  m_pendingTrack.reset();

  const auto it = m_players.find(pending.busName);
  if (it == m_players.end()) {
    return;
  }
  it->second.trackSignature = pending.signature;
  kLog.debug("media osd: announcing track name={} value={}", pending.busName, pending.content.value);
  if (m_overlay != nullptr) {
    m_overlay->show(pending.content);
  }
}
