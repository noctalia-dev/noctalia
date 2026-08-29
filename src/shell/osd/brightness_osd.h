#pragma once

#include <string>
#include <vector>

class BrightnessService;
class OsdOverlay;

class BrightnessOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void primeFromService(const BrightnessService& service);
  void onBrightnessChanged(const BrightnessService& service);
  void showValue(float brightness);

private:
  struct DisplaySnapshot {
    std::string id;
    int percent = -1;
  };

  OsdOverlay* m_overlay = nullptr;
  std::vector<DisplaySnapshot> m_snapshots;
};
