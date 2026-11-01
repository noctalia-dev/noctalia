#include "capture/screencopy_capture.h"
#include "capture/screencopy_util.h"
#include "wayland/wayland_connection.h"
#include "wlr-screencopy-unstable-v1-test-server-protocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "screencopy_connection_isolation_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  class MockWaylandServer {
  public:
    MockWaylandServer() {
      const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      if (runtimeDir != nullptr) {
        m_previousRuntimeDir = runtimeDir;
      }
      const char* displayName = std::getenv("WAYLAND_DISPLAY");
      if (displayName != nullptr) {
        m_previousDisplayName = displayName;
      }
      const char* waylandSocket = std::getenv("WAYLAND_SOCKET");
      if (waylandSocket != nullptr) {
        m_previousWaylandSocket = waylandSocket;
      }

      char runtimeTemplate[] = "/tmp/noctalia-screencopy-test-XXXXXX";
      const char* createdRuntimeDir = mkdtemp(runtimeTemplate);
      if (createdRuntimeDir == nullptr) {
        throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
      }
      m_runtimeDir = createdRuntimeDir;

      try {
        setEnvironment("XDG_RUNTIME_DIR", m_runtimeDir);
        if (unsetenv("WAYLAND_SOCKET") != 0) {
          throw std::runtime_error(std::string("unsetenv failed for WAYLAND_SOCKET: ") + std::strerror(errno));
        }

        m_display = wl_display_create();
        if (m_display == nullptr) {
          throw std::runtime_error("wl_display_create failed");
        }

        const char* socketName = wl_display_add_socket_auto(m_display);
        if (socketName == nullptr) {
          throw std::runtime_error("wl_display_add_socket_auto failed");
        }
        setEnvironment("WAYLAND_DISPLAY", socketName);

        createGlobal(&wl_compositor_interface, 4, &bindCompositor);
        createGlobal(&wl_shm_interface, 1, &bindShm);
        createGlobal(&wl_output_interface, 4, &bindOutput);
        createGlobal(&zwlr_screencopy_manager_v1_interface, 3, &bindScreencopyManager);

        m_serverThread = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
      } catch (...) {
        cleanup();
        throw;
      }
    }

    ~MockWaylandServer() { cleanup(); }

    MockWaylandServer(const MockWaylandServer&) = delete;
    MockWaylandServer& operator=(const MockWaylandServer&) = delete;

    [[nodiscard]] int compositorBindCount() const noexcept { return m_compositorBindCount.load(); }

  private:
    using BindCallback = void (*)(wl_client*, void*, std::uint32_t, std::uint32_t);

    static void setEnvironment(const char* name, const std::string& value) {
      if (setenv(name, value.c_str(), 1) != 0) {
        throw std::runtime_error(std::string("setenv failed for ") + name + ": " + std::strerror(errno));
      }
    }

    static void restoreEnvironment(const char* name, const std::optional<std::string>& value) noexcept {
      if (value.has_value()) {
        static_cast<void>(setenv(name, value->c_str(), 1));
      } else {
        static_cast<void>(unsetenv(name));
      }
    }

    void createGlobal(const wl_interface* interface, int version, BindCallback bind) {
      if (wl_global_create(m_display, interface, version, this, bind) == nullptr) {
        throw std::runtime_error(std::string("wl_global_create failed for ") + interface->name);
      }
    }

    static wl_resource*
    createResource(wl_client* client, const wl_interface* interface, std::uint32_t version, std::uint32_t id) {
      auto* resource = wl_resource_create(client, interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
      }
      return resource;
    }

    static void destroyResource(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

    static void createSurface(wl_client* client, wl_resource* /*resource*/, std::uint32_t /*id*/) {
      wl_client_post_no_memory(client);
    }

    static void createRegion(wl_client* client, wl_resource* /*resource*/, std::uint32_t /*id*/) {
      wl_client_post_no_memory(client);
    }

    static void bindCompositor(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* self = static_cast<MockWaylandServer*>(data);
      auto* resource = createResource(client, &wl_compositor_interface, std::min(version, 4U), id);
      if (resource == nullptr) {
        return;
      }

      static const struct wl_compositor_interface implementation = {
          .create_surface = &createSurface,
          .create_region = &createRegion,
      };
      wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
      self->m_compositorBindCount.fetch_add(1);
    }

    static void createShmPool(
        wl_client* client, wl_resource* /*resource*/, std::uint32_t /*id*/, std::int32_t fd, std::int32_t /*size*/
    ) {
      close(fd);
      wl_client_post_no_memory(client);
    }

    static void bindShm(wl_client* client, void* /*data*/, std::uint32_t version, std::uint32_t id) {
      auto* resource = createResource(client, &wl_shm_interface, std::min(version, 1U), id);
      if (resource == nullptr) {
        return;
      }

      static const struct wl_shm_interface implementation = {
          .create_pool = &createShmPool,
      };
      wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
      wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
    }

    static void outputResourceDestroyed(wl_resource* resource) {
      auto* self = static_cast<MockWaylandServer*>(wl_resource_get_user_data(resource));
      if (self != nullptr && self->m_primaryOutputResource == resource) {
        self->m_primaryOutputResource = nullptr;
      }
    }

    static void bindOutput(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* self = static_cast<MockWaylandServer*>(data);
      const std::uint32_t resourceVersion = std::min(version, 4U);
      auto* resource = createResource(client, &wl_output_interface, resourceVersion, id);
      if (resource == nullptr) {
        return;
      }

      static const struct wl_output_interface implementation = {
          .release = &destroyResource,
      };
      wl_resource_set_implementation(resource, &implementation, self, &outputResourceDestroyed);
      if (self->m_primaryOutputResource == nullptr) {
        self->m_primaryOutputResource = resource;
      }

      wl_output_send_geometry(
          resource, 0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Noctalia", "Mock output", WL_OUTPUT_TRANSFORM_NORMAL
      );
      wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
      if (resourceVersion >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, 1);
      }
      if (resourceVersion >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(resource, "MOCK-1");
        wl_output_send_description(resource, "Noctalia screencopy isolation test output");
      }
      if (resourceVersion >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
      }
    }

    static void postHyprlandCaptureError(wl_resource* managerResource) {
      auto* self = static_cast<MockWaylandServer*>(wl_resource_get_user_data(managerResource));
      if (self != nullptr && self->m_primaryOutputResource != nullptr) {
        wl_output_send_mode(
            self->m_primaryOutputResource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1600, 900, 60000
        );
        wl_output_send_done(self->m_primaryOutputResource);
        wl_client_flush(wl_resource_get_client(self->m_primaryOutputResource));
      }
      wl_resource_post_error(managerResource, std::numeric_limits<std::uint32_t>::max(), "Failed to capture output");
    }

    static void captureOutput(
        wl_client* /*client*/, wl_resource* resource, std::uint32_t /*frame*/, std::int32_t /*overlayCursor*/,
        wl_resource* /*output*/
    ) {
      postHyprlandCaptureError(resource);
    }

    static void captureOutputRegion(
        wl_client* /*client*/, wl_resource* resource, std::uint32_t /*frame*/, std::int32_t /*overlayCursor*/,
        wl_resource* /*output*/, std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*width*/, std::int32_t /*height*/
    ) {
      postHyprlandCaptureError(resource);
    }

    static void bindScreencopyManager(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* self = static_cast<MockWaylandServer*>(data);
      auto* resource = createResource(client, &zwlr_screencopy_manager_v1_interface, std::min(version, 3U), id);
      if (resource == nullptr) {
        return;
      }

      static const struct zwlr_screencopy_manager_v1_interface implementation = {
          .capture_output = &captureOutput,
          .capture_output_region = &captureOutputRegion,
          .destroy = &destroyResource,
      };
      wl_resource_set_implementation(resource, &implementation, self, nullptr);
    }

    void run(std::stop_token stopToken) noexcept {
      auto* eventLoop = wl_display_get_event_loop(m_display);
      while (!stopToken.stop_requested()) {
        if (wl_event_loop_dispatch(eventLoop, 10) < 0 && errno != EINTR) {
          return;
        }
        wl_display_flush_clients(m_display);
      }
    }

    void cleanup() noexcept {
      if (m_serverThread.joinable()) {
        m_serverThread.request_stop();
        m_serverThread.join();
      }
      if (m_display != nullptr) {
        wl_display_destroy_clients(m_display);
        wl_display_destroy(m_display);
        m_display = nullptr;
      }

      restoreEnvironment("WAYLAND_DISPLAY", m_previousDisplayName);
      restoreEnvironment("XDG_RUNTIME_DIR", m_previousRuntimeDir);
      restoreEnvironment("WAYLAND_SOCKET", m_previousWaylandSocket);
      if (!m_runtimeDir.empty()) {
        static_cast<void>(rmdir(m_runtimeDir.c_str()));
        m_runtimeDir.clear();
      }
    }

    std::optional<std::string> m_previousRuntimeDir;
    std::optional<std::string> m_previousDisplayName;
    std::optional<std::string> m_previousWaylandSocket;
    std::string m_runtimeDir;
    wl_display* m_display = nullptr;
    wl_resource* m_primaryOutputResource = nullptr;
    std::jthread m_serverThread;
    std::atomic<int> m_compositorBindCount = 0;
  };

} // namespace

