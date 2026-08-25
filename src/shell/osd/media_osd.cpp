#include "shell/osd/media_osd.h"

#include "dbus/mpris/mpris_service.h"
#include "shell/osd/osd_overlay.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr double kVolumeChangeEpsilon = 0.003;

  OsdContent makeMprisContent(const MediaOsdData& data) {
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = data.artist.empty() ? data.title : data.title + " — " + data.artist,
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
  const auto activePlayerOpt = service.activePlayer();
  if (activePlayerOpt.has_value()) {
    const auto& activePlayer = activePlayerOpt.value();
    const MediaOsdData osdData = {.title = activePlayer.title, .artist = joinedArtists(activePlayer.artists)};

    // First snapshot seeds the baseline; it is not a user-visible transition.
    if (!m_hasData) {
      m_lastData = osdData;
      m_hasData = true;
    } else if (activePlayer.playbackStatus == "Playing" && osdData != m_lastData) {
      m_lastData = osdData;
      if (m_overlay != nullptr) {
        m_overlay->show(makeMprisContent(osdData));
      }
    }
  }

  // Volume is tracked per player rather than for the active one only: turning down a background
  // player is exactly when its name matters. listPlayers() is the blacklist-filtered view.
  const auto players = service.listPlayers();
  for (const auto& player : players) {
    const auto it = m_lastVolumes.find(player.busName);
    if (it == m_lastVolumes.end()) {
      m_lastVolumes.emplace(player.busName, player.volume);
      continue;
    }
    if (std::abs(player.volume - it->second) <= kVolumeChangeEpsilon) {
      continue;
    }
    it->second = player.volume;
    if (m_overlay != nullptr) {
      m_overlay->show(makeVolumeContent(player.identity, player.volume));
    }
  }

  std::erase_if(m_lastVolumes, [&players](const auto& entry) {
    return std::ranges::none_of(players, [&entry](const MprisPlayerInfo& player) {
      return player.busName == entry.first;
    });
  });
}
