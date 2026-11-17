#include "tests/test_check.h"
#include "wayland/native_data_transfer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <ranges>
#include <source_location>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
}

namespace {

  class WlrootsServer {
  public:
    struct Client {
      WlrootsServer* owner = nullptr;
      wl_client* serverClient = nullptr;
      wl_display* display = nullptr;
      wl_registry* registry = nullptr;
      wl_compositor* compositor = nullptr;
      wl_shm* shm = nullptr;
      wl_seat* seat = nullptr;
      wl_pointer* pointer = nullptr;
      wl_data_device_manager* manager = nullptr;
      wl_surface* surface = nullptr;
      std::uint32_t buttonSerial = 0;

      ~Client() { disconnect(); }

      void disconnect() {
        if (surface)
          wl_surface_destroy(surface);
        surface = nullptr;
        if (pointer)
          wl_pointer_destroy(pointer);
        pointer = nullptr;
        if (seat)
          wl_seat_destroy(seat);
        seat = nullptr;
        if (compositor)
          wl_compositor_destroy(compositor);
        compositor = nullptr;
        if (shm)
          wl_shm_destroy(shm);
        shm = nullptr;
        if (manager)
          wl_data_device_manager_destroy(manager);
        manager = nullptr;
        if (registry)
          wl_registry_destroy(registry);
        registry = nullptr;
        if (display)
          wl_display_disconnect(display);
        display = nullptr;
      }

