#include "compositors/compositor_event_subscriptions.h"

#include "compositors/compositor_runtime.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/umbriel/umbriel_runtime.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace compositors {

  namespace {

    constexpr Logger kLog("compositor_subscriptions");
    constexpr auto kReconnectInitial = std::chrono::seconds(1);
    constexpr auto kReconnectMax = std::chrono::seconds(30);
    constexpr std::size_t kReadBufferMaxBytes = 1024U * 1024U;
    constexpr std::string_view kResetEvent = "reset";
    constexpr std::string_view kErrorEvent = "error";
    constexpr std::string_view kHyprlandSeparator = ">>";

    // The handshake requests are a few dozen bytes on a fresh socket, so a short write means the compositor is not
    // reading and the attempt fails like a refused connect.
    [[nodiscard]] bool sendAll(int fd, std::string_view data) {
      std::size_t offset = 0;
      while (offset < data.size()) {
        const ssize_t written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          return false;
        }
        offset += static_cast<std::size_t>(written);
      }
      return true;
    }

    // Umbriel streams only the requested families: {"cmd":"subscribe","events":[...]}.
    [[nodiscard]] std::string umbrielSubscribeRequest(const std::vector<std::string>& events) {
      nlohmann::json request = nlohmann::json::object();
      request["cmd"] = "subscribe";
      request["events"] = events;
      std::string payload = request.dump();
      payload.push_back('\n');
      return payload;
    }

    [[nodiscard]] CompositorEventSubscriptions::Payload makePayload(nlohmann::json value) {
      return std::make_shared<const CompositorEventPayload>(CompositorEventPayload{.value = std::move(value)});
    }

    [[nodiscard]] CompositorEventSubscriptions::Payload makePayload(std::string value) {
      return std::make_shared<const CompositorEventPayload>(CompositorEventPayload{.value = std::move(value)});
    }

  } // namespace

  bool supportsEventSubscriptions(CompositorKind kind) noexcept {
    return kind == CompositorKind::Umbriel || kind == CompositorKind::Niri || kind == CompositorKind::Hyprland;
  }

  CompositorEventSubscriptions::CompositorEventSubscriptions(CompositorRuntimeRegistry& runtimes)
      : m_runtimes(runtimes), m_kind(detect()) {}

  CompositorEventSubscriptions::~CompositorEventSubscriptions() {
    for (const auto& subscription : m_subscriptions) {
      closeSocket(*subscription);
    }
  }

  void CompositorEventSubscriptions::subscribe(
      std::uint64_t hostId, int subscriptionId, std::vector<std::string> events, Delivery delivery
  ) {
    unsubscribe(hostId, subscriptionId);
    if (!delivery || !supportsEventSubscriptions(m_kind)) {
      return;
    }
    auto subscription = std::make_unique<Subscription>();
    subscription->serial = m_nextSerial++;
    subscription->hostId = hostId;
    subscription->subscriptionId = subscriptionId;
    subscription->events = std::move(events);
    subscription->delivery = std::move(delivery);
    Subscription& added = *subscription;
    m_subscriptions.push_back(std::move(subscription));
    connect(added);
  }

  void CompositorEventSubscriptions::unsubscribe(std::uint64_t hostId, int subscriptionId) {
    std::erase_if(m_subscriptions, [hostId, subscriptionId](const std::unique_ptr<Subscription>& subscription) {
      if (subscription->hostId != hostId || subscription->subscriptionId != subscriptionId) {
        return false;
      }
      closeSocket(*subscription);
      return true;
    });
  }

  void CompositorEventSubscriptions::release(std::uint64_t hostId) {
    std::erase_if(m_subscriptions, [hostId](const std::unique_ptr<Subscription>& subscription) {
      if (subscription->hostId != hostId) {
        return false;
      }
      closeSocket(*subscription);
      return true;
    });
  }

  int CompositorEventSubscriptions::pollTimeoutMs() const {
    int timeout = -1;
    const auto now = Clock::now();
    for (const auto& subscription : m_subscriptions) {
      if (subscription->fd >= 0) {
        continue;
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(subscription->nextConnectAt - now).count();
      const int remainingMs = static_cast<int>(std::max<std::int64_t>(0, remaining));
      timeout = timeout < 0 ? remainingMs : std::min(timeout, remainingMs);
    }
    return timeout;
  }

  void CompositorEventSubscriptions::doAddPollFds(std::vector<pollfd>& fds) {
    m_polledSerials.clear();
    for (const auto& subscription : m_subscriptions) {
      if (subscription->fd < 0) {
        continue;
      }
      fds.push_back({.fd = subscription->fd, .events = POLLIN, .revents = 0});
      m_polledSerials.push_back(subscription->serial);
    }
  }

  void CompositorEventSubscriptions::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    for (std::size_t i = 0; i < m_polledSerials.size() && startIdx + i < fds.size(); ++i) {
      const pollfd& polled = fds[startIdx + i];
      if (polled.revents == 0) {
        continue;
      }
      Subscription* subscription = find(m_polledSerials[i]);
      if (subscription == nullptr || subscription->fd != polled.fd) {
        continue;
      }
      // A hangup still drains whatever the compositor wrote before closing; reading then reports EOF.
      if ((polled.revents & (POLLIN | POLLHUP)) != 0) {
        readSocket(*subscription);
      } else {
        disconnect(*subscription);
      }
    }

    const auto now = Clock::now();
    for (const auto& subscription : m_subscriptions) {
      if (subscription->fd < 0 && now >= subscription->nextConnectAt) {
        connect(*subscription);
      }
    }
  }

  CompositorEventSubscriptions::Subscription* CompositorEventSubscriptions::find(std::uint64_t serial) const {
    const auto it = std::ranges::find(m_subscriptions, serial, &Subscription::serial);
    return it != m_subscriptions.end() ? it->get() : nullptr;
  }

  std::string CompositorEventSubscriptions::socketPath() const {
    switch (m_kind) {
    case CompositorKind::Umbriel:
      return umbriel::findSocketPath();
    case CompositorKind::Niri:
      return m_runtimes.niri().socketPath();
    case CompositorKind::Hyprland:
      return m_runtimes.hyprland().eventSocketPath();
    default:
      return {};
    }
  }

  void CompositorEventSubscriptions::connect(Subscription& subscription) {
    if (subscription.resetPending) {
      if (!subscription.delivery(kResetEvent, nullptr)) {
        subscription.nextConnectAt = Clock::now() + kReconnectInitial;
        return;
      }
      subscription.resetPending = false;
    }

    const std::string path = socketPath();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      scheduleReconnect(subscription);
      return;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      scheduleReconnect(subscription);
      return;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      kLog.debug("event stream subscription could not connect to {} (errno {})", path, errno);
      ::close(fd);
      scheduleReconnect(subscription);
      return;
    }

    bool sent = true;
    if (m_kind == CompositorKind::Umbriel) {
      sent = sendAll(fd, umbrielSubscribeRequest(subscription.events));
    } else if (m_kind == CompositorKind::Niri) {
      sent = sendAll(fd, niri::kEventStreamRequest);
    }
    if (!sent) {
      ::close(fd);
      scheduleReconnect(subscription);
      return;
    }

    subscription.fd = fd;
    subscription.awaitingReply = m_kind == CompositorKind::Niri;
    subscription.delivered = false;
    subscription.readBuffer.clear();
    subscription.nextConnectAt = {};
    kLog.debug("event stream subscription connected to {}", path);
  }

  void CompositorEventSubscriptions::disconnect(Subscription& subscription) {
    const bool wasConnected = subscription.fd >= 0;
    closeSocket(subscription);
    scheduleReconnect(subscription);
    if (wasConnected && !subscription.delivery(kResetEvent, nullptr)) {
      subscription.resetPending = true;
    }
  }

  void CompositorEventSubscriptions::desync(Subscription& subscription) const {
    kLog.warn("event queue full for plugin host {} on {}; resubscribing", subscription.hostId, compositors::id(m_kind));
    closeSocket(subscription);
    subscription.resetPending = true;
    // A slow subscriber is not a compositor failure, so the backoff does not grow.
    subscription.nextConnectAt = Clock::now() + kReconnectInitial;
  }

  void CompositorEventSubscriptions::closeSocket(Subscription& subscription) {
    if (subscription.fd >= 0) {
      ::close(subscription.fd);
      subscription.fd = -1;
    }
    subscription.readBuffer.clear();
    subscription.awaitingReply = false;
    subscription.delivered = false;
  }

  void CompositorEventSubscriptions::scheduleReconnect(Subscription& subscription) {
    subscription.nextConnectAt = Clock::now() + subscription.backoff;
    subscription.backoff = std::min(subscription.backoff * 2, kReconnectMax);
  }

  void CompositorEventSubscriptions::readSocket(Subscription& subscription) {
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t readBytes = ::read(subscription.fd, buffer.data(), buffer.size());
      if (readBytes > 0) {
        subscription.readBuffer.insert(subscription.readBuffer.end(), buffer.begin(), buffer.begin() + readBytes);
        if (!handleLines(subscription)) {
          return;
        }
        if (subscription.readBuffer.size() > kReadBufferMaxBytes) {
          kLog.warn("compositor event stream line exceeded {} bytes; reconnecting", kReadBufferMaxBytes);
          disconnect(subscription);
          return;
        }
        continue;
      }
      if (readBytes < 0 && errno == EINTR) {
        continue;
      }
      if (readBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      // EOF or a read error. Complete lines were already delivered above, so an Umbriel `err` reply sent just
      // before the compositor closed the connection is not lost.
      disconnect(subscription);
      return;
    }
  }

  bool CompositorEventSubscriptions::handleLines(Subscription& subscription) {
    auto& readBuffer = subscription.readBuffer;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < readBuffer.size(); ++i) {
      if (readBuffer[i] != '\n') {
        continue;
      }
      std::string_view line(readBuffer.data() + lineStart, i - lineStart);
      lineStart = i + 1;
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      if (!line.empty() && !handleLine(subscription, line)) {
        return false;
      }
    }
    readBuffer.erase(readBuffer.begin(), readBuffer.begin() + static_cast<std::ptrdiff_t>(lineStart));
    return true;
  }

  bool CompositorEventSubscriptions::handleLine(Subscription& subscription, std::string_view line) {
    if (m_kind == CompositorKind::Hyprland) {
      // name>>data, with data passed through as text.
      const auto split = line.find(kHyprlandSeparator);
      if (split != std::string_view::npos
          && !deliverMessage(
              subscription, line.substr(0, split),
              makePayload(std::string(line.substr(split + kHyprlandSeparator.size())))
          )) {
        desync(subscription);
        return false;
      }
      return true;
    }

    nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
    if (json.is_discarded()) {
      kLog.warn("failed to parse compositor event stream message");
      return true;
    }
    if (!json.is_object()) {
      return true;
    }

    if (m_kind == CompositorKind::Niri) {
      // The first line answers the EventStream request; every later line is a single-key {Name: payload} object.
      if (subscription.awaitingReply) {
        subscription.awaitingReply = false;
        if (json.contains("Err")) {
          kLog.warn("niri rejected an event stream subscription; reconnecting");
          disconnect(subscription);
          return false;
        }
        return true;
      }
      if (json.size() != 1) {
        return true;
      }
      auto entry = json.begin();
      // Moving the value out leaves the object's key intact.
      const std::string& event = entry.key();
      if (!deliverMessage(subscription, event, makePayload(std::move(entry.value())))) {
        desync(subscription);
        return false;
      }
      return true;
    }

    // Umbriel: {"event": E, "data": D} per line, or one {"err": message} reply when the subscription is rejected.
    if (const auto err = json.find("err"); err != json.end()) {
      std::string message = err->is_string() ? err->get<std::string>() : err->dump();
      kLog.warn("umbriel rejected an event stream subscription: {}", message);
      // The connection closes either way; a refused message still leaves the "reset" below to deliver.
      (void)subscription.delivery(kErrorEvent, makePayload(std::move(message)));
      disconnect(subscription);
      return false;
    }
    const auto event = json.find("event");
    if (event == json.end() || !event->is_string()) {
      return true;
    }
    const auto& name = event->get_ref<const std::string&>();
    const auto data = json.find("data");
    if (!deliverMessage(subscription, name, makePayload(data != json.end() ? std::move(*data) : nlohmann::json()))) {
      desync(subscription);
      return false;
    }
    return true;
  }

  bool CompositorEventSubscriptions::deliverMessage(Subscription& subscription, std::string_view event, Payload data) {
    if (!subscription.delivery(event, std::move(data))) {
      return false;
    }
    if (!subscription.delivered) {
      // The stream works: the next drop reconnects quickly again.
      subscription.delivered = true;
      subscription.backoff = kReconnectInitial;
    }
    return true;
  }

} // namespace compositors
