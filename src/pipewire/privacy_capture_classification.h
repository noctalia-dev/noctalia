#pragma once

#include <string_view>

namespace noctalia::pipewire {

  struct PrivacyAudioConsumerProperties {
    std::string_view mediaClass;
    std::string_view linkGroup;
    bool nodePassive = false;
    bool streamCaptureSink = false;
  };

  // Privacy indicators represent application capture, not the internal streams used to connect
  // filters, loopbacks, or sink monitors into the PipeWire graph.
  [[nodiscard]] bool isPrivacyAudioCaptureConsumer(const PrivacyAudioConsumerProperties& properties);

} // namespace noctalia::pipewire
