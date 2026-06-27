#include "shell/osd/media_osd.h"

#include "dbus/mpris/mpris_service.h"
#include "shell/osd/osd_overlay.h"

namespace {

  OsdContent makeMprisContent(const MediaOsdData& data) {
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = data.artist.empty() ? data.title : data.title + " — " + data.artist,
        .showProgress = false,
    };
  }

} // namespace

void MediaOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void MediaOsd::onMprisChanged(const MprisService& service) {
  const auto activePlayerOpt = service.activePlayer();
  if (!activePlayerOpt.has_value()) {
    return;
  }
  const auto& activePlayer = activePlayerOpt.value();
  const MediaOsdData osdData = {.title = activePlayer.title, .artist = joinedArtists(activePlayer.artists)};

  // First snapshot seeds the baseline; it is not a user-visible transition.
  if (!m_hasData) {
    m_lastData = osdData;
    m_hasData = true;
    return;
  }

  if (activePlayer.playbackStatus != "Playing") {
    return;
  }

  if (osdData == m_lastData || m_overlay == nullptr) {
    return;
  }
  m_overlay->show(makeMprisContent(osdData));
  m_lastData = osdData;
}
