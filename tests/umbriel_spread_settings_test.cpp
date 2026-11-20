#include "compositors/umbriel/umbriel_runtime.h"
#include "config/toml.h"
#include "tests/test_check.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
  class FakeUmbriel {
  public:
    FakeUmbriel() {
      char pattern[] = "/tmp/noctalia-spread-XXXXXX";
      const char* created = ::mkdtemp(pattern);
      TEST_CHECK(created != nullptr);
      m_directory = created;
      const std::string path = m_directory + "/ipc.sock";
      m_listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_listener >= 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      TEST_CHECK(::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(::listen(m_listener, 4) == 0);
      ::setenv("UMBRIEL_SOCKET", path.c_str(), 1);
      m_worker = std::jthread([this](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
          pollfd fd{.fd = m_listener, .events = POLLIN, .revents = 0};
          if (::poll(&fd, 1, 50) <= 0) {
            continue;
          }
          const int peer = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
          TEST_CHECK(peer >= 0);
          std::string line;
          char ch = 0;
          while (::read(peer, &ch, 1) == 1 && ch != '\n') {
            line += ch;
          }
          const auto request = nlohmann::json::parse(line);
          nlohmann::json reply;
          {
            const std::scoped_lock lock(m_mutex);
            ++requests;
            const auto& args = request.at("args");
            const std::string cmd = request.at("cmd");
            if (cmd == "config-get") {
              reply = {{"ok", {{"value", args[0] == "overview" ? overview() : m_values}, {"override", m_overridden}}}};
            } else if (cmd == "config-set") {
              ++sets;
              if (rejectNext.exchange(false)) {
                reply = {{"err", "test rejection"}};
              } else {
                TEST_CHECK(args[0] == "overview.spread");
                const auto table = toml::parse("spread = " + args[1].get<std::string>());
                for (const auto& [key, node] : *table["spread"].as_table()) {
                  if (node.is_boolean()) {
                    m_values[key.str()] = *node.value<bool>();
                  } else {
                    m_values[key.str()] = *node.value<double>();
                  }
                }
                m_overridden = true;
                reply = {{"ok", nullptr}};
              }
            } else {
              TEST_CHECK(cmd == "config-unset" && args[0] == "overview.spread");
              ++unsets;
              m_overridden = false;
              m_values = defaults;
              reply = {{"ok", nullptr}};
            }
          }
          const std::string response = reply.dump() + '\n';
          TEST_CHECK(
              ::send(peer, response.data(), response.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(response.size())
          );
          ::close(peer);
        }
      });
    }
    ~FakeUmbriel() {
      m_worker.request_stop();
      m_worker.join();
      ::close(m_listener);
      std::filesystem::remove_all(m_directory);
    }
    void externalChange() {
      const std::scoped_lock lock(m_mutex);
      m_values["dim"] = 0.9;
    }
    std::atomic<bool> supported = false;
    std::atomic<bool> rejectNext = false;
    std::atomic<int> requests = 0;
    std::atomic<int> sets = 0;
    std::atomic<int> unsets = 0;
    const nlohmann::json defaults{
        {"all_workspaces", true}, {"all_outputs", true}, {"dim", 0.4}, {"wallpaper_dim", 0.0}, {"duration_ms", 250}
    };

  private:
    nlohmann::json overview() const {
      return supported ? nlohmann::json{{"spread", m_values}} : nlohmann::json{{"zoom", 0.5}};
    }
    std::string m_directory;
    int m_listener = -1;
    std::jthread m_worker;
    std::mutex m_mutex;
    bool m_overridden = false;
    nlohmann::json m_values = defaults;
  };
} // namespace

int main() {
  FakeUmbriel server;
  compositors::umbriel::UmbrielRuntime older;
  TEST_CHECK(!older.spreadSettingsSupported());
  const int queried = server.requests;
  TEST_CHECK(!older.spreadSettingsSupported() && server.requests == queried);
  TEST_CHECK(older.applySpreadSettings(std::nullopt));
  TEST_CHECK(server.sets == 0);

  server.supported = true;
  compositors::umbriel::UmbrielRuntime runtime;
  TEST_CHECK(runtime.spreadSettingsSupported());
  auto desired = server.defaults;
  desired["all_workspaces"] = false;
  desired["dim"] = 0.6;
  server.rejectNext = true;
  TEST_CHECK(!runtime.applySpreadSettings(desired));
  TEST_CHECK(runtime.applySpreadSettings(desired));
  TEST_CHECK(server.sets == 2);
  TEST_CHECK(runtime.applySpreadSettings(desired));
  TEST_CHECK(server.sets == 2); // Unrelated Noctalia config reloads must not reload Umbriel.
  TEST_CHECK(runtime.applySpreadSettings(std::nullopt));
  TEST_CHECK(server.unsets == 1);
  TEST_CHECK(runtime.applySpreadSettings(desired));
  server.externalChange();
  TEST_CHECK(runtime.applySpreadSettings(std::nullopt));
  TEST_CHECK(server.unsets == 1); // A newer external runtime override belongs to its author.
  return EXIT_SUCCESS;
}
