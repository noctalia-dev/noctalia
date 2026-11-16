#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "tests/test_check.h"
#include "text-input-test-server-protocol.h"
#include "ui/controls/input.h"
#include "wayland/text_input_service.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <wayland-client.h>
#include <wayland-server.h>

namespace {
  class Peer {
    wl_display* m_server = wl_display_create();
    wl_display* m_client = nullptr;
    wl_registry* m_registry = nullptr;
    wl_compositor* m_compositor = nullptr;
    wl_seat* m_seat = nullptr;
    zwp_text_input_manager_v3* m_manager = nullptr;
    wl_surface* m_surface = nullptr;
    wl_resource* m_surfaceResource = nullptr;
    wl_resource* m_textInput = nullptr;
    TextInputService m_service;

    static Peer& owner(wl_resource* resource) { return *static_cast<Peer*>(wl_resource_get_user_data(resource)); }
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    static void bindCompositor(wl_client* client, void* data, std::uint32_t, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &wl_compositor_interface, 1, id);
      static const auto implementation = [] {
        struct wl_compositor_interface result{};
        result.create_surface = [](wl_client* client, wl_resource* compositor, std::uint32_t id) {
          auto& peer = owner(compositor);
          peer.m_surfaceResource = wl_resource_create(client, &wl_surface_interface, 1, id);
          static const auto surfaceImplementation = [] {
            struct wl_surface_interface surface{};
            surface.destroy = destroy;
            surface.commit = [](wl_client*, wl_resource*) {};
            return surface;
          }();
          wl_resource_set_implementation(peer.m_surfaceResource, &surfaceImplementation, &peer, nullptr);
        };
        return result;
      }();
      wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }
    static void bindManager(wl_client* client, void* data, std::uint32_t, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface, 1, id);
      static const struct zwp_text_input_manager_v3_interface implementation{
          .destroy = destroy,
          .get_text_input = [](wl_client* client, wl_resource* manager, std::uint32_t id, wl_resource*) {
            auto& peer = owner(manager);
            peer.m_textInput = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
            static const auto inputImplementation = [] {
              struct zwp_text_input_v3_interface input{};
              input.destroy = destroy;
              input.enable = [](wl_client*, wl_resource* resource) { ++owner(resource).enables; };
              input.disable = [](wl_client*, wl_resource* resource) { ++owner(resource).disables; };
              input.set_surrounding_text = [](wl_client*, wl_resource*, const char*, std::int32_t, std::int32_t) {};
              input.set_text_change_cause = [](wl_client*, wl_resource*, std::uint32_t) {};
              input.set_content_type = [](wl_client*, wl_resource*, std::uint32_t, std::uint32_t) {};
              input.set_cursor_rectangle = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t,
                                              std::int32_t) {};
              input.commit = [](wl_client*, wl_resource* resource) { ++owner(resource).commits; };
              return input;
            }();
            wl_resource_set_implementation(peer.m_textInput, &inputImplementation, &peer, nullptr);
          }
      };
      wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

  public:
    std::uint32_t enables = 0, disables = 0, commits = 0;

    Peer() {
      TEST_CHECK(m_server != nullptr);
      wl_global_create(m_server, &wl_compositor_interface, 1, this, bindCompositor);
      wl_global_create(m_server, &zwp_text_input_manager_v3_interface, 1, this, bindManager);
      wl_global_create(
          m_server, &wl_seat_interface, 1, nullptr, [](wl_client* client, void*, std::uint32_t, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_seat_interface, 1, id);
            wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
            wl_seat_send_capabilities(resource, 0);
          }
      );
      int sockets[2];
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
      TEST_CHECK(wl_client_create(m_server, sockets[0]) != nullptr);
      m_client = wl_display_connect_to_fd(sockets[1]);
      TEST_CHECK(m_client != nullptr);
      m_registry = wl_display_get_registry(m_client);
      static const wl_registry_listener registry{
          .global =
              [](void* data, wl_registry* registry, std::uint32_t id, const char* type, std::uint32_t) {
                auto& peer = *static_cast<Peer*>(data);
                if (std::string_view(type) == wl_compositor_interface.name) {
                  peer.m_compositor =
                      static_cast<wl_compositor*>(wl_registry_bind(registry, id, &wl_compositor_interface, 1));
                } else if (std::string_view(type) == wl_seat_interface.name) {
                  peer.m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, id, &wl_seat_interface, 1));
                } else if (std::string_view(type) == zwp_text_input_manager_v3_interface.name) {
                  peer.m_manager = static_cast<zwp_text_input_manager_v3*>(
                      wl_registry_bind(registry, id, &zwp_text_input_manager_v3_interface, 1)
                  );
                }
              },
          .global_remove = [](void*, wl_registry*, std::uint32_t) {}
      };
      wl_registry_add_listener(m_registry, &registry, this);
      until([this] { return m_compositor && m_seat && m_manager; });
      m_surface = wl_compositor_create_surface(m_compositor);
      TEST_CHECK(m_service.bind(m_manager, m_seat));
      until([this] { return m_surfaceResource && m_textInput; });
      zwp_text_input_v3_send_enter(m_textInput, m_surfaceResource);
      pump();
    }
    ~Peer() {
      m_service.cleanup();
      wl_surface_destroy(m_surface);
      wl_proxy_destroy(reinterpret_cast<wl_proxy*>(m_manager));
      wl_seat_destroy(m_seat);
      wl_compositor_destroy(m_compositor);
      wl_registry_destroy(m_registry);
      wl_display_disconnect(m_client);
      wl_display_destroy_clients(m_server);
      wl_display_destroy(m_server);
    }
    void pump() {
      TEST_CHECK(wl_display_flush(m_client) >= 0);
      TEST_CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(m_server), 0) >= 0);
      wl_display_flush_clients(m_server);
      pollfd fd{.fd = wl_display_get_fd(m_client), .events = POLLIN, .revents = 0};
      if (::poll(&fd, 1, 0) > 0) {
        TEST_CHECK(wl_display_dispatch(m_client) >= 0);
      }
    }
    template <typename Predicate> void until(Predicate ready) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!ready() && std::chrono::steady_clock::now() < deadline) {
        pump();
      }
      TEST_CHECK(ready());
    }
    void edit(const char* preedit, const char* commit = nullptr) {
      zwp_text_input_v3_send_preedit_string(m_textInput, preedit, -1, -1);
      if (commit != nullptr) {
        zwp_text_input_v3_send_commit_string(m_textInput, commit);
      }
      zwp_text_input_v3_send_done(m_textInput, commits);
      pump();
      pump();
    }
    TextInputService& service() { return m_service; }
    wl_surface* surface() const { return m_surface; }
  };

  struct Forwarder final : TextInputClient {
    explicit Forwarder(TextInputService& service) : service(service) {}
    ~Forwarder() override { service.clearFocusedClient(this); }
    TextInputState textInputState() const override { return {}; }
    void textInputApplyEdit(const TextInputEdit& edit) override { apply(edit); }
    void textInputResetPreedit() override { ++resets; }
    void textInputActivated(TextInputService&) override { ++activations; }
    void textInputDeactivated(TextInputService&) override { ++deactivations; }
    TextInputService& service;
    std::function<void(const TextInputEdit&)> apply;
    int resets = 0, activations = 0, deactivations = 0;
  };
} // namespace

