#pragma once

#include "app/poll_source.h"
#include "pipewire/pipewire_spectrum.h"

class PipeWireSpectrumPollSource final : public PollSource {
public:
  explicit PipeWireSpectrumPollSource(PipeWireSpectrum& spectrum) : m_spectrum(spectrum) {}

  int pollTimeoutMs() const override { return m_spectrum.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_spectrum.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  PipeWireSpectrum& m_spectrum;
};
