#include "capture/toplevel_thumbnail_capture.h"
#include "core/timer_manager.h"
#include "ext-foreign-toplevel-list-v1-test-server-protocol.h"
#include "ext-image-capture-source-v1-test-server-protocol.h"
#include "ext-image-copy-capture-v1-test-server-protocol.h"
#include "test_check.h"
#include "wayland/wayland_connection.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <wayland-client.h>
#include <wayland-server.h>

namespace {
  class Server {
  public:
    std::atomic<int> sessions = 0;
    std::atomic<int> frames = 0;
    std::atomic<int> destroyedSessions = 0;
    std::atomic<int> uniqueBuffers = 0;
    std::atomic<bool> stopNextFrame = false;
    std::atomic<bool> rejectNextFrame = false;

    Server() {
      char path[] = "/tmp/nythera-live-capture-XXXXXX";
      TEST_CHECK(mkdtemp(path));
      m_runtime = path;
      TEST_CHECK(setenv("XDG_RUNTIME_DIR", path, 1) == 0);
      TEST_CHECK(unsetenv("WAYLAND_SOCKET") == 0);
      m_display = wl_display_create();
      TEST_CHECK(m_display);
      TEST_CHECK(wl_display_init_shm(m_display) == 0);
      const auto* socket = wl_display_add_socket_auto(m_display);
      TEST_CHECK(socket);
      TEST_CHECK(setenv("WAYLAND_DISPLAY", socket, 1) == 0);
      TEST_CHECK(wl_global_create(m_display, &wl_compositor_interface, 4, this, bindCompositor));
      TEST_CHECK(wl_global_create(m_display, &ext_foreign_toplevel_list_v1_interface, 1, this, bindList));
      TEST_CHECK(wl_global_create(
          m_display, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1, this, bindSource
      ));
      TEST_CHECK(wl_global_create(m_display, &ext_image_copy_capture_manager_v1_interface, 1, this, bindCapture));
      m_thread = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
          TEST_CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(m_display), 5) >= 0);
          wl_display_flush_clients(m_display);
        }
      });
    }
    ~Server() {
      m_thread.request_stop();
      m_thread.join();
      wl_display_destroy_clients(m_display);
      wl_display_destroy(m_display);
      std::filesystem::remove_all(m_runtime);
    }

  private:
    struct Session {
      Server* server;
      wl_resource* resource;
      int width = 2;
      int delivered = 0;
    };
    struct Frame {
      Session* session;
      wl_resource* buffer = nullptr;
    };
    wl_display* m_display = nullptr;
    std::string m_runtime;
    std::jthread m_thread;
    std::unordered_set<wl_resource*> m_buffers;

    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    static wl_resource* resource(wl_client* client, const wl_interface* interface, uint32_t id) {
      auto* result = wl_resource_create(client, interface, 1, id);
      TEST_CHECK(result);
      return result;
    }
    static void unsupported(wl_client* client, wl_resource*, uint32_t) { wl_client_post_no_memory(client); }
    static void bindCompositor(wl_client* client, void*, uint32_t, uint32_t id) {
      auto* compositor = wl_resource_create(client, &wl_compositor_interface, 4, id);
      static const struct wl_compositor_interface impl = {.create_surface = unsupported, .create_region = unsupported};
      wl_resource_set_implementation(compositor, &impl, nullptr, nullptr);
    }
    static void stopList(wl_client*, wl_resource* list) { ext_foreign_toplevel_list_v1_send_finished(list); }
    static void bindList(wl_client* client, void*, uint32_t, uint32_t id) {
      auto* list = resource(client, &ext_foreign_toplevel_list_v1_interface, id);
      static const struct ext_foreign_toplevel_list_v1_interface impl = {.stop = stopList, .destroy = destroy};
      wl_resource_set_implementation(list, &impl, nullptr, nullptr);
      auto* handle = resource(client, &ext_foreign_toplevel_handle_v1_interface, 0);
      static const struct ext_foreign_toplevel_handle_v1_interface handleImpl = {.destroy = destroy};
      wl_resource_set_implementation(handle, &handleImpl, nullptr, nullptr);
      ext_foreign_toplevel_list_v1_send_toplevel(list, handle);
      ext_foreign_toplevel_handle_v1_send_identifier(handle, "test-window");
      ext_foreign_toplevel_handle_v1_send_app_id(handle, "test.app");
      ext_foreign_toplevel_handle_v1_send_title(handle, "Test window");
      ext_foreign_toplevel_handle_v1_send_done(handle);
    }
    static void createSource(wl_client* client, wl_resource*, uint32_t id, wl_resource*) {
      auto* source = resource(client, &ext_image_capture_source_v1_interface, id);
      static const struct ext_image_capture_source_v1_interface impl = {.destroy = destroy};
      wl_resource_set_implementation(source, &impl, nullptr, nullptr);
    }
    static void bindSource(wl_client* client, void*, uint32_t, uint32_t id) {
      auto* source = resource(client, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, id);
      static const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface impl = {
          .create_source = createSource, .destroy = destroy
      };
      wl_resource_set_implementation(source, &impl, nullptr, nullptr);
    }
    static void constraints(Session* session) {
      ext_image_copy_capture_session_v1_send_buffer_size(session->resource, static_cast<uint32_t>(session->width), 2);
      ext_image_copy_capture_session_v1_send_shm_format(session->resource, WL_SHM_FORMAT_ARGB8888);
      ext_image_copy_capture_session_v1_send_done(session->resource);
    }
    static void attach(wl_client*, wl_resource* frameResource, wl_resource* buffer) {
      auto* frame = static_cast<Frame*>(wl_resource_get_user_data(frameResource));
      frame->buffer = buffer;
    }
    static void damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
    static void capture(wl_client*, wl_resource* frameResource) {
      auto* frame = static_cast<Frame*>(wl_resource_get_user_data(frameResource));
      auto* session = frame->session;
      auto* server = session->server;
      if (server->stopNextFrame.exchange(false)) {
        ext_image_copy_capture_session_v1_send_stopped(session->resource);
        return;
      }
      if (server->rejectNextFrame.exchange(false)) {
        session->width = 4;
        constraints(session);
        ext_image_copy_capture_frame_v1_send_failed(
            frameResource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS
        );
        return;
      }
      TEST_CHECK(frame->buffer);
      auto* buffer = wl_shm_buffer_get(frame->buffer);
      TEST_CHECK(buffer && wl_shm_buffer_get_width(buffer) == session->width);
      if (server->m_buffers.insert(frame->buffer).second) {
        ++server->uniqueBuffers;
        // Wayland may reuse a resource's address after destruction. Track its
        // lifetime rather than mistaking a new allocation for the old buffer.
        struct Watch {
          wl_listener listener{};
          Server* server;
          wl_resource* resource;
        };
        auto* watch = new Watch{{}, server, frame->buffer};
        watch->listener.notify = [](wl_listener* listener, void*) {
          auto* watched = reinterpret_cast<Watch*>(listener);
          watched->server->m_buffers.erase(watched->resource);
          wl_list_remove(&watched->listener.link);
          delete watched;
        };
        wl_resource_add_destroy_listener(frame->buffer, &watch->listener);
      }
      wl_shm_buffer_begin_access(buffer);
      auto* data = static_cast<uint32_t*>(wl_shm_buffer_get_data(buffer));
      std::fill_n(data, session->width * 2, 0xFFFF0000U);
      wl_shm_buffer_end_access(buffer);
      ++server->frames;
      ++session->delivered;
      ext_image_copy_capture_frame_v1_send_transform(frameResource, WL_OUTPUT_TRANSFORM_NORMAL);
      ext_image_copy_capture_frame_v1_send_damage(frameResource, 0, 0, session->width, 2);
      ext_image_copy_capture_frame_v1_send_presentation_time(frameResource, 0, 0, 0);
      ext_image_copy_capture_frame_v1_send_ready(frameResource);
      if (session->delivered == 2) {
        session->width = 4;
        constraints(session);
      }
    }
    static void createFrame(wl_client* client, wl_resource* sessionResource, uint32_t id) {
      auto* frame = resource(client, &ext_image_copy_capture_frame_v1_interface, id);
      auto* state = new Frame{static_cast<Session*>(wl_resource_get_user_data(sessionResource))};
      static const struct ext_image_copy_capture_frame_v1_interface impl = {
          .destroy = destroy, .attach_buffer = attach, .damage_buffer = damage, .capture = capture
      };
      wl_resource_set_implementation(frame, &impl, state, [](wl_resource* res) {
        delete static_cast<Frame*>(wl_resource_get_user_data(res));
      });
    }
    static void createSession(wl_client* client, wl_resource* manager, uint32_t id, wl_resource*, uint32_t) {
      auto* server = static_cast<Server*>(wl_resource_get_user_data(manager));
      auto* res = resource(client, &ext_image_copy_capture_session_v1_interface, id);
      auto* state = new Session{server, res};
      ++server->sessions;
      static const struct ext_image_copy_capture_session_v1_interface impl = {
          .create_frame = createFrame, .destroy = destroy
      };
      wl_resource_set_implementation(res, &impl, state, [](wl_resource* resource) {
        auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
        ++session->server->destroyedSessions;
        delete session;
      });
      constraints(state);
    }
    static void pointerSession(wl_client* client, wl_resource*, uint32_t, wl_resource*, wl_resource*) {
      wl_client_post_no_memory(client);
    }
    static void bindCapture(wl_client* client, void* server, uint32_t, uint32_t id) {
      auto* manager = resource(client, &ext_image_copy_capture_manager_v1_interface, id);
      static const struct ext_image_copy_capture_manager_v1_interface impl = {
          .create_session = createSession, .create_pointer_cursor_session = pointerSession, .destroy = destroy
      };
      wl_resource_set_implementation(manager, &impl, server, nullptr);
    }
  };
} // namespace