int main() {
  Peer peer;
  Node root;
  InputDispatcher dispatcher;
  Forwarder from(peer.service());
  auto source = std::make_unique<InputArea>();
  source->setFocusable(true);
  source->setTextInputClient(&from);
  auto* sourceArea = static_cast<InputArea*>(root.addChild(std::move(source)));
  auto targetNode = std::make_unique<Input>();
  auto* target = static_cast<Input*>(root.addChild(std::move(targetNode)));
  dispatcher.setSceneRoot(&root);
  dispatcher.setTextInputContext(peer.surface(), &peer.service());
  dispatcher.setFocus(sourceArea);
  peer.until([&] { return peer.enables == 1; });
  TEST_CHECK(!peer.service().transferFocusedClient(peer.surface(), &from, target));
  from.apply = [&](const TextInputEdit& edit) {
    TEST_CHECK(!peer.service().transferFocusedClient(nullptr, &from, target));
    TEST_CHECK(!peer.service().transferFocusedClient(peer.surface(), target, &from));
    TEST_CHECK(!peer.service().transferFocusedClient(peer.surface(), &from, &from));
    dispatcher.setFocus(target->inputArea(), true);
    target->textInputApplyEdit(edit);
  };
  peer.edit("composing");
  TEST_CHECK(target->composing() && target->value() == "composing");
  TEST_CHECK(dispatcher.focusedArea() == target->inputArea());
  TEST_CHECK(from.resets == 0 && from.deactivations == 1);
  TEST_CHECK(peer.enables == 1 && peer.disables == 0);
  peer.edit("", "committed");
  TEST_CHECK(!target->composing() && target->value() == "committed");
  dispatcher.setFocus(sourceArea, true);
  peer.until([&] { return peer.enables == 2; });
  TEST_CHECK(peer.disables == 1);
  from.apply = [](const TextInputEdit&) { throw std::runtime_error("edit failure"); };
  bool threw = false;
  try {
    peer.service().handleDone(peer.commits);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TEST_CHECK(threw);
  TEST_CHECK(!peer.service().transferFocusedClient(peer.surface(), &from, target));
  dispatcher.setFocus(nullptr);
  std::println("text input forwarding checks passed");
  return 0;
}
