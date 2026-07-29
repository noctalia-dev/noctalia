#pragma once

class KeyboardBacklightService;
class OsdOverlay;

class KeyboardBacklightOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void onBrightnessChanged(const KeyboardBacklightService& service);
  void showValue(float brightness);

private:
  OsdOverlay* m_overlay = nullptr;
};
