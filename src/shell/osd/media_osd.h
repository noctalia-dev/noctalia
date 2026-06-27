#pragma once

#include <string>
class MprisService;
class OsdOverlay;

struct MediaOsdData {
  std::string title;
  std::string artist;

  bool operator==(const MediaOsdData& d) const { return d.artist == artist && d.title == title; }
};

class MediaOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void onMprisChanged(const MprisService& service);

private:
  OsdOverlay* m_overlay = nullptr;
  MediaOsdData m_lastData;
  bool m_hasData = false;
};
