#pragma once

#include "app/poll_source.h"
#include "system/location_service.h"

class LocationPollSource final : public PollSource {
public:
  explicit LocationPollSource(LocationService& location) : m_location(location) {}

  int pollTimeoutMs() const override { return m_location.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_location.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  LocationService& m_location;
};
