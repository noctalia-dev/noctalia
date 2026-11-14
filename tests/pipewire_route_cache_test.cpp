#include "pipewire/audio_glyphs.h"
#include "system/v4l2_camera.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define private public
#include "pipewire/pipewire_service.h"
#undef private

namespace {

  constexpr std::uint32_t kDeviceId = 7;
  constexpr std::uint32_t kInputId = 42;
  constexpr std::uint32_t kOutputId = 43;
  constexpr std::uint32_t kOtherInputId = 44;

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

  struct NodeProxy {
    spa_interface interface;
    std::vector<bool> muteWrites;
    int routeEnumerations = 0;

    NodeProxy() {
      static const pw_node_methods methods = {
          .version = PW_VERSION_NODE_METHODS,
          .enum_params =
              [](void* object, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod*) {
                if (id == SPA_PARAM_Route) {
                  ++static_cast<NodeProxy*>(object)->routeEnumerations;
                }
                return 0;
              },
          .set_param =
              [](void* object, std::uint32_t id, std::uint32_t, const spa_pod* param) {
                if (id == SPA_PARAM_Props) {
                  bool muted = false;
                  if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, nullptr, SPA_PROP_mute, SPA_POD_Bool(&muted))
                      >= 0) {
                    static_cast<NodeProxy*>(object)->muteWrites.push_back(muted);
                  }
                }
                return 0;
              },
      };
      interface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &methods, this);
    }

    pw_node* handle() { return reinterpret_cast<pw_node*>(&interface); }
  };

  struct DeviceProxy {
    spa_interface interface;
    int routeEnumerations = 0;

    DeviceProxy() {
      static const pw_device_methods methods = {
          .version = PW_VERSION_DEVICE_METHODS,
          .enum_params = [](void* object, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod*) {
            if (id == SPA_PARAM_Route) {
              ++static_cast<DeviceProxy*>(object)->routeEnumerations;
            }
            return 0;
          },
      };
      interface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, &methods, this);
    }

    pw_device* handle() { return reinterpret_cast<pw_device*>(&interface); }
  };

  class Fixture {
  public:
    Fixture() {
      TEST_CHECK(service.coreHandle() == nullptr);
      addNode(kInputId, "test-input", "Audio/Source", 1, input.handle());
      addNode(kOutputId, "test-output", "Audio/Sink", 0, nullptr);
      addNode(kOtherInputId, "test-other-input", "Audio/Source", 2, nullptr);
      service.m_defaultSourceName = "test-input";
      service.m_defaultSinkName = "test-output";
      PipeWireService::DeviceData device;
      device.service = &service;
      device.id = kDeviceId;
      device.proxy = card.handle();
      service.m_devices.emplace(kDeviceId, std::move(device));
      service.rebuildState();
    }

    ~Fixture() {
      service.m_nodes.at(kInputId)->proxy = nullptr;
      service.m_devices.at(kDeviceId).proxy = nullptr;
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void deviceInfo(std::optional<std::uint32_t> routeFlags) {
      spa_param_info route{.id = SPA_PARAM_Route, .flags = routeFlags.value_or(0)};
      pw_device_info info{};
      info.change_mask = PW_DEVICE_CHANGE_MASK_PARAMS;
      info.params = routeFlags ? &route : nullptr;
      info.n_params = routeFlags ? 1U : 0U;
      service.onDeviceInfo(kDeviceId, &info);
    }

    void nodeInfo(std::optional<std::uint32_t> routeFlags) {
      spa_param_info route{.id = SPA_PARAM_Route, .flags = routeFlags.value_or(0)};
      pw_node_info info{};
      info.change_mask = PW_NODE_CHANGE_MASK_PARAMS;
      info.params = routeFlags ? &route : nullptr;
      info.n_params = routeFlags ? 1U : 0U;
      service.onNodeInfo(kInputId, &info);
    }

    void route(bool device, std::uint32_t slot, const PipeWireService::DeviceRouteData& data) {
      std::array<std::uint8_t, 128> propsBuffer{};
      spa_pod_builder propsBuilder = SPA_POD_BUILDER_INIT(propsBuffer.data(), propsBuffer.size());
      const auto* props = static_cast<spa_pod*>(spa_pod_builder_add_object(
          &propsBuilder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(data.muted)
      ));
      std::array<std::uint8_t, 512> buffer{};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
      const auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route, SPA_PARAM_ROUTE_index, SPA_POD_Int(data.index),
          SPA_PARAM_ROUTE_device, SPA_POD_Int(data.device), SPA_PARAM_ROUTE_direction, SPA_POD_Id(data.direction),
          SPA_PARAM_ROUTE_priority, SPA_POD_Int(data.priority), SPA_PARAM_ROUTE_available, SPA_POD_Id(data.available),
          SPA_PARAM_ROUTE_props, SPA_POD_Pod(props)
      ));
      if (device) {
        service.onDeviceParam(kDeviceId, SPA_PARAM_Route, slot, slot + 1, pod);
      } else {
        service.onNodeParam(kInputId, SPA_PARAM_Route, slot, slot + 1, pod);
      }
    }

    bool inputMuted() const {
      TEST_CHECK(service.defaultSource() != nullptr);
      return service.defaultSource()->muted;
    }

    bool otherInputMuted() const {
      const auto& sources = service.state().sources;
      const auto source = std::ranges::find(sources, kOtherInputId, &AudioNode::id);
      TEST_CHECK(source != sources.end());
      return source->muted;
    }

    PipeWireService service;
    NodeProxy input;
    DeviceProxy card;

  private:
    void addNode(std::uint32_t id, std::string name, std::string mediaClass, std::int32_t profile, pw_node* proxy) {
      auto node = std::make_unique<PipeWireService::NodeData>();
      node->service = &service;
      node->id = id;
      node->name = std::move(name);
      node->mediaClass = std::move(mediaClass);
      node->profileDevice = profile;
      node->deviceId = kDeviceId;
      node->proxy = proxy;
      service.m_nodes.emplace(id, std::move(node));
    }
  };

  const PipeWireService::DeviceRouteData kOutputRoute{
      .index = 100, .device = 0, .direction = SPA_DIRECTION_OUTPUT, .muted = true
  };
  const PipeWireService::DeviceRouteData kOldInputRoute{
      .index = 101, .device = 1, .direction = SPA_DIRECTION_INPUT, .priority = 1000, .muted = true
  };
  const PipeWireService::DeviceRouteData kOtherInputRoute{
      .index = 102, .device = 2, .direction = SPA_DIRECTION_INPUT, .priority = 2000, .muted = true
  };
  const PipeWireService::DeviceRouteData kNewInputRoute{
      .index = 103, .device = 1, .direction = SPA_DIRECTION_INPUT, .priority = 10, .muted = false
  };

  void verifyDeviceRoutes() {
    Fixture fixture;
    fixture.deviceInfo(SPA_PARAM_INFO_READ);
    fixture.route(true, 4, kOutputRoute);
    fixture.route(true, 7, kOldInputRoute);
    fixture.route(true, 9, kOtherInputRoute);
    TEST_CHECK(fixture.inputMuted());

    fixture.deviceInfo(SPA_PARAM_INFO_READ);
    fixture.route(true, 4, kOutputRoute);
    fixture.route(true, 7, kNewInputRoute);
    fixture.route(true, 9, kOtherInputRoute);
    fixture.service.onMixerVolumeChanged(kInputId, 1.0F, false);
    TEST_CHECK(!fixture.inputMuted());
    TEST_CHECK(fixture.service.defaultSink()->muted);
    TEST_CHECK(fixture.otherInputMuted());
    TEST_CHECK(fixture.card.routeEnumerations == 2);

    fixture.service.setMicMuted(!fixture.inputMuted());
    fixture.service.setMicMuted(!fixture.inputMuted());
    TEST_CHECK(fixture.input.muteWrites == std::vector<bool>({true, false}));
    TEST_CHECK(!fixture.inputMuted());

    fixture.route(true, 7, kOldInputRoute);
    TEST_CHECK(fixture.inputMuted());
    fixture.deviceInfo(SPA_PARAM_INFO_READ);
    TEST_CHECK(!fixture.inputMuted());

    fixture.route(true, 7, kOldInputRoute);
    fixture.deviceInfo(0);
    TEST_CHECK(!fixture.inputMuted());

    fixture.route(true, 7, kOldInputRoute);
    fixture.deviceInfo(std::nullopt);
    TEST_CHECK(!fixture.inputMuted());
  }

  void verifyNodeRoutes() {
    Fixture fixture;
    fixture.nodeInfo(SPA_PARAM_INFO_READ);
    fixture.route(false, 7, kOldInputRoute);
    TEST_CHECK(fixture.inputMuted());

    fixture.nodeInfo(SPA_PARAM_INFO_READ);
    fixture.route(false, 7, kNewInputRoute);
    fixture.service.onMixerVolumeChanged(kInputId, 1.0F, false);
    TEST_CHECK(!fixture.inputMuted());
    TEST_CHECK(fixture.input.routeEnumerations == 2);

    fixture.route(false, 7, kOldInputRoute);
    fixture.nodeInfo(SPA_PARAM_INFO_READ);
    TEST_CHECK(!fixture.inputMuted());

    fixture.route(false, 7, kOldInputRoute);
    fixture.nodeInfo(0);
    TEST_CHECK(!fixture.inputMuted());

    fixture.route(false, 7, kOldInputRoute);
    fixture.nodeInfo(std::nullopt);
    TEST_CHECK(!fixture.inputMuted());
  }

} // namespace

int main() {
  std::array<char, 64> directory{};
  constexpr std::string_view pattern = "/tmp/noctalia-pipewire-route-test.XXXXXX";
  std::ranges::copy(pattern, directory.begin());
  TEST_CHECK(mkdtemp(directory.data()) != nullptr);
  {
    const EnvironmentVariable runtime("PIPEWIRE_RUNTIME_DIR", directory.data());
    const EnvironmentVariable remote("PIPEWIRE_REMOTE", std::string(directory.data()) + "/absent");
    verifyDeviceRoutes();
    verifyNodeRoutes();
  }
  TEST_CHECK(std::filesystem::remove(directory.data()));
}
