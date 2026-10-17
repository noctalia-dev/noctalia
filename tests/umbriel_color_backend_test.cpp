#include "compositors/umbriel/umbriel_color_backend.h"
#include "compositors/umbriel/umbriel_runtime.h"
#include "test_check.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using compositors::umbriel::brightnessFromSdrWhite;
using compositors::umbriel::controlsBrightness;
using compositors::umbriel::parseColorStates;
using compositors::umbriel::rendersHdrNow;
using compositors::umbriel::requestColorStates;
using compositors::umbriel::sdrWhiteFromBrightness;
using compositors::umbriel::setOutputBrightness;
using compositors::umbriel::UmbrielRuntime;
using namespace std::chrono_literals;

namespace {

  [[nodiscard]] nlohmann::json colorReply() {
    return nlohmann::json::parse(R"({"ok":{"color_manager":true,"outputs":[
      {"name":"DP-2","enabled":true,"hdr_active":true,"hdr_mode":"on","sdr_white":203.0},
      {"name":"DP-3","enabled":true,"hdr_active":false,"hdr_mode":"auto","sdr_white":141.5},
      {"name":"DP-4","enabled":true,"hdr_active":false,"hdr_mode":"off","sdr_white":203.0},
      {"name":"DP-5","enabled":true,"hdr_active":true,"hdr_mode":"fullscreen"},
      {"name":"DP-7","enabled":true,"hdr_mode":"on","sdr_white":120.0},
      {"hdr_mode":"on","sdr_white":203.0},
      {"name":"DP-6","enabled":true}]}})");
  }

  // One newline-delimited request per connection, answered with the configured reply.
  class Server {
  public:
    Server() {
      char directory[] = "/tmp/noctalia-color-test-XXXXXX";
      const auto* created = mkdtemp(directory);
      TEST_CHECK(created != nullptr);
      m_directory = created;
      m_path = m_directory + "/umbriel.sock";
      TEST_CHECK(setenv("UMBRIEL_SOCKET", m_path.c_str(), 1) == 0);

      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      TEST_CHECK(fd >= 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
      TEST_CHECK(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(listen(fd, 4) == 0);
      m_fd = fd;
      m_thread = std::thread([this]() { serve(); });
    }

    ~Server() {
      m_stop.store(true);
      m_thread.join();
      close(m_fd);
      unlink(m_path.c_str());
      rmdir(m_directory.c_str());
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void setReply(std::string reply) {
      const std::scoped_lock lock(m_mutex);
      m_reply = std::move(reply);
    }

    // The `arg` of the last request, which carries every shell action.
    [[nodiscard]] std::string lastAction() const {
      const std::scoped_lock lock(m_mutex);
      if (m_request.empty()) {
        return {};
      }
      return nlohmann::json::parse(m_request).value("arg", "");
    }

  private:
    void serve() {
      while (!m_stop.load()) {
        const int client = accept4(m_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
          std::this_thread::sleep_for(2ms);
          continue;
        }

        std::string input;
        std::array<char, 4096> data{};
        while (!m_stop.load()) {
          const auto size = recv(client, data.data(), data.size(), 0);
          if (size > 0) {
            input.append(data.data(), static_cast<std::size_t>(size));
            if (input.contains('\n')) {
              break;
            }
            continue;
          }
          if (size == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            break;
          }
          std::this_thread::sleep_for(2ms);
        }

        std::string reply;
        {
          const std::scoped_lock lock(m_mutex);
          m_request = input.substr(0, input.find('\n'));
          reply = m_reply;
        }
        reply.push_back('\n');
        const ssize_t ignored = send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        (void)ignored;
        close(client);
      }
    }

    std::string m_directory;
    std::string m_path;
    std::string m_reply;
    std::string m_request;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    int m_fd = -1;
  };

  void checkMapping() {
    TEST_CHECK(sdrWhiteFromBrightness(0.0F) == 80.0F);
    TEST_CHECK(sdrWhiteFromBrightness(1.0F) == 203.0F);
    TEST_CHECK(std::abs(sdrWhiteFromBrightness(0.5F) - 141.5F) < 0.001F);
    TEST_CHECK(sdrWhiteFromBrightness(-1.0F) == 80.0F);
    TEST_CHECK(sdrWhiteFromBrightness(4.0F) == 203.0F);

    TEST_CHECK(brightnessFromSdrWhite(80.0F) == 0.0F);
    TEST_CHECK(brightnessFromSdrWhite(203.0F) == 1.0F);
    TEST_CHECK(std::abs(brightnessFromSdrWhite(141.5F) - 0.5F) < 0.001F);
    TEST_CHECK(brightnessFromSdrWhite(0.0F) == 0.0F);
    TEST_CHECK(brightnessFromSdrWhite(5000.0F) == 1.0F);

    const auto roundTrip = [](float brightness) {
      return std::abs(brightnessFromSdrWhite(sdrWhiteFromBrightness(brightness)) - brightness) < 0.001F;
    };
    TEST_CHECK(roundTrip(0.0F));
    TEST_CHECK(roundTrip(0.25F));
    TEST_CHECK(roundTrip(0.75F));
    TEST_CHECK(roundTrip(1.0F));
  }

  void checkModes() {
    TEST_CHECK(controlsBrightness(compositors::umbriel::OutputColorState{.hdrMode = "on"}));
    TEST_CHECK(controlsBrightness(compositors::umbriel::OutputColorState{.hdrMode = "auto"}));
    TEST_CHECK(controlsBrightness(compositors::umbriel::OutputColorState{.hdrMode = "fullscreen"}));
    TEST_CHECK(!controlsBrightness(compositors::umbriel::OutputColorState{.hdrMode = "off"}));
    TEST_CHECK(!controlsBrightness(compositors::umbriel::OutputColorState{}));
  }

  void checkParse() {
    const auto states = parseColorStates(colorReply()["ok"]);
    TEST_CHECK(states.size() == 5);

    const auto hdr = states.find("DP-2");
    TEST_CHECK(hdr != states.end());
    TEST_CHECK(hdr->second.hdrMode == "on");
    TEST_CHECK(hdr->second.sdrWhite == 203.0F);
    TEST_CHECK(controlsBrightness(hdr->second));

    // An absent SDR white stays unset; the mode still routes brightness to the compositor.
    const auto fullscreen = states.find("DP-5");
    TEST_CHECK(fullscreen != states.end());
    TEST_CHECK(fullscreen->second.hdrMode == "fullscreen");
    TEST_CHECK(fullscreen->second.sdrWhite == 0.0F);

    const auto sdr = states.find("DP-4");
    TEST_CHECK(sdr != states.end());
    TEST_CHECK(!controlsBrightness(sdr->second));

    // While the output renders HDR its panel value is no longer the level the shell may show.
    TEST_CHECK(rendersHdrNow(states.at("DP-2")));
    TEST_CHECK(rendersHdrNow(states.at("DP-5")));
    TEST_CHECK(!rendersHdrNow(states.at("DP-3")));
    TEST_CHECK(!rendersHdrNow(states.at("DP-4")));
    TEST_CHECK(!rendersHdrNow(states.at("DP-7")));

    // An unusable payload leaves the panel controls in charge.
    TEST_CHECK(parseColorStates(nlohmann::json::parse(R"({"outputs":"none"})")).empty());
    TEST_CHECK(parseColorStates(nlohmann::json::parse(R"({"outputs":[]})")).empty());
    TEST_CHECK(parseColorStates(nlohmann::json::parse(R"({"color_manager":false})")).empty());
    TEST_CHECK(parseColorStates(nlohmann::json::parse(R"("surfaces")")).empty());
  }

  void checkRequests() {
    Server server;
    server.setReply(colorReply().dump());
    UmbrielRuntime runtime;

    const auto states = requestColorStates(runtime);
    TEST_CHECK(states.size() == 5);
    TEST_CHECK(states.contains("DP-3"));
    TEST_CHECK(controlsBrightness(states.at("DP-3")));

    TEST_CHECK(setOutputBrightness(runtime, "DP-2", 0.5F));
    TEST_CHECK(server.lastAction() == "output-sdr-white:141.5/DP-2");

    TEST_CHECK(setOutputBrightness(runtime, "DP-2", 0.0F));
    TEST_CHECK(server.lastAction() == "output-sdr-white:80.0/DP-2");

    // An empty connector leaves the output target off the action, so every output follows.
    TEST_CHECK(setOutputBrightness(runtime, "", 1.0F));
    TEST_CHECK(server.lastAction() == "output-sdr-white:203.0");

    // A session without a colour manager reports an error and keeps the panel controls.
    server.setReply(R"({"err":"no color manager"})");
    TEST_CHECK(requestColorStates(runtime).empty());
    TEST_CHECK(!setOutputBrightness(runtime, "DP-2", 0.5F));
  }

} // namespace

int main() {
  checkMapping();
  checkModes();
  checkParse();
  checkRequests();
  return 0;
}
