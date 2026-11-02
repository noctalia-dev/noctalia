#pragma once

#include "app/poll_source.h"
#include "system/weather_service.h"

class WeatherPollSource final : public PollSource {
public:
  explicit WeatherPollSource(WeatherService& weather) : m_weather(weather) {}

  int pollTimeoutMs() const override { return m_weather.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_weather.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  WeatherService& m_weather;
};
