#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/wireplumber_mixer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <print>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  // No hardware, session manager, user configuration, or desktop audio socket is used.
  class Daemon {
  public:
    explicit Daemon(const char* executable) : m_executable(executable) {
      char path[] = "/tmp/noctalia-pipewire-test-XXXXXX";
      const auto* dir = mkdtemp(path);
      require(dir != nullptr, "mkdtemp failed");
      m_dir = dir;
      setenv("PIPEWIRE_RUNTIME_DIR", m_dir.c_str(), 1);
      setenv("PIPEWIRE_REMOTE", "noctalia-test", 1);
      setenv("PIPEWIRE_CONFIG_DIR", m_dir.c_str(), 1);
      setenv("PIPEWIRE_CONFIG_PREFIX", m_dir.c_str(), 1);
      setenv("PIPEWIRE_CONFIG_NAME", "client.conf", 1);
      // Avoid loading the user's client modules (including RTKit/D-Bus) in the test clients.
      std::ofstream(m_dir / "client.conf") << R"(
context.properties = { support.dbus = false }
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
]
)";
    }
    ~Daemon() {
      stop();
      std::filesystem::remove_all(m_dir);
    }

    void start(const std::string& suffix) {
      const auto configPath = m_dir / "daemon.conf";
      std::ofstream config(configPath);
      config << R"(
context.properties = { core.daemon = true core.name = noctalia-test support.dbus = false }
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access args = { access.legacy = true } }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
context.objects = [
    { factory = metadata args = { metadata.name = default metadata.values = [
)";
      config
          << "{ key = default.audio.sink type = \"Spa:String:JSON\" value = { \"name\": \"sink-"
          << suffix
          << "\" } }\n";
      config
          << "{ key = default.audio.source type = \"Spa:String:JSON\" value = { \"name\": \"source-"
          << suffix
          << "\" } }\n";
      config << "] } }\n";
      for (const auto* kind : {"sink", "source"}) {
        config
            << "{ factory = adapter args = { factory.name = support.null-audio-sink node.name = "
            << kind
            << '-'
            << suffix
            << " media.class = Audio/"
            << (std::string(kind) == "sink" ? "Sink" : "Source")
            << " audio.position = [ FL FR ] node.param.Props = { channelVolumes = [ 0.125 0.125 ] } } }\n";
      }
      config << "]\n";
      config.close();
      // Spawn without running library code in a forked child: the clients own PipeWire threads.
      std::vector<char*> env;
      for (char** entry = environ; *entry != nullptr; ++entry) {
        if (!std::string_view(*entry).starts_with("PIPEWIRE_CONFIG_NAME=")) {
          env.push_back(*entry);
        }
      }
      env.push_back(nullptr);
      char configFlag[] = "-c";
      char configName[] = "daemon.conf";
      char* args[] = {const_cast<char*>(m_executable), configFlag, configName, nullptr};
      require(posix_spawn(&m_pid, m_executable, nullptr, nullptr, args, env.data()) == 0, "posix_spawn failed");
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!std::filesystem::exists(m_dir / "noctalia-test")) {
        require(std::chrono::steady_clock::now() < deadline, "daemon socket did not appear");
        std::this_thread::sleep_for(10ms);
      }
    }

    void stop() {
      if (m_pid > 0) {
        kill(m_pid, SIGTERM);
        while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {
        }
        m_pid = -1;
      }
    }

  private:
    const char* m_executable;
    std::filesystem::path m_dir;
    pid_t m_pid = -1;
  };

  class DefaultSelection {
  public:
    explicit DefaultSelection(pw_core* core) {
      m_registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      static const pw_registry_events events = {
          .version = PW_VERSION_REGISTRY_EVENTS,
          .global = [](void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t,
                       const spa_dict* props) {
            auto* self = static_cast<DefaultSelection*>(data);
            if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0 || props == nullptr) {
              return;
            }
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name == nullptr || std::strcmp(name, "default") != 0) {
              return;
            }
            self->m_metadata =
                static_cast<pw_metadata*>(pw_registry_bind(self->m_registry, id, type, PW_VERSION_METADATA, 0));
            static const pw_metadata_events metadataEvents = {
                .version = PW_VERSION_METADATA_EVENTS,
                .property = [](void* data, std::uint32_t, const char* key, const char*, const char* value) -> int {
                  auto* self = static_cast<DefaultSelection*>(data);
                  if (key != nullptr && value != nullptr) {
                    if (std::strcmp(key, "default.configured.audio.sink") == 0) {
                      self->sink = value;
                    }
                    if (std::strcmp(key, "default.configured.audio.source") == 0) {
                      self->source = value;
                    }
                  }
                  return 0;
                },
            };
            pw_metadata_add_listener(self->m_metadata, &self->m_metadataListener, &metadataEvents, self);
          },
      };
      pw_registry_add_listener(m_registry, &m_registryListener, &events, this);
    }
    ~DefaultSelection() {
      if (m_metadata != nullptr) {
        spa_hook_remove(&m_metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_metadata));
      }
      spa_hook_remove(&m_registryListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
    }
    std::string sink;
    std::string source;

  private:
    pw_registry* m_registry = nullptr;
    pw_metadata* m_metadata = nullptr;
    spa_hook m_registryListener{};
    spa_hook m_metadataListener{};
  };

  class Audio {
  public:
    PipeWireService service;
    WirePlumberMixer mixer;
    PipeWireSpectrum spectrum{service};
    PipeWirePollSource source{service};
    unsigned changes = 0;
    unsigned mixerChanges = 0;

    Audio() {
      service.setWirePlumberMixer(&mixer);
      mixer.setChangeCallback([this](std::uint32_t id, float volume, bool muted) {
        ++mixerChanges;
        service.onMixerVolumeChanged(id, volume, muted);
      });
      service.setChangeCallback([this] {
        ++changes;
        spectrum.handleAudioStateChanged();
      });
      spectrum.addChangeListener(16, [] {});
    }

    // Exercise the actual PollSource bridge, including timer-only recovery while the daemon is down.
    void step(int maxWaitMs = 50, bool pumpDiscovery = true) {
      std::vector<pollfd> fds;
      const auto sourceStart = pumpDiscovery ? source.addPollFds(fds) : 0;
      const auto mixerStart = mixer.addPollFds(fds);
      int timeout = maxWaitMs;
      for (int value : {pumpDiscovery ? source.pollTimeoutMs() : -1, mixer.pollTimeoutMs()}) {
        if (value >= 0) {
          timeout = std::min(timeout, value);
        }
      }
      poll(fds.data(), fds.size(), timeout);
      if (pumpDiscovery) {
        source.dispatch(fds, sourceStart);
      }
      mixer.dispatch(fds, mixerStart);
    }

    void until(const std::function<bool()>& condition, const char* message) {
      const auto deadline = std::chrono::steady_clock::now() + 8s;
      while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        step();
      }
    }

    void expectDevices(const std::string& suffix) {
      until(
          [&] {
            const auto* sink = service.defaultSink();
            const auto* input = service.defaultSource();
            return mixer.ready()
                && sink != nullptr
                && input != nullptr
                && sink->name == "sink-" + suffix
                && input->name == "source-" + suffix
                && std::abs(sink->volume - 0.5F) < 0.01F
                && std::abs(input->volume - 0.5F) < 0.01F;
          },
          "devices/defaults did not recover"
      );
      require(service.state().sinks.size() == 1 && service.state().sources.size() == 1, "stale devices retained");
    }

    void expectControls() {
      const auto before = mixerChanges;
      const auto id = service.defaultSink()->id;
      // Call the mixer directly so an optimistic UI update cannot make this assertion pass.
      mixer.setVolume(id, 0.37F);
      mixer.setMuted(id, true);
      until(
          [&] {
            const auto* sink = service.defaultSink();
            return mixerChanges > before && sink != nullptr && std::abs(sink->volume - 0.37F) < 0.01F && sink->muted;
          },
          "volume/mute writes or subscriptions did not recover"
      );
      mixer.setMuted(id, false);
      until([&] { return !service.defaultSink()->muted; }, "unmute did not recover");
      DefaultSelection selection(service.coreHandle());
      const auto sinkName = service.defaultSink()->name;
      const auto sourceName = service.defaultSource()->name;
      mixer.setDefaultNode(service.defaultSink()->id);
      mixer.setDefaultNode(service.defaultSource()->id);
      until(
          [&] {
            return selection.sink.find(sinkName) != std::string::npos
                && selection.source.find(sourceName) != std::string::npos;
          },
          "default device selection did not reach the daemon"
      );
      const spa_dict_item props[] = {
          {"factory.name", "support.null-audio-sink"},
          {"node.name", "hotplug-sink"},
          {"media.class", "Audio/Sink"},
          {"audio.position", "[ FL FR ]"}
      };
      const spa_dict properties = SPA_DICT_INIT_ARRAY(props);
      auto* hotplug = static_cast<pw_proxy*>(pw_core_create_object(
          service.coreHandle(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &properties, 0
      ));
      require(hotplug != nullptr, "could not create hotplug sink");
      until([&] { return service.state().sinks.size() == 2; }, "new devices no longer discovered");
      pw_proxy_destroy(hotplug);
      until([&] { return service.state().sinks.size() == 1; }, "removed devices retained");
    }
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 1;
  }
  try {
    Daemon daemon(argv[1]);
    daemon.start("initial");
    Audio audio;
    auto* loop = audio.service.loop();
    audio.expectDevices("initial");
    audio.expectControls();

    for (int restart = 0; restart < 3; ++restart) {
      const auto changes = audio.changes;
      daemon.stop();
      audio.until(
          [&] {
            return audio.service.state().sinks.empty() && audio.service.state().sources.empty() && !audio.mixer.ready();
          },
          "disconnect did not clear audio state/readiness"
      );
      require(audio.changes > changes, "disconnect did not notify UI consumers");
      require(audio.service.privacyState().captures.empty(), "stale privacy captures retained");
      require(audio.service.coreHandle() == nullptr, "disconnected core retained");
      require(audio.source.pollTimeoutMs() > 0, "missing or busy-looping reconnect deadline");

      // Give both clients time to retry against an unavailable daemon.
      const auto unavailableUntil = std::chrono::steady_clock::now() + 1200ms;
      unsigned dispatches = 0;
      while (std::chrono::steady_clock::now() < unavailableUntil) {
        audio.step();
        require(++dispatches < 200, "audio poll sources busy-loop while disconnected");
      }
      const auto suffix = "restart-" + std::to_string(restart);
      daemon.start(suffix);
      // Force the mixer to receive its initial volume snapshots before discovery reconnects.
      const auto mixerDeadline = std::chrono::steady_clock::now() + 8s;
      while (!audio.mixer.ready()) {
        require(std::chrono::steady_clock::now() < mixerDeadline, "mixer did not reconnect first");
        audio.step(50, false);
      }
      audio.expectDevices(suffix);
      require(audio.service.loop() == loop, "shared PipeWire loop replaced");
      audio.expectControls();
    }
    // Destruction with a scheduled retry must release timer sources/callbacks safely.
    daemon.stop();
    audio.until(
        [&] { return !audio.mixer.ready() && audio.service.coreHandle() == nullptr; }, "final disconnect failed"
    );
    {
      // Starting the shell before PipeWire should not permanently disable audio.
      Audio lateStart;
      daemon.start("late");
      lateStart.expectDevices("late");
      lateStart.expectControls();
    }
    for (int step = 0; step < 5; ++step) {
      // Destroy clients at different points in asynchronous connection/module activation.
      Audio shortLived;
      for (int i = 0; i < step; ++i) {
        shortLived.step(0);
      }
    }
    {
      Audio interrupted;
      interrupted.step();
      daemon.stop();
      interrupted.until(
          [&] { return interrupted.service.coreHandle() == nullptr && !interrupted.mixer.ready(); },
          "disconnect during activation failed"
      );
      daemon.start("activation");
      interrupted.expectDevices("activation");
      interrupted.expectControls();
    }
    std::println(
        "PipeWire reconnection: startup, repeated restarts, state clearing, volume/mute and device selection passed"
    );
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "pipewire_reconnect_test: {}", error.what());
    return 1;
  }
}
