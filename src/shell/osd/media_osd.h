#pragma once

#include <string>
#include <unordered_map>
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
  std::unordered_map<std::string, double> m_lastVolumes;
  bool m_hasData = false;
};
