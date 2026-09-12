#pragma once

#include "app/poll_source.h"
#include "core/inotify/inotify.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

struct SystemdPasswordQuery {
  fs::path ask_file;
  std::string message;
  fs::path icon;
  pid_t pid = 0;
  bool echo = false;
  bool silent = false;
  bool accept_cached = false;
  fs::path socket;
  std::uint64_t not_after = 0;
};

class SystemdPasswordAgent {
public:
  using StateCallback = std::function<void()>;

  explicit SystemdPasswordAgent(std::vector<std::filesystem::path> paths) : m_paths(std::move(paths)) {}
  ~SystemdPasswordAgent();

  SystemdPasswordAgent(const SystemdPasswordAgent&) = delete;
  SystemdPasswordAgent& operator=(const SystemdPasswordAgent&) = delete;

  // Start watching the ask-password directories
  void start();

  void setStateCallback(StateCallback callback);
  void submitResponse(const std::string& response);
  void cancelRequest();

  [[nodiscard]] bool hasPendingRequest() const noexcept;
  [[nodiscard]] SystemdPasswordQuery pendingRequest() const;

  void processEvents();
  [[nodiscard]] int fd() const noexcept { return m_inotify.fd(); }
  // Milliseconds until the nearest NotAfter expiry, or -1 if none. Drives
  // PollSource::pollTimeoutMs so the main loop wakes to hide expired prompts.
  [[nodiscard]] int nextExpiryTimeoutMs() const;
  // Drop expired (NotAfter elapsed), dead-PID, and vanished-file queries.
  // Returns true if the visible set changed (and emits the state callback).
  bool sweepExpired();

private:
  void reconcilePending();
  void emitStateCallback();

  std::optional<SystemdPasswordQuery> parsePasswordQuery(const fs::path& filepath);
  std::uint64_t eraseQuery(const fs::path& key);
  bool addQuery(const fs::path& path, SystemdPasswordQuery&& query);

  SystemdPasswordQuery* m_pending = nullptr;
  fs::path m_pendingKey;
  std::deque<std::pair<fs::path, SystemdPasswordQuery>> m_queries;
  const std::vector<std::filesystem::path> m_paths;
  std::vector<std::pair<int, fs::path>> m_watches;
  Inotify m_inotify;
  StateCallback m_stateCallback;
};

class SystemdPasswordAgentPollSource final : public PollSource {
public:
  explicit SystemdPasswordAgentPollSource(SystemdPasswordAgent& agent) : m_agent(agent) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_agent.nextExpiryTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    // Timeout wakes (NotAfter expiry) arrive with no fd event; fd wakes carry
    // inotify changes. processEvents() handles both (drain is non-blocking).
    if (m_agent.fd() < 0) {
      return;
    }
    if (startIdx >= fds.size() || (fds[startIdx].revents & POLLIN) != 0) {
      m_agent.processEvents();
    } else {
      m_agent.sweepExpired();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_agent.fd() >= 0)
      fds.push_back({.fd = m_agent.fd(), .events = POLLIN, .revents = 0});
  }

private:
  SystemdPasswordAgent& m_agent;
};