int main() {
  try {
    MockWaylandServer server;
    {
      WaylandConnection primary;
      expect(primary.connect(), "primary Wayland connection should connect");
      expect(primary.compositor() != nullptr, "primary connection should bind wl_compositor");

      WaylandConnection captureConnection(WaylandConnection::Purpose::Screencopy);
      std::string setupError;
      const bool captureConnected = captureConnection.connectUntil(
          std::chrono::steady_clock::now() + screencopy::kBlockingCaptureTimeout, &primary, setupError
      );
      expect(captureConnected, "capture Wayland connection should connect to the primary endpoint");
      if (!captureConnected) {
        std::println(stderr, "screencopy_connection_isolation_test: capture setup error: {}", setupError);
      }
      expect(captureConnection.compositor() == nullptr, "capture-purpose connection must not bind wl_compositor");
      expect(server.compositorBindCount() == 1, "only the primary connection should bind wl_compositor");
      expect(captureConnection.shm() != nullptr, "capture-purpose connection should bind wl_shm");
      expect(captureConnection.hasScreencopy(), "capture-purpose connection should bind zwlr_screencopy_manager_v1");
      expect(captureConnection.outputs().size() == 1, "capture-purpose connection should bind the advertised output");

      if (!captureConnection.outputs().empty()) {
        ScreencopyCapture capture(captureConnection);
        ScreencopyImage image;
        std::string error;
        const screencopy::CaptureOutputStatus captureStatus = screencopy::captureOutputBlocking(
            capture, captureConnection, captureConnection.outputs().front().output, image, error, false, &primary
        );

        expect(
            captureStatus == screencopy::CaptureOutputStatus::EventLoopFailed,
            "Hyprland-style fatal screencopy error should fail the isolated event loop"
        );
        expect(
            wl_display_get_error(captureConnection.display()) == EPROTO,
            "fatal screencopy error should remain isolated as EPROTO on the capture connection"
        );
        expect(
            error.contains("protocol_error.interface=zwlr_screencopy_manager_v1"),
            "capture error should identify zwlr_screencopy_manager_v1"
        );
        expect(
            error.contains("code=4294967295"), "capture error should preserve Hyprland's UINT32_MAX protocol error code"
        );
        expect(
            !primary.outputs().empty() && primary.outputs().front().width == 1600,
            "blocking capture should pump pending events on the primary connection"
        );
      }

      expect(
          wl_display_roundtrip(primary.display()) >= 0,
          "primary Wayland connection should remain healthy after capture connection protocol failure"
      );
      expect(wl_display_get_error(primary.display()) == 0, "primary Wayland connection should have no display error");
    }
  } catch (const std::exception& error) {
    std::println(stderr, "screencopy_connection_isolation_test: setup failed: {}", error.what());
    return 1;
  }

  return g_failures == 0 ? 0 : 1;
}
