#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

class SystemBus;

struct PolkitRequestIdentity {
  std::string kind;
  std::uint32_t uid = 0;
  std::string userName;
};

struct PolkitRequest {
  std::string actionId;
  std::string message;
  std::string iconName;
  std::string cookie;
  bool isInternal = false;
  std::vector<PolkitRequestIdentity> identities;
};

class PolkitAgent {
public:
  using StateCallback = std::function<void()>;
  using ReadyCallback = std::function<void(bool ok, const std::string& error)>;

  explicit PolkitAgent(SystemBus& bus);
  ~PolkitAgent();

  PolkitAgent(const PolkitAgent&) = delete;
  PolkitAgent& operator=(const PolkitAgent&) = delete;

  // Kicks off asynchronous polkit session lookup + agent registration.
  // The ReadyCallback (if set) fires once registration succeeds or fails.
  void start();

  void setStateCallback(StateCallback callback);
  void setReadyCallback(ReadyCallback callback);
  void submitResponse(const std::string& response);
  void cancelRequest();

  void markNextRequestInternal();

  void addPollFds(std::vector<pollfd>& fds) const;
  int pollTimeoutMs() const;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx);

  bool hasPendingRequest() const noexcept;
  PolkitRequest pendingRequest() const;
  bool isResponseRequired() const noexcept;
  bool responseVisible() const noexcept;
  std::string inputPrompt() const;
  std::string supplementaryMessage() const;
  bool supplementaryIsError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
