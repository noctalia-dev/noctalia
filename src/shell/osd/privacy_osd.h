#pragma once

#include "pipewire/privacy_filter.h"

struct Config;
class OsdOverlay;
class PipeWireService;
struct PrivacyState;

class PrivacyOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void configure(const Config& config);
  void onConfigReload(const Config& config, const PipeWireService* service);
  void onPrivacyStateChanged(const PipeWireService& service);

private:
  struct State {
    bool mic = false;
    bool camera = false;
    bool screen = false;

    bool operator==(const State&) const = default;
  };

  [[nodiscard]] State fromPipewireState(const PrivacyState& privacyState) const;

  OsdOverlay* m_overlay = nullptr;
  PrivacyFilter m_micFilter;
  PrivacyFilter m_camFilter;
  PrivacyFilter m_screenFilter;
  // Baseline starts empty by contract: the first PipeWire enumeration announces
  // any capture already active at launch as an on-transition. Do not prime from
  // live state or these startup notifications are lost.
  State m_lastState;
};
