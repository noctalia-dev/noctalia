#include "tests/test_check.h"
#include "wayland/native_data_transfer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace {

  class TestServer {
  public:
    struct OfferData {
      TestServer* owner;
      std::map<std::string, std::string> payload;
      bool stall = false;
      int stalledFd = -1;
      wl_resource* resource = nullptr;
    };

    TestServer() {
      int sockets[2] = {-1, -1};
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
      m_display = wl_display_create();
      TEST_CHECK(m_display != nullptr);
      TEST_CHECK(wl_client_create(m_display, sockets[0]) != nullptr);
      m_client = wl_display_connect_to_fd(sockets[1]);
      TEST_CHECK(m_client != nullptr);
      TEST_CHECK(wl_global_create(m_display, &wl_data_device_manager_interface, 3, this, &bindManager) != nullptr);
      TEST_CHECK(wl_global_create(m_display, &wl_seat_interface, 1, this, &bindSeat) != nullptr);
      TEST_CHECK(wl_global_create(m_display, &wl_compositor_interface, 4, this, &bindCompositor) != nullptr);

      m_registry = wl_display_get_registry(m_client);
      static const wl_registry_listener listener = {.global = &global, .global_remove = &globalRemove};
      TEST_CHECK(wl_registry_add_listener(m_registry, &listener, this) == 0);
      pumpUntil([this] { return m_manager && m_seat && m_compositor; });
      m_origin = wl_compositor_create_surface(m_compositor);
      TEST_CHECK(m_origin != nullptr);
      pumpUntil([this] { return m_surfaceResource != nullptr; });
    }

    ~TestServer() {
      for (auto& offer : m_offers)
        if (offer->stalledFd >= 0)
          close(offer->stalledFd);
      if (m_registry)
        wl_registry_destroy(m_registry);
      if (m_origin)
        wl_surface_destroy(m_origin);
      if (m_compositor)
        wl_compositor_destroy(m_compositor);
      if (m_manager)
        wl_data_device_manager_destroy(m_manager);
      if (m_seat)
        wl_seat_destroy(m_seat);
      if (m_client)
        wl_display_disconnect(m_client);
      if (m_display) {
        wl_display_destroy_clients(m_display);
        wl_display_destroy(m_display);
      }
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    [[nodiscard]] wl_data_device_manager* manager() const { return m_manager; }
    [[nodiscard]] wl_seat* seat() const { return m_seat; }
    [[nodiscard]] wl_surface* origin() const { return m_origin; }
    [[nodiscard]] wl_resource* surfaceResource() const { return m_surfaceResource; }

    void pump() {
      TEST_CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(m_display), 0) >= 0 || errno == EINTR);
      wl_display_flush_clients(m_display);

      const int displayFd = wl_display_get_fd(m_client);
      if (wl_display_prepare_read(m_client) == 0) {
        static_cast<void>(wl_display_flush(m_client));
        pollfd displayPoll{displayFd, POLLIN, 0};
        const int result = poll(&displayPoll, 1, 0);
        if (result > 0 && (displayPoll.revents & POLLIN))
          TEST_CHECK(wl_display_read_events(m_client) == 0);
        else
          wl_display_cancel_read(m_client);
      }
      TEST_CHECK(wl_display_dispatch_pending(m_client) >= 0);

      if (m_transfer) {
        std::vector<pollfd> fds;
        const auto start = m_transfer->addPollFds(fds);
        if (!fds.empty())
          static_cast<void>(poll(fds.data(), fds.size(), 0));
        m_transfer->dispatch(fds, start);
      }
    }

    template <typename Predicate>
    void pumpUntil(Predicate predicate, std::source_location location = std::source_location::current()) {
      const auto waitNumber = ++m_waitCount;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        pump();
        usleep(1000);
      }
      if (!predicate()) {
        std::fprintf(stderr, "Wayland test wait %u timed out in %s\n", waitNumber, location.function_name());
        TEST_CHECK(false);
      }
    }

    void attach(NativeDataTransfer& transfer) { m_transfer = &transfer; }

    wl_resource* deviceResource() const { return m_device; }
    wl_resource* sourceResource() const { return m_source; }
    wl_resource* dragSourceResource() const { return m_dragSource; }
    wl_resource* dragOriginResource() const { return m_dragOrigin; }
    std::uint32_t dragSerial() const { return m_dragSerial; }
    std::uint32_t sourceActionMask() const { return m_sourceActionMask; }
    const std::vector<std::string>& sourceMimes() const { return m_sourceMimes; }
    void clearSourceMimes() { m_sourceMimes.clear(); }
    std::uint32_t acceptSerial() const { return m_acceptSerial; }
    const std::string& acceptedMime() const { return m_acceptedMime; }
    std::uint32_t receivedSourceActions() const { return m_receivedSourceActions; }
    std::uint32_t preferredAction() const { return m_preferredAction; }
    bool offerFinished() const { return m_offerFinished; }
    [[nodiscard]] bool stalledReceiveStarted() const { return !m_offers.empty() && m_offers.back()->stalledFd >= 0; }

    void offerSelection(std::map<std::string, std::string> payload, bool stall = false) {
      TEST_CHECK(m_device != nullptr);
      auto data = std::make_unique<OfferData>(OfferData{this, std::move(payload), stall, -1, nullptr});
      auto* resource = wl_resource_create(m_serverClient, &wl_data_offer_interface, 3, 0);
      TEST_CHECK(resource != nullptr);
      data->resource = resource;
      static const struct wl_data_offer_interface implementation = {
          .accept = [](wl_client*, wl_resource*, std::uint32_t, const char*) {},
          .receive = &receive,
          .destroy = &destroyResource,
          .finish = [](wl_client*, wl_resource*) {},
          .set_actions = [](wl_client*, wl_resource*, std::uint32_t, std::uint32_t) {},
      };
      wl_resource_set_implementation(resource, &implementation, data.get(), nullptr);
      auto* raw = data.get();
      m_offers.push_back(std::move(data));
      wl_data_device_send_data_offer(m_device, resource);
      for (const auto& [mime, _] : raw->payload)
        wl_data_offer_send_offer(resource, mime.c_str());
      wl_data_device_send_selection(m_device, resource);
    }

    wl_resource* offerDrag(std::map<std::string, std::string> payload, std::uint32_t sourceActions) {
      TEST_CHECK(m_device != nullptr);
      auto data = std::make_unique<OfferData>(OfferData{this, std::move(payload), false, -1, nullptr});
      auto* resource = createOffer(std::move(data));
      wl_data_device_send_data_offer(m_device, resource);
      sendOfferMimes(resource);
      wl_data_offer_send_source_actions(resource, sourceActions);
      wl_data_device_send_enter(
          m_device, 41, m_surfaceResource, wl_fixed_from_double(12.5), wl_fixed_from_double(7.25), resource
      );
      wl_display_flush_clients(m_display);
      return resource;
    }

    void sendDragMotion(double x, double y) {
      wl_data_device_send_motion(m_device, 42, wl_fixed_from_double(x), wl_fixed_from_double(y));
      wl_display_flush_clients(m_display);
    }
    void sendDragLeave() {
      wl_data_device_send_leave(m_device);
      wl_display_flush_clients(m_display);
    }
    void sendDragDrop() {
      wl_data_device_send_drop(m_device);
      wl_display_flush_clients(m_display);
    }
    void selectDragAction(std::uint32_t action) {
      TEST_CHECK(m_dragOffer != nullptr);
      wl_data_offer_send_action(m_dragOffer, action);
      wl_display_flush_clients(m_display);
    }
    void sendOfferMimes(wl_resource* resource) {
      const auto it =
          std::ranges::find_if(m_offers, [resource](const auto& offer) { return offer->resource == resource; });
      TEST_CHECK(it != m_offers.end());
      for (const auto& [mime, _] : (*it)->payload)
        wl_data_offer_send_offer(resource, mime.c_str());
    }
    void sendSourceAction(std::uint32_t action) {
      TEST_CHECK(m_dragSource != nullptr);
      sendSourceAction(m_dragSource, action);
    }
    void sendSourceAction(wl_resource* source, std::uint32_t action) {
      TEST_CHECK(source != nullptr);
      wl_data_source_send_action(source, action);
      wl_display_flush_clients(m_display);
    }
    void sendDropPerformed() {
      TEST_CHECK(m_dragSource != nullptr);
      sendDropPerformed(m_dragSource);
    }
    void sendDropPerformed(wl_resource* source) {
      TEST_CHECK(source != nullptr);
      wl_data_source_send_dnd_drop_performed(source);
      wl_display_flush_clients(m_display);
    }
    void sendDndFinished() {
      TEST_CHECK(m_dragSource != nullptr);
      sendDndFinished(m_dragSource);
    }
    void sendDndFinished(wl_resource* source) {
      TEST_CHECK(source != nullptr);
      wl_data_source_send_dnd_finished(source);
      wl_display_flush_clients(m_display);
    }

    void cancelSource() {
      TEST_CHECK(m_source != nullptr);
      wl_data_source_send_cancelled(m_source);
      wl_display_flush_clients(m_display);
    }

    std::string readFromSource(std::string_view mime, bool closeReader = false) {
      return readFromSource(m_source, mime, closeReader);
    }

    std::string readFromSource(wl_resource* source, std::string_view mime, bool closeReader = false) {
      TEST_CHECK(source != nullptr);
      int pipeFds[2] = {-1, -1};
      TEST_CHECK(pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) == 0);
      wl_data_source_send_send(source, std::string(mime).c_str(), pipeFds[1]);
      close(pipeFds[1]);
      if (closeReader) {
        close(pipeFds[0]);
        for (int i = 0; i < 10; ++i) {
          pump();
          usleep(1000);
        }
        return {};
      }

      std::string bytes;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline) {
        pump();
        std::array<char, 256> buffer{};
        const auto count = read(pipeFds[0], buffer.data(), buffer.size());
        if (count > 0)
          bytes.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count == 0) {
          close(pipeFds[0]);
          return bytes;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          close(pipeFds[0]);
          TEST_CHECK(false);
        }
      }
      close(pipeFds[0]);
      TEST_CHECK(false);
      return {};
    }

  private:
    static void destroyResource(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void bindManager(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* self = static_cast<TestServer*>(data);
      self->m_serverClient = client;
      auto* resource = wl_resource_create(client, &wl_data_device_manager_interface, static_cast<int>(version), id);
      TEST_CHECK(resource != nullptr);
      static const struct wl_data_device_manager_interface implementation = {
          .create_data_source = &createSource,
          .get_data_device = &getDataDevice,
      };
      wl_resource_set_implementation(resource, &implementation, self, nullptr);
    }

    static void bindSeat(wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
      TEST_CHECK(resource != nullptr);
      static const struct wl_seat_interface implementation = {.release = &destroyResource};
      wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
      wl_seat_send_capabilities(resource, 0);
    }

    static void bindCompositor(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &wl_compositor_interface, static_cast<int>(version), id);
      TEST_CHECK(resource != nullptr);
      static const struct wl_compositor_interface implementation = {
          .create_surface = &createSurface,
          .create_region =
              [](wl_client* owner, wl_resource*, std::uint32_t regionId) {
                auto* region = wl_resource_create(owner, &wl_region_interface, 1, regionId);
                TEST_CHECK(region != nullptr);
                static const struct wl_region_interface regionImplementation = {
                    .destroy = &destroyResource,
                    .add = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {},
                    .subtract = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {},
                };
                wl_resource_set_implementation(region, &regionImplementation, nullptr, nullptr);
              },
          .release = &destroyResource,
      };
      wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    static void createSurface(wl_client* client, wl_resource* compositor, std::uint32_t id) {
      auto* self = static_cast<TestServer*>(wl_resource_get_user_data(compositor));
      auto* resource = wl_resource_create(client, &wl_surface_interface, 4, id);
      TEST_CHECK(resource != nullptr);
      self->m_surfaceResource = resource;
      static const struct wl_surface_interface implementation = {
          .destroy = &destroyResource,
          .attach = [](wl_client*, wl_resource*, wl_resource*, std::int32_t, std::int32_t) {},
          .damage = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {},
          .frame = [](wl_client*, wl_resource*, std::uint32_t) {},
          .set_opaque_region = [](wl_client*, wl_resource*, wl_resource*) {},
          .set_input_region = [](wl_client*, wl_resource*, wl_resource*) {},
          .commit = [](wl_client*, wl_resource*) {},
          .set_buffer_transform = [](wl_client*, wl_resource*, std::int32_t) {},
          .set_buffer_scale = [](wl_client*, wl_resource*, std::int32_t) {},
          .damage_buffer = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {},
          .offset = [](wl_client*, wl_resource*, std::int32_t, std::int32_t) {},
      };
      wl_resource_set_implementation(resource, &implementation, self, nullptr);
    }

    static void createSource(wl_client* client, wl_resource* manager, std::uint32_t id) {
      auto* self = static_cast<TestServer*>(wl_resource_get_user_data(manager));
      auto* sourceResource = wl_resource_create(client, &wl_data_source_interface, 3, id);
      TEST_CHECK(sourceResource != nullptr);
      static const struct wl_data_source_interface implementation = {
          .offer =
              [](wl_client*, wl_resource* source, const char* mime) {
                auto* self = static_cast<TestServer*>(wl_resource_get_user_data(source));
                self->m_sourceMimes.emplace_back(mime ? mime : "");
              },
          .destroy = &destroyResource,
          .set_actions =
              [](wl_client*, wl_resource* source, std::uint32_t actions) {
                auto* self = static_cast<TestServer*>(wl_resource_get_user_data(source));
                self->m_sourceActionMask = actions;
              },
      };
      wl_resource_set_implementation(sourceResource, &implementation, self, nullptr);
      self->m_sources.push_back(sourceResource);
    }

    static void getDataDevice(wl_client* client, wl_resource* manager, std::uint32_t id, wl_resource*) {
      auto* self = static_cast<TestServer*>(wl_resource_get_user_data(manager));
      self->m_device = wl_resource_create(client, &wl_data_device_interface, 3, id);
      TEST_CHECK(self->m_device != nullptr);
      static const struct wl_data_device_interface implementation = {
          .start_drag =
              [](wl_client*, wl_resource* device, wl_resource* source, wl_resource* origin, wl_resource* icon,
                 std::uint32_t serial) {
                auto* self = static_cast<TestServer*>(wl_resource_get_user_data(device));
                self->m_dragSource = source;
                self->m_dragOrigin = origin;
                self->m_dragIcon = icon;
                self->m_dragSerial = serial;
              },
          .set_selection =
              [](wl_client*, wl_resource* device, wl_resource* source, std::uint32_t) {
                auto* self = static_cast<TestServer*>(wl_resource_get_user_data(device));
                self->m_source = source;
              },
          .release = &destroyResource,
      };
      wl_resource_set_implementation(self->m_device, &implementation, self, nullptr);
    }

    static wl_resource* createOffer(std::unique_ptr<OfferData> data) {
      auto* owner = data->owner;
      auto* resource = wl_resource_create(owner->m_serverClient, &wl_data_offer_interface, 3, 0);
      TEST_CHECK(resource != nullptr);
      data->resource = resource;
      static const struct wl_data_offer_interface implementation = {
          .accept =
              [](wl_client*, wl_resource* offer, std::uint32_t serial, const char* mime) {
                auto* data = static_cast<OfferData*>(wl_resource_get_user_data(offer));
                data->owner->m_acceptSerial = serial;
                data->owner->m_acceptedMime = mime ? mime : "";
              },
          .receive = &receive,
          .destroy = &destroyResource,
          .finish =
              [](wl_client*, wl_resource* offer) {
                auto* data = static_cast<OfferData*>(wl_resource_get_user_data(offer));
                data->owner->m_offerFinished = true;
              },
          .set_actions =
              [](wl_client*, wl_resource* offer, std::uint32_t sourceActions, std::uint32_t preferred) {
                auto* data = static_cast<OfferData*>(wl_resource_get_user_data(offer));
                data->owner->m_receivedSourceActions = sourceActions;
                data->owner->m_preferredAction = preferred;
                data->owner->m_dragOffer = offer;
                if (preferred && (sourceActions & preferred))
                  wl_data_offer_send_action(offer, preferred);
                else
                  wl_data_offer_send_action(offer, 0);
              },
      };
      wl_resource_set_implementation(resource, &implementation, data.get(), nullptr);
      owner->m_dragOffer = resource;
      owner->m_offers.push_back(std::move(data));
      return resource;
    }

    static void receive(wl_client*, wl_resource* resource, const char* mime, int fd) {
      auto* data = static_cast<OfferData*>(wl_resource_get_user_data(resource));
      const auto it = data->payload.find(mime ? mime : "");
      if (it == data->payload.end() || data->stall) {
        data->stalledFd = fd;
        return;
      }
      const auto& bytes = it->second;
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count > 0)
          offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR)
          continue;
        else
          break;
      }
      close(fd);
    }

    static void
    global(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
      auto* self = static_cast<TestServer*>(data);
      if (std::string_view(interface) == wl_data_device_manager_interface.name)
        self->m_manager = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
        );
      else if (std::string_view(interface) == wl_seat_interface.name)
        self->m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
      else if (std::string_view(interface) == wl_compositor_interface.name)
        self->m_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U))
        );
    }
    static void globalRemove(void*, wl_registry*, std::uint32_t) {}

    wl_display* m_display = nullptr;
    wl_client* m_serverClient = nullptr;
    wl_display* m_client = nullptr;
    wl_registry* m_registry = nullptr;
    wl_data_device_manager* m_manager = nullptr;
    wl_seat* m_seat = nullptr;
    wl_compositor* m_compositor = nullptr;
    wl_surface* m_origin = nullptr;
    wl_resource* m_device = nullptr;
    wl_resource* m_source = nullptr;
    wl_resource* m_dragSource = nullptr;
    wl_resource* m_dragOffer = nullptr;
    wl_resource* m_dragOrigin = nullptr;
    wl_resource* m_dragIcon = nullptr;
    wl_resource* m_surfaceResource = nullptr;
    std::vector<wl_resource*> m_sources;
    std::vector<std::string> m_sourceMimes;
    std::uint32_t m_sourceActionMask = 0;
    std::uint32_t m_dragSerial = 0;
    std::uint32_t m_acceptSerial = 0;
    std::uint32_t m_receivedSourceActions = 0;
    std::uint32_t m_preferredAction = 0;
    std::string m_acceptedMime;
    bool m_offerFinished = false;
    std::vector<std::unique_ptr<OfferData>> m_offers;
    NativeDataTransfer* m_transfer = nullptr;
    unsigned m_waitCount = 0;
  };

  void testClipboardExport() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    TEST_CHECK(transfer.available());
    TEST_CHECK(transfer.setSelection(
        {
            {"text/uri-list", "file:///tmp/item\r\n"},
            {"application/x-kde-cutselection", "1"},
            {"x-special/gnome-copied-files", "cut\nfile:///tmp/item\n"},
        },
        7
    ));
    const auto ownedGeneration = transfer.selectionGeneration();
    std::optional<NativeDataTransfer::Payload> immediateRead;
    transfer.readSelection([&immediateRead](auto payload) { immediateRead = std::move(payload); });
    TEST_CHECK(immediateRead.has_value());
    TEST_CHECK(immediateRead->at("text/uri-list") == "file:///tmp/item\r\n");
    transfer.consumeSelection(ownedGeneration - 1);
    immediateRead.reset();
    transfer.readSelection([&immediateRead](auto payload) { immediateRead = std::move(payload); });
    TEST_CHECK(immediateRead->at("text/uri-list") == "file:///tmp/item\r\n");

    transfer.consumeSelection(ownedGeneration);
    immediateRead.reset();
    transfer.readSelection([&immediateRead](auto payload) { immediateRead = std::move(payload); });
    TEST_CHECK(immediateRead.has_value());
    TEST_CHECK(immediateRead->at("text/uri-list").empty());
    TEST_CHECK(immediateRead->at("x-special/gnome-copied-files") == "copy\n");
    TEST_CHECK(immediateRead->at("application/x-kde-cutselection") == "0");

    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });
    server.offerSelection({{"text/uri-list", "file:///tmp/compositor-echo\r\n"}});
    server.pumpUntil([&transfer, ownedGeneration] { return transfer.selectionGeneration() > ownedGeneration; });
    std::optional<NativeDataTransfer::Payload> compositorRead;
    transfer.readSelection([&compositorRead](auto payload) { compositorRead = std::move(payload); });
    server.pumpUntil([&compositorRead] { return compositorRead.has_value(); });
    TEST_CHECK(compositorRead->at("text/uri-list") == "file:///tmp/compositor-echo\r\n");

    server.pumpUntil([&server] { return server.sourceResource() != nullptr; });
    TEST_CHECK(transfer.ownsSelection());
    TEST_CHECK(server.readFromSource("text/uri-list").empty());
    TEST_CHECK(server.readFromSource("application/x-kde-cutselection") == "0");
    TEST_CHECK(server.readFromSource("x-special/gnome-copied-files") == "copy\n");

    // The peer may close its read end before the source responds. A failed write
    // must be contained without changing process-wide SIGPIPE handling.
    static_cast<void>(server.readFromSource("text/uri-list", true));
    TEST_CHECK(transfer.ownsSelection());
    const auto beforeCancellation = transfer.selectionGeneration();
    server.cancelSource();
    server.pumpUntil([&transfer] { return !transfer.ownsSelection(); });
    TEST_CHECK(transfer.selectionGeneration() == beforeCancellation + 1);
  }

  void testClipboardImportAndReplacement() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    server.offerSelection({
        {"text/uri-list", "file:///tmp/from-offer%20one\r\n"},
        {"application/x-kde-cutselection", "1"},
        {"x-special/gnome-copied-files", "cut\nfile:///tmp/from-offer%20one\n"},
    });
    server.pumpUntil([&transfer] { return transfer.selectionGeneration() > 0; });
    std::optional<NativeDataTransfer::Payload> received;
    transfer.readSelection([&received](auto payload) { received = std::move(payload); });
    server.pumpUntil([&received] { return received.has_value(); });
    TEST_CHECK(received->at("text/uri-list") == "file:///tmp/from-offer%20one\r\n");
    TEST_CHECK(received->at("application/x-kde-cutselection") == "1");
    TEST_CHECK(!received->contains("x-special/gnome-copied-files"));

    received.reset();
    transfer.readSelection([&received](auto payload) { received = std::move(payload); }, true);
    server.pumpUntil([&received] { return received.has_value(); });
    TEST_CHECK(received->at("application/x-kde-cutselection") == "1");
    TEST_CHECK(received->at("x-special/gnome-copied-files") == "cut\nfile:///tmp/from-offer%20one\n");

    server.offerSelection({{"text/uri-list", "file:///tmp/slow\r\n"}}, true);
    server.pumpUntil([&transfer] { return transfer.selectionGeneration() > 1; });
    bool replacementRead = false;
    std::optional<NativeDataTransfer::Payload> replaced;
    transfer.readSelection([&](auto payload) {
      replacementRead = true;
      replaced = std::move(payload);
    });
    server.pumpUntil([&server] { return server.stalledReceiveStarted(); });
    server.offerSelection({{"text/uri-list", "file:///tmp/new-selection\r\n"}});
    server.pumpUntil([&replacementRead] { return replacementRead; });
    TEST_CHECK(!replaced.has_value());
  }

  void testTextClipboard() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&] { return server.deviceResource() != nullptr; });
    TEST_CHECK(!transfer.selectionOffersText());

    TEST_CHECK(transfer.setSelection({{"text/plain;charset=utf-8", "/tmp/owned path"}}, 7));
    TEST_CHECK(transfer.selectionOffersText());
    std::optional<std::string> result;
    transfer.readTextSelection([&](auto text) { result = std::move(text); });
    TEST_CHECK(result == "/tmp/owned path");

    const auto generation = transfer.selectionGeneration();
    server.offerSelection({{"UTF8_STRING", "/tmp/offered path"}});
    server.pumpUntil([&] { return transfer.selectionGeneration() > generation; });
    TEST_CHECK(transfer.selectionOffersText());
    result.reset();
    transfer.readTextSelection([&](auto text) { result = std::move(text); });
    server.pumpUntil([&] { return result.has_value(); });
    TEST_CHECK(result == "/tmp/offered path");
  }

  void testDragSourceProtocol() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    TEST_CHECK(transfer.setSelection({{"text/uri-list", "file:///tmp/clipboard\r\n"}}, 7));
    server.pumpUntil([&server] { return server.sourceResource() != nullptr; });
    TEST_CHECK(transfer.ownsSelection());
    server.clearSourceMimes();

    std::optional<bool> finished;
    transfer.setDragHandlers({}, {}, {}, [&finished](bool success) { finished = success; });
    const NativeDataTransfer::Payload dragPayload{
        {"text/uri-list", "file:///tmp/drag-source\r\n"},
        {"text/plain;charset=utf-8", "drag item"},
    };
    TEST_CHECK(transfer.startDrag(dragPayload, server.origin(), 29));
    TEST_CHECK(transfer.dragging());
    TEST_CHECK(!transfer.dragDropped());
    TEST_CHECK(transfer.busy());
    server.pumpUntil([&server] { return server.dragSourceResource() != nullptr; });

    TEST_CHECK(server.dragOriginResource() == server.surfaceResource());
    TEST_CHECK(server.dragSerial() == 29);
    TEST_CHECK(
        server.sourceActionMask() == (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
    );
    TEST_CHECK(std::ranges::find(server.sourceMimes(), "text/uri-list") != server.sourceMimes().end());
    TEST_CHECK(std::ranges::find(server.sourceMimes(), "text/plain;charset=utf-8") != server.sourceMimes().end());
    TEST_CHECK(server.readFromSource(server.dragSourceResource(), "text/uri-list") == "file:///tmp/drag-source\r\n");

    server.sendSourceAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    server.pump();
    server.sendDropPerformed();
    for (int i = 0; i < 3; ++i)
      server.pump();
    TEST_CHECK(transfer.dragging());
    TEST_CHECK(transfer.dragDropped());
    TEST_CHECK(!finished.has_value());
    server.sendDndFinished();
    server.pumpUntil([&finished] { return finished.has_value(); });
    TEST_CHECK(*finished);
    TEST_CHECK(!transfer.dragging());
    TEST_CHECK(!transfer.dragDropped());
    TEST_CHECK(transfer.ownsSelection());

    server.cancelSource();
    server.pumpUntil([&transfer] { return !transfer.ownsSelection(); });
  }

  void testDragDestinationAcceptAndFinish() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    std::uint32_t motionCount = 0;
    std::uint32_t dropCalls = 0;
    float lastX = 0;
    float lastY = 0;
    std::uint32_t supported = 0;
    std::optional<NativeDataTransfer::Payload> received;
    NativeDataTransfer::FinishDrop finishDrop;
    std::uint32_t droppedAction = 0;
    transfer.setDragHandlers(
        [&](wl_surface* surface, float x, float y, std::uint32_t actions) {
          TEST_CHECK(surface == server.origin());
          ++motionCount;
          lastX = x;
          lastY = y;
          supported = actions;
          return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
        },
        {},
        [&](std::uint32_t action) {
          ++dropCalls;
          droppedAction = action;
          return NativeDataTransfer::DropReceiver([&](auto payload, auto finish) {
            received = std::move(payload);
            finishDrop = std::move(finish);
          });
        },
        {}
    );

    auto* offer = server.offerDrag(
        {{"text/uri-list", "file:///tmp/drop-target%20item\r\n"}},
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
    );
    server.pumpUntil([&server] {
      return server.acceptSerial() == 41 && server.preferredAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    });
    server.pump();
    TEST_CHECK(motionCount == 1);
    TEST_CHECK(lastX == 12.5F && lastY == 7.25F);
    TEST_CHECK(supported == (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE));
    TEST_CHECK(server.acceptedMime() == "text/uri-list");
    TEST_CHECK(server.receivedSourceActions() == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    server.sendDragMotion(3.5, 9.25);
    server.pumpUntil([&motionCount] { return motionCount == 2; });
    TEST_CHECK(lastX == 3.5F && lastY == 9.25F);
    server.selectDragAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    server.pump();
    server.pump();
    server.sendDragDrop();
    server.sendDragDrop(); // Duplicate notifications must not enqueue another operation.
    server.pumpUntil([&received] { return received.has_value(); });
    TEST_CHECK(dropCalls == 1);
    TEST_CHECK(droppedAction == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    TEST_CHECK(received->at("text/uri-list") == "file:///tmp/drop-target%20item\r\n");
    TEST_CHECK(static_cast<bool>(finishDrop));
    TEST_CHECK(!server.offerFinished());
    finishDrop(true);
    server.pumpUntil([&server] { return server.offerFinished(); });
    static_cast<void>(offer);
  }

  void testDragDestinationRejectAndLeave() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    std::uint32_t dropCalls = 0;
    std::uint32_t leaveCalls = 0;
    transfer.setDragHandlers(
        [](wl_surface*, float, float, std::uint32_t) { return 0U; }, [&leaveCalls] { ++leaveCalls; },
        [&dropCalls](std::uint32_t) {
          ++dropCalls;
          return NativeDataTransfer::DropReceiver{};
        },
        {}
    );

    server.offerDrag({{"text/uri-list", "file:///tmp/rejected\r\n"}}, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    server.pumpUntil([&server] { return server.acceptSerial() == 41; });
    server.pump();
    TEST_CHECK(server.acceptedMime().empty());
    TEST_CHECK(server.preferredAction() == 0);
    server.sendDragDrop();
    for (int i = 0; i < 5; ++i)
      server.pump();
    TEST_CHECK(dropCalls == 0);
    TEST_CHECK(!server.offerFinished());

    server.sendDragLeave();
    server.pumpUntil([&leaveCalls] { return leaveCalls == 1; });
    TEST_CHECK(!server.offerFinished());
  }

  void testDragDestinationCancelDoesNotFinish() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    std::optional<NativeDataTransfer::Payload> received;
    NativeDataTransfer::FinishDrop finishDrop;
    transfer.setDragHandlers(
        [](wl_surface*, float, float, std::uint32_t) { return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; }, {},
        [&](std::uint32_t) {
          return NativeDataTransfer::DropReceiver([&](auto payload, auto finish) {
            received = std::move(payload);
            finishDrop = std::move(finish);
          });
        },
        {}
    );

    server.offerDrag({{"text/uri-list", "file:///tmp/cancelled-drop\r\n"}}, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    server.pumpUntil([&server] { return server.preferredAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; });
    server.pump();
    server.pump();
    server.sendDragDrop();
    server.pumpUntil([&received] { return received.has_value(); });
    TEST_CHECK(static_cast<bool>(finishDrop));
    finishDrop(false);
    for (int i = 0; i < 4; ++i)
      server.pump();
    TEST_CHECK(!server.offerFinished());
  }

  void testDragCancellationPreservesClipboardSelection() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });
    TEST_CHECK(transfer.setSelection({{"text/uri-list", "file:///tmp/clipboard\r\n"}}, 11));
    server.pumpUntil([&server] { return server.sourceResource() != nullptr; });

    std::optional<bool> finished;
    transfer.setDragHandlers({}, {}, {}, [&finished](bool success) { finished = success; });
    TEST_CHECK(transfer.startDrag({{"text/uri-list", "file:///tmp/cancel\r\n"}}, server.origin(), 31));
    server.pumpUntil([&server] { return server.dragSourceResource() != nullptr; });
    transfer.cancelDrag();
    TEST_CHECK(!transfer.dragging());
    TEST_CHECK(finished.has_value() && !*finished);
    TEST_CHECK(transfer.ownsSelection());
  }

  void testOverlappingDroppedDragSources() {
    TestServer server;
    NativeDataTransfer transfer;
    server.attach(transfer);
    transfer.bind(server.manager(), server.seat());
    server.pumpUntil([&server] { return server.deviceResource() != nullptr; });

    std::vector<bool> finishes;
    transfer.setDragHandlers({}, {}, {}, [&finishes](bool success) { finishes.push_back(success); });
    TEST_CHECK(transfer.startDrag({{"text/uri-list", "file:///tmp/first\r\n"}}, server.origin(), 51));
    server.pumpUntil([&server] { return server.dragSourceResource() != nullptr; });
    auto* firstSource = server.dragSourceResource();
    server.sendDropPerformed(firstSource);
    server.pumpUntil([&transfer] { return transfer.dragDropped(); });
    TEST_CHECK(transfer.busy());

    TEST_CHECK(transfer.startDrag({{"text/uri-list", "file:///tmp/second\r\n"}}, server.origin(), 52));
    server.pumpUntil([&server, firstSource] { return server.dragSourceResource() != firstSource; });
    auto* secondSource = server.dragSourceResource();
    TEST_CHECK(transfer.dragging());
    TEST_CHECK(!transfer.dragDropped());

    server.sendDndFinished(firstSource);
    for (int i = 0; i < 4; ++i)
      server.pump();
    TEST_CHECK(finishes.empty());
    TEST_CHECK(transfer.dragging());
    TEST_CHECK(transfer.busy());

    server.sendSourceAction(secondSource, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    server.pump();
    server.sendDropPerformed(secondSource);
    server.pumpUntil([&transfer] { return transfer.dragDropped(); });
    server.sendDndFinished(secondSource);
    server.pumpUntil([&finishes] { return finishes.size() == 1; });
    TEST_CHECK(finishes.front());
    TEST_CHECK(!transfer.dragging());
    TEST_CHECK(!transfer.busy());
  }

} // namespace

int main() {
  testClipboardExport();
  testClipboardImportAndReplacement();
  testTextClipboard();
  testDragSourceProtocol();
  testDragDestinationAcceptAndFinish();
  testDragDestinationRejectAndLeave();
  testDragDestinationCancelDoesNotFinish();
  testDragCancellationPreservesClipboardSelection();
  testOverlappingDroppedDragSources();
}
