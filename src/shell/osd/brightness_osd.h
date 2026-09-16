#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

class BrightnessService;
class OsdOverlay;

class BrightnessOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void primeFromService(const BrightnessService& service);
  void beginBatch();
  void endBatch();
  void onBrightnessChanged(const BrightnessService& service);
  void showValue(float brightness);

private:
  struct DisplaySnapshot {
    std::string id;
    int percent = -1;
  };

  OsdOverlay* m_overlay = nullptr;
  std::vector<DisplaySnapshot> m_snapshots;
  std::size_t m_batchDepth = 0;
  std::optional<float> m_batchBrightness;
};
