#include "compositors/umbriel/umbriel_audio_provider.h"
#include "compositors/umbriel/umbriel_runtime.h"
#include "test_check.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using compositors::umbriel::UmbrielAudioProvider;
using compositors::umbriel::UmbrielRuntime;
using namespace std::chrono_literals;

namespace {
  class Server {
  public:
    explicit Server(bool useRuntimePath = false) {
      char directory[] = "/tmp/noctalia-audio-test-XXXXXX";
      const auto* created = mkdtemp(directory);
      TEST_CHECK(created != nullptr);
      m_directory = created;
      m_path = m_directory + "/umbriel-audio-test.sock";
      if (useRuntimePath) {
        TEST_CHECK(unsetenv("UMBRIEL_SOCKET") == 0);
        TEST_CHECK(setenv("XDG_RUNTIME_DIR", m_directory.c_str(), 1) == 0);
        TEST_CHECK(setenv("WAYLAND_DISPLAY", "audio-test", 1) == 0);
      } else {
        TEST_CHECK(setenv("UMBRIEL_SOCKET", m_path.c_str(), 1) == 0);
      }
      m_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_fd >= 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
      TEST_CHECK(bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(listen(m_fd, 4) == 0);
    }
    ~Server() {
      closeClient();
      close(m_fd);
      unlink(m_path.c_str());
      rmdir(m_directory.c_str());
    }
    void closeClient() {
      if (m_client >= 0) {
        close(m_client);
        m_client = -1;
      }
      m_input.clear();
    }
    void step(UmbrielAudioProvider& provider) {
      std::vector<pollfd> fds;
      const auto index = provider.addPollFds(fds);
      const auto ready = poll(fds.data(), fds.size(), 1);
      TEST_CHECK(ready >= 0);
      if (ready > 0 || provider.pollTimeoutMs() == 0) {
        provider.dispatch(fds, index);
      }
      if (m_client < 0) {
        m_client = accept4(m_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      }
      if (m_client >= 0) {
        std::array<char, 8192> data{};
        const auto size = recv(m_client, data.data(), data.size(), 0);
        if (size > 0) {
          m_input.append(data.data(), static_cast<std::size_t>(size));
        }
      }
    }
    std::string packet(UmbrielAudioProvider& provider) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (!m_input.contains('\n') && std::chrono::steady_clock::now() < deadline) {
        step(provider);
      }
      const auto end = m_input.find('\n');
      TEST_CHECK(end != std::string::npos);
      auto result = m_input.substr(0, end + 1);
      m_input.erase(0, end + 1);
      TEST_CHECK(result.size() <= 4096);
      return result;
    }
    void reply(std::string_view value = "{\"ok\":true}\n") const {
      TEST_CHECK(send(m_client, value.data(), value.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(value.size()));
    }
    void noPacket(UmbrielAudioProvider& provider, std::chrono::milliseconds duration) {
      const auto deadline = std::chrono::steady_clock::now() + duration;
      while (std::chrono::steady_clock::now() < deadline) {
        step(provider);
        TEST_CHECK(m_input.empty());
      }
    }
    bool clientClosed() const {
      char byte = 0;
      return recv(m_client, &byte, 1, MSG_PEEK) == 0;
    }

  private:
    int m_fd = -1;
    int m_client = -1;
    std::string m_directory;
    std::string m_path;
    std::string m_input;
  };

  void checkBands(const std::string& packet, float expected) {
    const auto message = nlohmann::json::parse(packet);
    TEST_CHECK(message.size() == 3);
    TEST_CHECK(message["cmd"] == "effect-audio");
    TEST_CHECK(message["version"] == 2);
    TEST_CHECK(message["bands"].size() == 64);
    for (const auto& band : message["bands"]) {
      TEST_CHECK(std::abs(band.get<float>() - expected) < 0.000001F);
    }
  }
} // namespace

int main(int argc, char** argv) {
  // Paired compositor integration: deterministic source, then healthy silence.
  if (argc == 2 && std::string_view(argv[1]) == "--feed") {
    UmbrielAudioProvider::Bands bands{};
    for (std::size_t i = 0; i < bands.size(); ++i) {
      bands[i] = static_cast<float>(i) / 100.0F;
    }
    UmbrielRuntime runtime;
    UmbrielAudioProvider provider(runtime, [&bands] { return bands; });
    provider.setEnabled(true);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 10s) {
      if (std::chrono::steady_clock::now() - start > 7s && bands.back() > 0) {
        bands.fill(0);
        provider.notifyChanged();
      }
      std::vector<pollfd> fds;
      const auto index = provider.addPollFds(fds);
      TEST_CHECK(poll(fds.data(), fds.size(), provider.pollTimeoutMs()) >= 0);
      provider.dispatch(fds, index);
    }
    return 0;
  }
  Server server;
  UmbrielAudioProvider::Bands source{};
  source.fill(0.9F);
  UmbrielRuntime runtime;
  UmbrielAudioProvider provider(runtime, [&source] { return source; });
  TEST_CHECK(provider.pollTimeoutMs() == -1);
  provider.setEnabled(true);
  checkBands(server.packet(provider), 0.9F);

  // Multiple measurements while awaiting an ACK collapse to the latest one.
  source.fill(0.2F);
  provider.notifyChanged();
  server.noPacket(provider, 25ms);
  source.fill(0.7F);
  provider.notifyChanged();
  server.reply("{\"ok\":");
  server.noPacket(provider, 25ms);
  server.reply("true}\n");
  checkBands(server.packet(provider), 0.7F);
  server.reply();

  // Fast changes respect the 60 Hz ceiling, even with immediate acknowledgments.
  source.fill(0.3F);
  provider.notifyChanged();
  server.noPacket(provider, 8ms);
  checkBands(server.packet(provider), 0.3F);
  server.reply();

  source.fill(0);
  provider.notifyChanged();
  checkBands(server.packet(provider), 0);
  server.reply();
  server.noPacket(provider, 60ms);
  checkBands(server.packet(provider), 0);
  server.reply();

  source.fill(0.5F);
  provider.notifyChanged();
  checkBands(server.packet(provider), 0.5F);

  // No ACK expires the connection; reconnect sends a fresh measurement.
  server.noPacket(provider, 220ms);
  TEST_CHECK(server.clientClosed());
  server.closeClient();
  source.fill(0.6F);
  checkBands(server.packet(provider), 0.6F);
  server.reply("{\"ok\":false,\"error\":\"locked\"}\n");
  server.step(provider);
  TEST_CHECK(server.clientClosed());
  server.closeClient();
  checkBands(server.packet(provider), 0.6F);
  server.reply();

  source[0] = std::numeric_limits<float>::quiet_NaN();
  provider.notifyChanged();
  server.noPacket(provider, 25ms);
  TEST_CHECK(server.clientClosed());
  provider.setEnabled(false);
  TEST_CHECK(provider.pollTimeoutMs() == -1);
  std::vector<pollfd> fds;
  provider.addPollFds(fds);
  TEST_CHECK(fds.empty());

  // Sessions without UMBRIEL_SOCKET use the runtime's Wayland socket path.
  Server runtimeServer(true);
  UmbrielRuntime fallbackRuntime;
  UmbrielAudioProvider runtimeProvider(fallbackRuntime, [] { return UmbrielAudioProvider::Bands{}; });
  runtimeProvider.setEnabled(true);
  checkBands(runtimeServer.packet(runtimeProvider), 0);
  runtimeProvider.setEnabled(false);
  TEST_CHECK(runtimeServer.clientClosed());
}
