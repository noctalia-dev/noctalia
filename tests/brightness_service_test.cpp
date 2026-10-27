#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "system/brightness_service.h"
#include "tests/test_check.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace {

  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  constexpr std::array<const char*, 2> kConnectors{"DDC-TEST-1", "DDC-TEST-2"};

  std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  void writeFile(const fs::path& path, std::string_view contents) {
    const auto temporary = fs::path(path.string() + ".tmp");
    {
      std::ofstream file(temporary);
      TEST_CHECK(file.is_open());
      file << contents;
      TEST_CHECK(file.good());
    }
    fs::rename(temporary, path);
  }

  int readCount(const fs::path& path) {
    std::ifstream file(path);
    int count = 0;
    file >> count;
    return count;
  }

  class EnvironmentVariable {
  public:
    EnvironmentVariable(std::string name, const std::string& value) : m_name(std::move(name)) {
      if (const char* previous = std::getenv(m_name.c_str())) {
        m_previous = previous;
      }
      TEST_CHECK(setenv(m_name.c_str(), value.c_str(), 1) == 0);
    }

    ~EnvironmentVariable() {
      if (m_previous) {
        TEST_CHECK(setenv(m_name.c_str(), m_previous->c_str(), 1) == 0);
      } else {
        TEST_CHECK(unsetenv(m_name.c_str()) == 0);
      }
    }

    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
  };

  // Both the Wayland connection and ddcutil processes belong to this fixture.
  class OutputServer {
  public:
    explicit OutputServer(int outputCount) {
      int sockets[2];
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
      m_display = wl_display_create();
      TEST_CHECK(m_display != nullptr);
      TEST_CHECK(wl_client_create(m_display, sockets[0]) != nullptr);
      m_socket = std::make_unique<EnvironmentVariable>("WAYLAND_SOCKET", std::to_string(sockets[1]));
      for (int index = 0; index < outputCount; ++index) {
        m_globals[static_cast<std::size_t>(index)] = wl_global_create(
            m_display, &wl_output_interface, 4, const_cast<char*>(kConnectors[static_cast<std::size_t>(index)]),
            &bindOutput
        );
        TEST_CHECK(m_globals[static_cast<std::size_t>(index)] != nullptr);
      }
      m_thread = std::jthread([this](std::stop_token stop) {
        auto* loop = wl_display_get_event_loop(m_display);
        while (!stop.stop_requested()) {
          TEST_CHECK(wl_event_loop_dispatch(loop, 10) >= 0 || errno == EINTR);
          {
            const std::scoped_lock lock(m_mutex);
            for (const auto index : m_removals) {
              wl_global_destroy(m_globals[index]);
              m_globals[index] = nullptr;
            }
            m_removals.clear();
          }
          wl_display_flush_clients(m_display);
        }
      });
    }

    ~OutputServer() {
      m_thread.request_stop();
      m_thread.join();
      wl_display_destroy_clients(m_display);
      wl_display_destroy(m_display);
    }

    OutputServer(const OutputServer&) = delete;
    OutputServer& operator=(const OutputServer&) = delete;

    void removeOutput(std::size_t index) {
      const std::scoped_lock lock(m_mutex);
      m_removals.push_back(index);
    }

  private:
    static void release(wl_client*, wl_resource* output) { wl_resource_destroy(output); }

    static void bindOutput(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* output = wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
      TEST_CHECK(output != nullptr);
      static const struct wl_output_interface implementation = {.release = &release};
      wl_resource_set_implementation(output, &implementation, nullptr, nullptr);
      wl_output_send_geometry(output, 0, 0, 600, 340, 0, "test", "test", 0);
      wl_output_send_mode(output, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
      wl_output_send_scale(output, 1);
      wl_output_send_name(output, static_cast<const char*>(data));
      wl_output_send_description(output, "Simulated DDC monitor");
      wl_output_send_done(output);
    }

    wl_display* m_display = nullptr;
    std::unique_ptr<EnvironmentVariable> m_socket;
    std::array<wl_global*, 2> m_globals{};
    std::jthread m_thread;
    std::mutex m_mutex;
    std::vector<std::size_t> m_removals;
  };

  class DdcExecutable {
  public:
    DdcExecutable(std::string_view mode, int outputCount) {
      std::array<char, 64> directory{};
      constexpr std::string_view pattern = "/tmp/noctalia-brightness-test.XXXXXX";
      std::ranges::copy(pattern, directory.begin());
      TEST_CHECK(mkdtemp(directory.data()) != nullptr);
      m_directory = directory.data();
      fs::create_symlink(fs::read_symlink("/proc/self/exe"), m_directory / "ddcutil");
      writeFile(m_directory / "mode", mode);
      writeFile(m_directory / "outputs", std::to_string(outputCount));
      m_path = std::make_unique<EnvironmentVariable>("PATH", m_directory.string());
      m_state = std::make_unique<EnvironmentVariable>("NOCTALIA_TEST_DDC_DIR", m_directory.string());
    }

    ~DdcExecutable() { fs::remove_all(m_directory); }

    DdcExecutable(const DdcExecutable&) = delete;
    DdcExecutable& operator=(const DdcExecutable&) = delete;

    void set(std::string_view name, std::string_view value = "1") { writeFile(m_directory / name, value); }

    void clear(std::string_view name) { fs::remove(m_directory / name); }

    [[nodiscard]] bool exists(std::string_view name) const { return fs::exists(m_directory / name); }

    [[nodiscard]] int count(std::string_view command) const {
      return readCount(m_directory / ("count-" + std::string(command)));
    }

  private:
    fs::path m_directory;
    std::unique_ptr<EnvironmentVariable> m_path;
    std::unique_ptr<EnvironmentVariable> m_state;
  };

  struct Fixture {
    DdcExecutable ddc;
    OutputServer server;
    WaylandConnection wayland;
    CompositorPlatform platform;
    BrightnessConfig config;
    std::unique_ptr<BrightnessService> service;
    std::size_t dispatchCount = 0;

    explicit Fixture(std::string_view mode, int outputCount = 1)
        : ddc(mode, outputCount), server(outputCount), platform(wayland) {
      TEST_CHECK(wayland.connect());
      TEST_CHECK(wayland.outputs().size() == static_cast<std::size_t>(outputCount));
      config.enableDdcutil = true;
      for (int index = 0; index < outputCount; ++index) {
        config.monitorOverrides.push_back({
            .match = kConnectors[static_cast<std::size_t>(index)],
            .backend = BrightnessBackendPreference::Ddcutil,
        });
      }
      service = std::make_unique<BrightnessService>(nullptr, platform, config);
    }

    bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 2500ms) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
          return true;
        }
        pollfd descriptor{.fd = service->watchFd(), .events = POLLIN};
        const int ready = poll(&descriptor, 1, 10);
        TEST_CHECK(ready >= 0 || errno == EINTR);
        if (ready > 0) {
          service->dispatchWatch();
          ++dispatchCount;
        }
      }
      return predicate();
    }

    void observe(std::chrono::milliseconds duration, const std::function<void()>& check = []() {}) {
      TEST_CHECK(!waitFor(
          [&check]() {
            check();
            return false;
          },
          duration
      ));
    }

    [[nodiscard]] bool controllable(std::size_t index = 0) const {
      const auto* display = service->findDisplay(kConnectors[index]);
      return display != nullptr && display->controllable;
    }

    [[nodiscard]] bool brightnessIs(float expected, std::size_t index = 0) const {
      const auto* display = service->findDisplay(kConnectors[index]);
      return display != nullptr && display->controllable && std::abs(display->brightness - expected) < 0.001F;
    }
  };

  int runDdcCommand(int argc, char** argv) {
    const char* directory = std::getenv("NOCTALIA_TEST_DDC_DIR");
    TEST_CHECK(directory != nullptr);
    const fs::path state(directory);
    std::string command;
    std::string bus;
    std::string target;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--bus" && index + 1 < argc) {
        bus = argv[++index];
      } else if (argument == "detect" || argument == "getvcp" || argument == "setvcp") {
        command = argument;
        if (argument == "setvcp" && index + 2 < argc) {
          target = argv[index + 2];
        }
      }
    }
    TEST_CHECK(!command.empty());
    const std::string key = command + (bus.empty() ? "" : "-" + bus);
    const int count = readCount(state / ("count-" + key)) + 1;
    writeFile(state / ("count-" + key), std::to_string(count));
    const std::string mode = readFile(state / "mode");
    const auto hold = state / ("hold-" + key);
    if (fs::exists(hold) || (mode == "blocked-detect" && command == "detect")) {
      writeFile(state / ("held-" + key), std::to_string(count));
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!fs::exists(state / ("release-" + key)) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
      }
    }

    if (command == "detect") {
      if (mode == "detect-failure" && count == 1) {
        return 1;
      }
      if (mode == "detect-empty" && count == 1) {
        return 0;
      }
      for (int index = 0; index < readCount(state / "outputs"); ++index) {
        const auto busMapping = state / ("bus-" + std::to_string(index));
        const int detectedBus = fs::exists(busMapping) ? readCount(busMapping) : 71 + index;
        std::println(
            "Display {}\n   I2C bus: /dev/i2c-{}\n   DRM connector: {}\n   Model: Simulated monitor", index + 1,
            detectedBus, kConnectors[static_cast<std::size_t>(index)]
        );
      }
      return 0;
    }
    if (command == "getvcp") {
      if (mode == "always-fail"
          || fs::exists(state / ("fail-" + key))
          || (mode == "query-recovery" && count == 1)
          || (mode == "partial" && bus == "72" && count == 1)) {
        return 1;
      }
      const auto brightness = state / ("brightness-" + bus);
      const int value = fs::exists(brightness) ? readCount(brightness) : 40;
      std::println("VCP code 0x10 (Brightness): current value = {}, max value = 100", value);
      return 0;
    }
    TEST_CHECK(command == "setvcp");
    if (fs::exists(state / ("fail-" + key))) {
      return 1;
    }
    writeFile(state / ("brightness-" + bus), target);
    return 0;
  }

  void checkStartupQueryRecovery() {
    Fixture fixture("query-recovery");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("getvcp-71") == 1; }));
    TEST_CHECK(!fixture.controllable());
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
    TEST_CHECK(fixture.brightnessIs(0.4F));
    TEST_CHECK(fixture.ddc.count("detect") == 2);
    fixture.observe(1200ms);
    TEST_CHECK(fixture.ddc.count("detect") == 2);
  }

  void checkDetectRecovery() {
    for (const auto* mode : {"detect-failure", "detect-empty"}) {
      Fixture fixture(mode);
      TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
      TEST_CHECK(fixture.ddc.count("detect") == 2);
      TEST_CHECK(fixture.brightnessIs(0.4F));
    }
  }

  void checkPartialRecoveryAndRescanPreserveWrites() {
    Fixture fixture("partial", 2);
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
    TEST_CHECK(!fixture.controllable(1));
    fixture.ddc.set("hold-getvcp-72");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.exists("held-getvcp-72"); }));
    fixture.ddc.set("fail-getvcp-71");
    fixture.service->setBrightness(kConnectors[0], 0.8F);
    TEST_CHECK(fixture.brightnessIs(0.8F));
    fixture.ddc.set("release-getvcp-72");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(1); }));
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("setvcp-71") == 1; }));
    TEST_CHECK(fixture.brightnessIs(0.8F));
    TEST_CHECK(fixture.ddc.count("getvcp-71") == 1);

    fixture.ddc.set("hold-detect");
    fixture.service->requestDdcRescan();
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.exists("held-detect"); }));
    fixture.service->setBrightness(kConnectors[0], 0.9F);
    fixture.ddc.set("release-detect");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("setvcp-71") == 2; }));
    fixture.observe(100ms, [&fixture]() {
      TEST_CHECK(fixture.brightnessIs(0.9F));
      TEST_CHECK(fixture.controllable(1));
    });
    TEST_CHECK(fixture.ddc.count("getvcp-71") == 1);
  }

  void checkBoundedRetriesAndManualRestart() {
    Fixture fixture("always-fail");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("getvcp-71") == 4; }, 8500ms));
    fixture.observe(4500ms);
    TEST_CHECK(fixture.ddc.count("detect") == 4);
    TEST_CHECK(!fixture.controllable());
    fixture.ddc.set("mode", "ready");
    fixture.service->requestDdcRescan();
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
    TEST_CHECK(fixture.ddc.count("detect") == 5);
  }

  void checkRescanUpdatesBusRouting() {
    Fixture fixture("ready");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
    fixture.ddc.set("bus-0", "73");
    fixture.ddc.set("brightness-73", "60");
    fixture.service->requestDdcRescan();
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.brightnessIs(0.6F); }));
    TEST_CHECK(fixture.ddc.count("getvcp-73") == 1);
    fixture.service->setBrightness(kConnectors[0], 0.8F);
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("setvcp-73") == 1; }));
    TEST_CHECK(fixture.ddc.count("setvcp-71") == 0);
    TEST_CHECK(fixture.brightnessIs(0.8F));
    TEST_CHECK(fixture.service->displays().size() == 1);
  }

  void checkRescanRecoversQuarantinedDisplay() {
    Fixture fixture("ready");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }));
    fixture.ddc.set("fail-setvcp-71");
    for (int count = 1; count <= 3; ++count) {
      const auto previousDispatches = fixture.dispatchCount;
      fixture.service->setBrightness(kConnectors[0], 0.4F + static_cast<float>(count) * 0.1F);
      TEST_CHECK(fixture.waitFor([&fixture, count, previousDispatches]() {
        return fixture.ddc.count("setvcp-71") == count && fixture.dispatchCount > previousDispatches;
      }));
    }
    fixture.service->setBrightness(kConnectors[0], 0.9F);
    TEST_CHECK(fixture.brightnessIs(0.7F));
    TEST_CHECK(fixture.ddc.count("setvcp-71") == 3);

    fixture.ddc.clear("fail-setvcp-71");
    fixture.ddc.set("brightness-71", "50");
    fixture.service->requestDdcRescan();
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.brightnessIs(0.5F); }));
    TEST_CHECK(fixture.ddc.count("getvcp-71") == 2);
    fixture.service->setBrightness(kConnectors[0], 0.8F);
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.count("setvcp-71") == 4; }));
    TEST_CHECK(fixture.brightnessIs(0.8F));
  }

  void checkDisableCancelsDiscovery() {
    Fixture fixture("blocked-detect");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.exists("held-detect"); }));
    fixture.config.enableDdcutil = false;
    fixture.service->reload(fixture.config);
    fixture.ddc.set("mode", "ready");
    fixture.config.enableDdcutil = true;
    fixture.service->reload(fixture.config);
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.controllable(); }, 1500ms));
    TEST_CHECK(fixture.ddc.count("detect") == 2);
    fixture.observe(1200ms);
    TEST_CHECK(fixture.ddc.count("detect") == 2);
  }

  void checkDestructionCancelsDiscovery() {
    Fixture fixture("blocked-detect");
    TEST_CHECK(fixture.waitFor([&fixture]() { return fixture.ddc.exists("held-detect"); }));
    const auto start = std::chrono::steady_clock::now();
    fixture.service.reset();
    TEST_CHECK(std::chrono::steady_clock::now() - start < 1500ms);
    TEST_CHECK(fixture.ddc.count("getvcp-71") == 0);
  }

  void checkDisableCancelsDelayedDiscovery() {
    Fixture fixture("always-fail");
    TEST_CHECK(fixture.waitFor([&fixture]() {
      return fixture.ddc.count("getvcp-71") == 1 && fixture.dispatchCount > 0;
    }));
    fixture.config.enableDdcutil = false;
    fixture.service->reload(fixture.config);
    fixture.observe(1500ms);
    TEST_CHECK(fixture.ddc.count("detect") == 1);
    TEST_CHECK(!fixture.controllable());
  }

  void checkExclusionsStopRetries() {
    Fixture fixture("always-fail", 2);
    fixture.config.monitorOverrides[0].backend = BrightnessBackendPreference::None;
    fixture.config.monitorOverrides[1].backend = BrightnessBackendPreference::Backlight;
    fixture.service->reload(fixture.config);
    fixture.observe(1500ms);
    TEST_CHECK(!fixture.controllable());
    TEST_CHECK(!fixture.controllable(1));
    const int attempts = fixture.ddc.count("detect");
    fixture.observe(1200ms);
    TEST_CHECK(fixture.ddc.count("detect") == attempts);
  }

  void checkOutputRemovalCancelsRetry() {
    Fixture fixture("always-fail");
    TEST_CHECK(fixture.waitFor([&fixture]() {
      return fixture.ddc.count("getvcp-71") == 1 && fixture.dispatchCount > 0;
    }));
    fixture.server.removeOutput(0);
    TEST_CHECK(wl_display_roundtrip(fixture.wayland.display()) >= 0);
    TEST_CHECK(wl_display_roundtrip(fixture.wayland.display()) >= 0);
    TEST_CHECK(fixture.wayland.outputs().empty());
    fixture.service->onOutputsChanged();
    fixture.observe(1500ms);
    TEST_CHECK(fixture.service->displays().empty());
    const int attempts = fixture.ddc.count("detect");
    fixture.observe(1200ms);
    TEST_CHECK(fixture.ddc.count("detect") == attempts);
  }

} // namespace

int main(int argc, char** argv) {
  if (fs::path(argv[0]).filename() == "ddcutil") {
    return runDdcCommand(argc, argv);
  }
  checkStartupQueryRecovery();
  checkDetectRecovery();
  checkPartialRecoveryAndRescanPreserveWrites();
  checkBoundedRetriesAndManualRestart();
  checkRescanUpdatesBusRouting();
  checkRescanRecoversQuarantinedDisplay();
  checkDisableCancelsDiscovery();
  checkDestructionCancelsDiscovery();
  checkDisableCancelsDelayedDiscovery();
  checkExclusionsStopRetries();
  checkOutputRemovalCancelsRetry();
  return 0;
}
