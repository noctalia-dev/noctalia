#pragma once

#include "app/poll_source.h"
#include "net/http_client.h"

class HttpClientPollSource : public PollSource {
public:
  explicit HttpClientPollSource(HttpClient& client) : m_client(client) {}

  int pollTimeoutMs() const override { return m_client.timeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override { m_client.dispatch(fds, startIdx); }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_client.addPollFds(fds); }

private:
  HttpClient& m_client;
};