int main() {
  Server server;
  WaylandConnection wayland{WaylandConnection::ShellProtocols{}};
  TEST_CHECK(wayland.connect());
  auto* handle = wayland.extHandleForIdentifier("test-window");
  TEST_CHECK(handle);
  ToplevelThumbnailCapture capture(wayland);
  int received = 0;
  capture.startLive(
      handle, 10, 10, std::chrono::milliseconds(50), [&](std::optional<ScreencopyImage> image, std::string error) {
        TEST_CHECK(error.empty() && image);
        TEST_CHECK(image->rgba[0] == 255 && image->rgba[1] == 0);
        ++received;
        TEST_CHECK(image->width == (received < 3 ? 2 : 4));
        if (received == 3)
          capture.cancelInFlight();
      }
  );
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (received < 3 && std::chrono::steady_clock::now() < deadline) {
    TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
    TimerManager::instance().tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_CHECK(received == 3 && !capture.busy());
  TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(server.sessions == 1 && server.destroyedSessions == 1);
  TEST_CHECK(server.uniqueBuffers == 2 && server.frames == 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  TimerManager::instance().tick();
  TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(server.frames == 3);
  server.rejectNextFrame = true;
  bool retryDone = false;
  capture.startLive(
      handle, 10, 10, std::chrono::milliseconds(50), [&](std::optional<ScreencopyImage> image, std::string error) {
        TEST_CHECK(image && error.empty() && image->width == 4);
        retryDone = true;
        capture.cancelInFlight();
      }
  );
  const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!retryDone && std::chrono::steady_clock::now() < retryDeadline) {
    TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
    TimerManager::instance().tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_CHECK(retryDone && !capture.busy());
  TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(server.sessions == 2 && server.destroyedSessions == 2);

  server.stopNextFrame = true;
  bool stopped = false;
  capture.startLive(
      handle, 10, 10, std::chrono::milliseconds(50), [&](std::optional<ScreencopyImage> image, std::string error) {
        TEST_CHECK(!image && !error.empty() && !capture.busy());
        stopped = true;
      }
  );
  for (int i = 0; i < 8 && !stopped; ++i)
    TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(stopped);
  TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(server.sessions == 3 && server.destroyedSessions == 3);
  bool oneShotDone = false;
  capture.capture(handle, 10, 10, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(image && error.empty() && !capture.busy());
    oneShotDone = true;
  });
  for (int i = 0; i < 8 && !oneShotDone; ++i)
    TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(oneShotDone);
  TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
  TEST_CHECK(server.sessions == 4 && server.destroyedSessions == 4);
}