      static void
      global(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
        auto& self = *static_cast<Client*>(data);
        if (std::string(interface) == wl_compositor_interface.name)
          self.compositor = static_cast<wl_compositor*>(
              wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 6U))
          );
        else if (std::string(interface) == wl_shm_interface.name)
          self.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        else if (std::string(interface) == wl_seat_interface.name) {
          self.seat =
              static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 7U)));
          static const wl_seat_listener listener = {.capabilities = &seatCapabilities, .name = &seatName};
          wl_seat_add_listener(self.seat, &listener, &self);
        } else if (std::string(interface) == wl_data_device_manager_interface.name)
          self.manager = static_cast<wl_data_device_manager*>(
              wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
          );
      }
      static void globalRemove(void*, wl_registry*, std::uint32_t) {}
      static void seatCapabilities(void* data, wl_seat* seat, std::uint32_t capabilities) {
        auto& self = *static_cast<Client*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self.pointer) {
          self.pointer = wl_seat_get_pointer(seat);
          static const wl_pointer_listener listener = {
              .enter = &pointerEnter,
              .leave = &pointerLeave,
              .motion = &pointerMotion,
              .button = &pointerButton,
              .axis = &pointerAxis,
              .frame = &pointerFrame,
              .axis_source = &pointerAxisSource,
              .axis_stop = &pointerAxisStop,
              .axis_discrete = &pointerAxisDiscrete,
              .axis_value120 = &pointerAxisValue120,
              .axis_relative_direction = &pointerAxisRelativeDirection,
          };
          wl_pointer_add_listener(self.pointer, &listener, &self);
        }
      }
      static void
      pointerButton(void* data, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t, std::uint32_t state) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED)
          static_cast<Client*>(data)->buttonSerial = serial;
      }
      static void pointerEnter(void*, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {}
      static void pointerLeave(void*, wl_pointer*, std::uint32_t, wl_surface*) {}
      static void pointerMotion(void*, wl_pointer*, std::uint32_t, wl_fixed_t, wl_fixed_t) {}
      static void pointerAxis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {}
      static void pointerFrame(void*, wl_pointer*) {}
      static void pointerAxisSource(void*, wl_pointer*, std::uint32_t) {}
      static void pointerAxisStop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
      static void pointerAxisDiscrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
      static void pointerAxisValue120(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
      static void pointerAxisRelativeDirection(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
      static void seatName(void*, wl_seat*, const char*) {}

      void connectClient() {
        int sockets[2] = {-1, -1};
        TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
        serverClient = wl_client_create(owner->m_display, sockets[0]);
        TEST_CHECK(serverClient != nullptr);
        display = wl_display_connect_to_fd(sockets[1]);
        TEST_CHECK(display != nullptr);
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {.global = &global, .global_remove = &globalRemove};
        TEST_CHECK(wl_registry_add_listener(registry, &listener, this) == 0);
      }
    };

    WlrootsServer() {
      m_display = wl_display_create();
      TEST_CHECK(m_display != nullptr);
      m_loop = wl_display_get_event_loop(m_display);
      auto* compositor = wlr_compositor_create(m_display, 6, nullptr);
      m_manager = wlr_data_device_manager_create(m_display);
      constexpr std::array<std::uint32_t, 2> shmFormats{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
      auto* shm = wlr_shm_create(m_display, 1, shmFormats.data(), shmFormats.size());
      m_seat = wlr_seat_create(m_display, "test-seat");
      TEST_CHECK(compositor && m_manager && shm && m_seat);
      wlr_seat_set_capabilities(m_seat, WL_SEAT_CAPABILITY_POINTER);
      wl_list_init(&m_startDragListener.link);
      m_startDragListener.notify = &startDragRequest;
      wl_signal_add(&m_seat->events.request_start_drag, &m_startDragListener);
      m_source.owner = this;
      m_target.owner = this;
      m_source.connectClient();
      m_target.connectClient();
      pumpUntil([this] {
        return m_source.compositor
            && m_source.shm
            && m_source.seat
            && m_source.pointer
            && m_source.manager
            && m_target.compositor
            && m_target.shm
            && m_target.seat
            && m_target.pointer
            && m_target.manager;
      });
      m_source.surface = wl_compositor_create_surface(m_source.compositor);
      m_target.surface = wl_compositor_create_surface(m_target.compositor);
      wl_surface_commit(m_source.surface);
      wl_surface_commit(m_target.surface);
      pumpUntil([this] { return sourceSurface() && targetSurface(); });
    }

    ~WlrootsServer() {
      m_source.disconnect();
      m_target.disconnect();
      wl_display_destroy_clients(m_display);
      wl_list_remove(&m_startDragListener.link);
      if (m_seat)
        wlr_seat_destroy(m_seat);
      if (m_display)
        wl_display_destroy(m_display);
    }

    WlrootsServer(const WlrootsServer&) = delete;
    WlrootsServer& operator=(const WlrootsServer&) = delete;

    Client& sourceClient() { return m_source; }
    Client& targetClient() { return m_target; }
    wlr_seat* seat() const { return m_seat; }
    bool dragStarted() const { return m_seat->drag && m_seat->drag->started; }
    bool dragIconCreated() const { return m_seat->drag && m_seat->drag->icon && m_seat->drag->icon->surface; }
    bool dragEnded() const { return m_seat->drag == nullptr; }
    wlr_surface* dragIconSurface() const {
      return m_seat->drag && m_seat->drag->icon ? m_seat->drag->icon->surface : nullptr;
    }
    wlr_surface* dragOrigin() const { return m_dragOrigin; }
    bool dragAcceptedCopy() const {
      return m_seat->drag
          && m_seat->drag->source
          && m_seat->drag->source->accepted
          && m_seat->drag->source->current_dnd_action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    }
    wlr_surface* sourceSurface() const { return findSurface(m_source, m_source.surface); }
    wlr_surface* targetSurface() const { return findSurface(m_target, m_target.surface); }
    wlr_surface* sourceClientSurface(wl_surface* surface) const { return findSurface(m_source, surface); }

    void pump() {
      TEST_CHECK(wl_event_loop_dispatch(m_loop, 0) >= 0);
      wl_display_flush_clients(m_display);
      pumpClient(m_source.display);
      pumpClient(m_target.display);
      if (m_sourceTransfer)
        pumpTransfer(*m_sourceTransfer);
      if (m_targetTransfer)
        pumpTransfer(*m_targetTransfer);
    }

    template <typename Predicate>
    void pumpUntil(Predicate predicate, std::source_location location = std::source_location::current()) {
      const auto waitNumber = ++m_waitCount;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        pump();
        usleep(1000);
      }
      if (!predicate()) {
        std::fprintf(stderr, "wlroots transfer wait %u timed out in %s\n", waitNumber, location.function_name());
        TEST_CHECK(false);
      }
    }

    void attach(NativeDataTransfer& source, NativeDataTransfer& target) {
      m_sourceTransfer = &source;
      m_targetTransfer = &target;
    }

  private:
    static void startDragRequest(wl_listener* listener, void* data) {
      WlrootsServer* self = nullptr;
      self = wl_container_of(listener, self, m_startDragListener);
      auto* request = static_cast<wlr_seat_request_start_drag_event*>(data);
      const bool valid = wlr_seat_validate_pointer_grab_serial(self->m_seat, request->origin, request->serial);
      if (!valid) {
        return;
      }
      self->m_dragOrigin = request->origin;
      wlr_seat_start_pointer_drag(self->m_seat, request->drag, request->serial);
    }

    static wlr_surface* findSurface(const Client& client, wl_surface* proxy) {
      if (!client.serverClient || !proxy)
        return nullptr;
      struct Search {
        std::uint32_t id;
        wlr_surface* found = nullptr;
      } search{wl_proxy_get_id(reinterpret_cast<wl_proxy*>(proxy))};
      wl_client_for_each_resource(
          client.serverClient,
          [](wl_resource* resource, void* data) -> wl_iterator_result {
            auto& search = *static_cast<Search*>(data);
            if (wl_resource_get_id(resource) == search.id
                && std::string(wl_resource_get_class(resource)) == wl_surface_interface.name) {
              search.found = wlr_surface_from_resource(resource);
              return WL_ITERATOR_STOP;
            }
            return WL_ITERATOR_CONTINUE;
          },
          &search
      );
      return search.found;
    }

    static void pumpClient(wl_display* display) {
      while (wl_display_prepare_read(display) != 0)
        TEST_CHECK(wl_display_dispatch_pending(display) >= 0);
      static_cast<void>(wl_display_flush(display));
      pollfd fd{wl_display_get_fd(display), POLLIN, 0};
      const int result = poll(&fd, 1, 0);
      if (result > 0 && (fd.revents & POLLIN))
        TEST_CHECK(wl_display_read_events(display) == 0);
      else
        wl_display_cancel_read(display);
      TEST_CHECK(wl_display_dispatch_pending(display) >= 0);
      TEST_CHECK(wl_display_get_error(display) == 0);
    }

    void pumpTransfer(NativeDataTransfer& transfer) {
      std::vector<pollfd> fds;
      const auto start = transfer.addPollFds(fds);
      if (!fds.empty())
        static_cast<void>(poll(fds.data(), fds.size(), 0));
      transfer.dispatch(fds, start);
    }

    wl_display* m_display = nullptr;
    wl_event_loop* m_loop = nullptr;
    wlr_data_device_manager* m_manager = nullptr;
    wlr_seat* m_seat = nullptr;
    wl_listener m_startDragListener{};
    wlr_surface* m_dragOrigin = nullptr;
    Client m_source;
    Client m_target;
    NativeDataTransfer* m_sourceTransfer = nullptr;
    NativeDataTransfer* m_targetTransfer = nullptr;
    unsigned m_waitCount = 0;
  };

  void testWlrootsRoundTrip() {
    WlrootsServer server;
    auto& sourceClient = server.sourceClient();
    auto& targetClient = server.targetClient();
    NativeDataTransfer source;
    NativeDataTransfer target;
    server.attach(source, target);
    source.bind(sourceClient.manager, sourceClient.seat);
    target.bind(targetClient.manager, targetClient.seat);
    TEST_CHECK(source.available() && target.available());
    source.setDragIconResources(sourceClient.compositor, sourceClient.shm);

    wlr_seat_pointer_notify_enter(server.seat(), server.sourceSurface(), 2.0, 3.0);
    wlr_seat_pointer_notify_frame(server.seat());
    const auto serial = wlr_seat_pointer_notify_button(server.seat(), 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&sourceClient] { return sourceClient.buttonSerial != 0; });
    TEST_CHECK(sourceClient.buttonSerial == serial && serial != 0);

    std::optional<bool> sourceFinished;
    source.setDragHandlers({}, {}, {}, [&sourceFinished](bool ok) { sourceFinished = ok; });
    std::optional<NativeDataTransfer::Payload> received;
    std::optional<bool> targetFinished;
    bool targetMotion = false;
    target.setDragHandlers(
        [&targetClient, &targetMotion](wl_surface* surface, float, float, std::uint32_t actions) {
          targetMotion = true;
          return surface == targetClient.surface && (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
              ? static_cast<std::uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
              : 0U;
        },
        {},
        [&received, &targetFinished](std::uint32_t action) {
          TEST_CHECK(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
          return NativeDataTransfer::DropReceiver([&received, &targetFinished](auto payload, auto finish) {
            received = std::move(payload);
            finish(true);
            targetFinished = true;
          });
        },
        {}
    );

    const NativeDataTransfer::Payload payload{{"text/uri-list", "file:///tmp/wlroots-private-test\r\n"}};
    const NativeDataTransfer::DragIconImage icon{
        .width = 4,
        .height = 4,
        .scale = 1,
        .pixels = {
            0xFFFF0000,
            0xFFFF0000,
            0xFF00FF00,
            0xFF00FF00,
            0xFFFF0000,
            0xFFFF0000,
            0xFF00FF00,
            0xFF00FF00,
            0xFF0000FF,
            0xFF0000FF,
            0xFFFFFFFF,
            0xFFFFFFFF,
            0xFF0000FF,
            0xFF0000FF,
            0xFFFFFFFF,
            0xFFFFFFFF,
        },
    };
    TEST_CHECK(source.startDrag(payload, sourceClient.surface, serial, &icon));
    server.pumpUntil([&server] { return server.dragStarted(); });
    server.pumpUntil([&server] { return server.dragIconCreated(); });
    TEST_CHECK(server.dragOrigin() == server.sourceSurface());
    TEST_CHECK(server.dragIconSurface() != server.sourceSurface());
    wlr_seat_pointer_notify_enter(server.seat(), server.targetSurface(), 9.0, 11.0);
    wlr_seat_pointer_notify_motion(server.seat(), 2, 9.0, 11.0);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&targetMotion] { return targetMotion; });
    server.pumpUntil([&server] { return server.dragAcceptedCopy(); });
    wlr_seat_pointer_notify_button(server.seat(), 3, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&targetFinished] { return targetFinished.has_value(); });
    server.pumpUntil([&sourceFinished] { return sourceFinished.has_value(); });
    server.pumpUntil([&server] { return server.dragEnded(); });
    TEST_CHECK(*targetFinished && *sourceFinished);
    TEST_CHECK(!source.dragging() && !source.busy());
    TEST_CHECK(received.has_value());
    TEST_CHECK(received->at("text/uri-list") == payload.at("text/uri-list"));
  }

  void testWlrootsIconCancel() {
    WlrootsServer server;
    auto& sourceClient = server.sourceClient();
    NativeDataTransfer source;
    NativeDataTransfer target;
    server.attach(source, target);
    source.bind(sourceClient.manager, sourceClient.seat);
    target.bind(server.targetClient().manager, server.targetClient().seat);
    source.setDragIconResources(sourceClient.compositor, sourceClient.shm);

    wlr_seat_pointer_notify_enter(server.seat(), server.sourceSurface(), 2.0, 3.0);
    wlr_seat_pointer_notify_frame(server.seat());
    const auto serial = wlr_seat_pointer_notify_button(server.seat(), 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&sourceClient] { return sourceClient.buttonSerial != 0; });
    TEST_CHECK(sourceClient.buttonSerial == serial && serial != 0);

    std::optional<bool> finished;
    source.setDragHandlers({}, {}, {}, [&finished](bool ok) { finished = ok; });
    const NativeDataTransfer::DragIconImage icon{
        .width = 4,
        .height = 4,
        .scale = 1,
        .pixels = std::vector<std::uint32_t>(16, 0xFF336699),
    };
    TEST_CHECK(source.startDrag(
        {{"text/uri-list", "file:///tmp/wlroots-icon-cancel\r\n"}}, sourceClient.surface, serial, &icon
    ));
    server.pumpUntil([&server] { return server.dragStarted() && server.dragIconCreated(); });
    TEST_CHECK(server.dragOrigin() == server.sourceSurface());
    TEST_CHECK(server.dragIconSurface() != server.sourceSurface());
    source.cancelDrag();
    TEST_CHECK(finished.has_value() && !*finished);
    TEST_CHECK(!source.dragging() && !source.busy());
    server.pumpUntil([&server] { return server.dragEnded(); });
  }

  void testWlrootsBorrowedIconLifecycle() {
    WlrootsServer server;
    auto& sourceClient = server.sourceClient();
    auto& targetClient = server.targetClient();
    NativeDataTransfer source;
    NativeDataTransfer target;
    server.attach(source, target);
    source.bind(sourceClient.manager, sourceClient.seat);
    target.bind(targetClient.manager, targetClient.seat);
    auto* externalIcon = wl_compositor_create_surface(sourceClient.compositor);
    TEST_CHECK(externalIcon != nullptr);

    wlr_seat_pointer_notify_enter(server.seat(), server.sourceSurface(), 2.0, 3.0);
    wlr_seat_pointer_notify_frame(server.seat());
    const auto serial = wlr_seat_pointer_notify_button(server.seat(), 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&sourceClient] { return sourceClient.buttonSerial != 0; });
    TEST_CHECK(sourceClient.buttonSerial == serial && serial != 0);

    std::vector<std::uint32_t> actionChanges;
    std::uint32_t finishCalls = 0;
    std::optional<bool> sourceFinished;
    source.setDragHandlers(
        {}, {}, {},
        [&sourceFinished, &finishCalls](bool success) {
          sourceFinished = success;
          ++finishCalls;
        },
        [&source, &actionChanges] { actionChanges.push_back(source.dragAction()); }
    );
    std::optional<NativeDataTransfer::Payload> received;
    std::optional<bool> targetFinished;
    bool targetMotion = false;
    target.setDragHandlers(
        [&targetClient, &targetMotion](wl_surface* surface, float, float, std::uint32_t actions) {
          targetMotion = true;
          return surface == targetClient.surface && (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
              ? static_cast<std::uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
              : 0U;
        },
        {},
        [&received, &targetFinished](std::uint32_t action) {
          TEST_CHECK(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
          return NativeDataTransfer::DropReceiver([&received, &targetFinished](auto payload, auto finish) {
            received = std::move(payload);
            finish(true);
            targetFinished = true;
          });
        },
        {}
    );

    const NativeDataTransfer::Payload payload{{"text/uri-list", "file:///tmp/wlroots-borrowed-icon\r\n"}};
    TEST_CHECK(source.startDrag(payload, sourceClient.surface, serial, nullptr, externalIcon));
    TEST_CHECK(source.dragIconVisible());
    server.pumpUntil([&server] { return server.dragStarted(); });
    TEST_CHECK(server.dragOrigin() == server.sourceSurface());
    server.pumpUntil([&server] { return server.dragIconCreated(); });
    TEST_CHECK(server.dragIconSurface() == server.sourceClientSurface(externalIcon));
    TEST_CHECK(server.dragIconSurface() != server.dragOrigin());

    wlr_seat_pointer_notify_enter(server.seat(), server.targetSurface(), 9.0, 11.0);
    wlr_seat_pointer_notify_motion(server.seat(), 2, 9.0, 11.0);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&targetMotion] { return targetMotion; });
    server.pumpUntil([&server] { return server.dragAcceptedCopy(); });
    server.pumpUntil([&source] { return source.dragAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; });
    TEST_CHECK(std::ranges::find(actionChanges, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != actionChanges.end());

    wlr_seat_pointer_notify_button(server.seat(), 3, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&source] { return source.dragDropped(); });
    TEST_CHECK(source.dragging());
    TEST_CHECK(!source.dragIconVisible());
    TEST_CHECK(!sourceFinished.has_value());
    server.pumpUntil([&targetFinished] { return targetFinished.has_value(); });
    server.pumpUntil([&sourceFinished] { return sourceFinished.has_value(); });
    server.pumpUntil([&server] { return server.dragEnded(); });
    TEST_CHECK(*sourceFinished && *targetFinished);
    TEST_CHECK(finishCalls == 1);
    TEST_CHECK(!source.dragging() && source.dragAction() == 0 && !source.busy());
    TEST_CHECK(received && received->at("text/uri-list") == payload.at("text/uri-list"));
    TEST_CHECK(server.sourceClientSurface(externalIcon) != nullptr); // Borrowed; the host owns destruction.
    source.cancelDrag(); // Finished sources are removed once; late cancellation is a no-op.
    TEST_CHECK(finishCalls == 1);
    wl_surface_destroy(externalIcon);
  }

  void testWlrootsBorrowedIconCancel() {
    WlrootsServer server;
    auto& sourceClient = server.sourceClient();
    NativeDataTransfer source;
    NativeDataTransfer target;
    server.attach(source, target);
    source.bind(sourceClient.manager, sourceClient.seat);
    target.bind(server.targetClient().manager, server.targetClient().seat);
    auto* externalIcon = wl_compositor_create_surface(sourceClient.compositor);
    TEST_CHECK(externalIcon != nullptr);

    wlr_seat_pointer_notify_enter(server.seat(), server.sourceSurface(), 2.0, 3.0);
    wlr_seat_pointer_notify_frame(server.seat());
    const auto serial = wlr_seat_pointer_notify_button(server.seat(), 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server.seat());
    server.pumpUntil([&sourceClient] { return sourceClient.buttonSerial != 0; });
    TEST_CHECK(sourceClient.buttonSerial == serial && serial != 0);

    std::uint32_t finishCalls = 0;
    std::optional<bool> finished;
    source.setDragHandlers({}, {}, {}, [&finished, &finishCalls](bool success) {
      finished = success;
      ++finishCalls;
    });
    const NativeDataTransfer::Payload payload{{"text/uri-list", "file:///tmp/wlroots-borrowed-icon-cancel\r\n"}};
    TEST_CHECK(source.startDrag(payload, sourceClient.surface, serial, nullptr, externalIcon));
    server.pumpUntil([&server] { return server.dragStarted() && server.dragIconCreated(); });
    TEST_CHECK(server.dragOrigin() == server.sourceSurface());
    TEST_CHECK(server.dragIconSurface() == server.sourceClientSurface(externalIcon));
    TEST_CHECK(source.dragIconVisible());

    source.cancelDrag();
    TEST_CHECK(finished.has_value() && !*finished);
    TEST_CHECK(finishCalls == 1);
    TEST_CHECK(!source.dragging() && !source.dragIconVisible() && !source.busy());
    server.pumpUntil([&server] { return server.dragEnded(); });
    TEST_CHECK(server.sourceClientSurface(externalIcon) != nullptr);
    source.cancelDrag();
    TEST_CHECK(finishCalls == 1);
    wl_surface_destroy(externalIcon);
  }

} // namespace

int main() {
  testWlrootsRoundTrip();
  testWlrootsIconCancel();
  testWlrootsBorrowedIconLifecycle();
  testWlrootsBorrowedIconCancel();
  return 0;
}
