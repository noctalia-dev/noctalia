#include "pipewire/pipewire_spectrum.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <print>
#include <string_view>

class PipeWireSpectrumTestAccess {
public:
  static std::unique_ptr<PipeWireSpectrum> create() {
    return std::unique_ptr<PipeWireSpectrum>(new PipeWireSpectrum(PipeWireSpectrum::TestModeTag{}));
  }
};

namespace {

  int g_failures = 0;

  void fail(std::string_view message) {
    std::println(stderr, "pipewire_spectrum: FAIL: {}", message);
    ++g_failures;
  }

  void checkBins(int sampleRate, int lowerCutoffHz, int upperCutoffHz) {
    constexpr int kBandCount = 64;
    const auto bins = noctalia::pipewire::spectrumFrequencyBins(lowerCutoffHz, upperCutoffHz, sampleRate, kBandCount);
    if (bins.size() != static_cast<std::size_t>(kBandCount)) {
      fail("frequency bin count does not match requested band count");
      return;
    }
    if (!std::ranges::all_of(bins, [](float bin) { return std::isfinite(bin) && bin >= 1.0F && bin <= 2048.0F; })) {
      fail("frequency bins are not finite and within the FFT range");
    }
    if (!std::ranges::is_sorted(bins)) {
      fail("frequency bins are reversed");
    }
  }

  void checkFrequencyBins() {
    for (int sampleRate : {44100, 48000, 96000}) {
      checkBins(sampleRate, 20, 20000);
      checkBins(sampleRate, 30000, 40000);
      checkBins(sampleRate, 20000, 1000);
    }
  }

  void checkRangeNotifications() {
    auto spectrum = PipeWireSpectrumTestAccess::create();
    int selfRemovingCalls = 0;
    int persistentCalls = 0;
    PipeWireSpectrum::ListenerId selfRemovingId = 0;

    selfRemovingId = spectrum->addChangeListener(64, [&]() {
      ++selfRemovingCalls;
      spectrum->removeChangeListener(selfRemovingId);
    });
    const auto persistentId = spectrum->addChangeListener(16, [&]() { ++persistentCalls; });
    selfRemovingCalls = 0;
    persistentCalls = 0;

    spectrum->setFrequencyRange(40, 16000);
    if (selfRemovingCalls != 1 || persistentCalls != 1) {
      fail("range update did not notify each listener exactly once");
    }
    if (spectrum->values(persistentId).size() != 16
        || !std::ranges::all_of(spectrum->values(persistentId), [](float value) { return value == 0.0F; })) {
      fail("range update did not safely reset listener state");
    }

    spectrum->setFrequencyRange(40, 16000);
    if (selfRemovingCalls != 1 || persistentCalls != 1) {
      fail("unchanged range emitted a notification");
    }

    spectrum->setUpperCutoff(12000);
    if (selfRemovingCalls != 1 || persistentCalls != 2) {
      fail("compatible cutoff setter did not emit one notification");
    }
  }

} // namespace

int main() {
  checkFrequencyBins();
  checkRangeNotifications();

  if (g_failures == 0) {
    std::println("pipewire_spectrum: all checks passed");
    return 0;
  }
  std::println(stderr, "pipewire_spectrum: {} failure(s)", g_failures);
  return 1;
}
