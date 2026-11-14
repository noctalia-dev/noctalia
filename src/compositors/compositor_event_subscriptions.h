#pragma once

#include "compositors/compositor_detect.h"
#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace compositors {

  class CompositorRuntimeRegistry;

  // One event-stream message payload: decoded JSON (Umbriel, niri), raw text (Hyprland's `name>>data` data and the
  // "error" message), or nothing (the "reset" event and a JSON `null`).
  struct CompositorEventPayload {
    std::variant<std::monostate, nlohmann::json, std::string> value;
  };

  // True when noctalia.compositor.subscribe() can open an event stream on `kind`: Umbriel, niri, and Hyprland.
  [[nodiscard]] bool supportsEventSubscriptions(CompositorKind kind) noexcept;

  // Event-stream connections requested by plugin scripts through noctalia.compositor.subscribe(). Each subscription
  // owns its own nonblocking socket to the session compositor, separate from the runtimes' shared stream, performs
  // the compositor's handshake, and forwards every message untouched. Nothing is cached or replayed: the compositor's
  // own initial snapshot after each handshake is what a subscriber sees first.
  //
  // When a connection drops (EOF, read error, oversized line, or an Umbriel `err` reply) the subscriber receives
  // ("reset", nullptr) and the subscription reconnects with exponential backoff. When the subscriber refuses a
  // message (its script event queue is full), the stream is desynced: the socket closes at once, buffered lines are
  // dropped, and the next connect attempt delivers ("reset", nullptr) before handshaking again, so the subscriber
  // never continues from a stream with a hole in it. Main thread only: subscriptions arrive as script side effects
  // and are released when their Luau host dies.
  class CompositorEventSubscriptions final : public PollSource {
  public:
    using Payload = std::shared_ptr<const CompositorEventPayload>;
    // Returns false when the subscriber could not accept the message.
    using Delivery = std::function<bool(std::string_view event, Payload data)>;

    explicit CompositorEventSubscriptions(CompositorRuntimeRegistry& runtimes);
    ~CompositorEventSubscriptions() override;

    CompositorEventSubscriptions(const CompositorEventSubscriptions&) = delete;
    CompositorEventSubscriptions& operator=(const CompositorEventSubscriptions&) = delete;

    // `events` is the Umbriel subscription list; niri and Hyprland stream everything and ignore it. A no-op on
    // compositors without event-stream support.
    void subscribe(std::uint64_t hostId, int subscriptionId, std::vector<std::string> events, Delivery delivery);
    void unsubscribe(std::uint64_t hostId, int subscriptionId);
    void release(std::uint64_t hostId);

    int pollTimeoutMs() const override;
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    using Clock = std::chrono::steady_clock;

    struct Subscription {
      std::uint64_t serial = 0;
      std::uint64_t hostId = 0;
      int subscriptionId = 0;
      std::vector<std::string> events;
      Delivery delivery;
      int fd = -1;
      // niri answers the EventStream request once before streaming; that reply is not an event.
      bool awaitingReply = false;
      // Set once the current connection delivered a message; a proven stream resets the backoff.
      bool delivered = false;
      // A "reset" the subscriber has not accepted yet; delivered before the next handshake.
      bool resetPending = false;
      std::vector<char> readBuffer;
      Clock::time_point nextConnectAt;
      std::chrono::seconds backoff{1};
    };

    [[nodiscard]] Subscription* find(std::uint64_t serial) const;
    [[nodiscard]] std::string socketPath() const;
    // Delivers a pending "reset" first; while the subscriber refuses it, retries later without connecting.
    void connect(Subscription& subscription);
    // Closes the connection, delivers ("reset", nullptr) when it was open, and schedules the reconnect.
    static void disconnect(Subscription& subscription);
    // The subscriber refused a stream message: closes the connection and resubscribes after the initial backoff.
    void desync(Subscription& subscription) const;
    static void closeSocket(Subscription& subscription);
    static void scheduleReconnect(Subscription& subscription);
    void readSocket(Subscription& subscription);
    // Handles every complete buffered line. Returns false when a line ended the connection.
    [[nodiscard]] bool handleLines(Subscription& subscription);
    [[nodiscard]] bool handleLine(Subscription& subscription, std::string_view line);
    // Returns false when the subscriber refused the message; the caller must desync.
    [[nodiscard]] static bool deliverMessage(Subscription& subscription, std::string_view event, Payload data);

    CompositorRuntimeRegistry& m_runtimes;
    CompositorKind m_kind;
    std::vector<std::unique_ptr<Subscription>> m_subscriptions;
    // Serials of the subscriptions whose fds the last doAddPollFds() appended, in order. dispatch() matches them by
    // serial and fd, so a subscription added or removed between poll and dispatch is never misattributed.
    std::vector<std::uint64_t> m_polledSerials;
    std::uint64_t m_nextSerial = 1;
  };

} // namespace compositors
