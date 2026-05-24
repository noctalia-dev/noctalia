#include "pipewire/pipewire_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/process.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/type.h>
#include <string>
#include <string_view>

namespace {

  constexpr float kDefaultVolumeStep = 0.05f;
  constexpr auto kVolumeApplyMinInterval = std::chrono::milliseconds(25);

  // Registry events.
  void onRegistryGlobal(
      void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t version, const spa_dict* props
  ) {
    auto* svc = static_cast<PipeWireService*>(data);
    svc->onRegistryGlobal(id, type, version, props);
  }

  void onRegistryGlobalRemove(void* data, std::uint32_t id) {
    auto* svc = static_cast<PipeWireService*>(data);
    svc->onRegistryGlobalRemove(id);
  }

  const pw_registry_events kRegistryEvents = {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = onRegistryGlobal,
      .global_remove = onRegistryGlobalRemove,
  };

  void onClientInfo(void* data, const pw_client_info* info) {
    auto* client = static_cast<PipeWireService::ClientData*>(data);
    client->service->onClientInfo(client->id, info);
  }

  const pw_client_events kClientEvents = {
      .version = PW_VERSION_CLIENT_EVENTS,
      .info = onClientInfo,
      .permissions = nullptr,
  };

  // Device events
  void onDeviceInfo(void* data, const pw_device_info* info) {
    auto* dev = static_cast<PipeWireService::DeviceData*>(data);
    dev->service->onDeviceInfo(dev->id, info);
  }

  void onDeviceParam(void* data, int, std::uint32_t id, std::uint32_t index, std::uint32_t next, const spa_pod* param) {
    auto* dev = static_cast<PipeWireService::DeviceData*>(data);
    dev->service->onDeviceParam(dev->id, id, index, next, param);
  }

  const pw_device_events kDeviceEvents = {
      .version = PW_VERSION_DEVICE_EVENTS,
      .info = onDeviceInfo,
      .param = onDeviceParam,
  };

  // Node events.
  void onNodeInfo(void* data, const pw_node_info* info) {
    auto* nd = static_cast<PipeWireService::NodeData*>(data);
    nd->service->onNodeInfo(nd->id, info);
  }

  void onNodeParam(void* data, int, std::uint32_t id, std::uint32_t index, std::uint32_t next, const spa_pod* param) {
    auto* nd = static_cast<PipeWireService::NodeData*>(data);
    nd->service->onNodeParam(nd->id, id, index, next, param);
  }

  const pw_node_events kNodeEvents = {
      .version = PW_VERSION_NODE_EVENTS,
      .info = onNodeInfo,
      .param = onNodeParam,
  };

  // default.audio.{sink,source} values are often JSON {"name":"…"} but may be a plain node.name string.
  std::string extractDefaultMetadataNodeName(std::string_view val) {
    constexpr std::string_view kNameKey = "\"name\"";
    const auto namePos = val.find(kNameKey);
    if (namePos != std::string_view::npos) {
      const auto colonPos = val.find(':', namePos + kNameKey.size());
      if (colonPos != std::string_view::npos) {
        std::size_t i = colonPos + 1;
        while (i < val.size() && (val[i] == ' ' || val[i] == '\t')) {
          ++i;
        }
        if (i < val.size() && val[i] == '"') {
          const std::size_t v0 = i + 1;
          const auto v1 = val.find('"', v0);
          if (v1 != std::string_view::npos && v1 > v0) {
            return std::string(val.substr(v0, v1 - v0));
          }
        }
      }
    }

    std::string_view s = val;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
      s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
      s.remove_suffix(1);
    }
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
      s = s.substr(1, s.size() - 2);
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
      }
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
      }
    }
    if (!s.empty()) {
      const char c = s.front();
      if (c != '{' && c != '[') {
        return std::string(s);
      }
    }
    return {};
  }

  // Default sink/source metadata.
  struct MetadataData {
    PipeWireService* service = nullptr;
    struct pw_metadata* proxy = nullptr;
    spa_hook* listener = nullptr;
  };

  int onMetadataProperty(void* data, std::uint32_t, const char* key, const char*, const char* value) {
    if (key == nullptr || value == nullptr) {
      return 0;
    }
    auto* md = static_cast<MetadataData*>(data);
    if (std::strcmp(key, "default.audio.sink") == 0 || std::strcmp(key, "default.audio.source") == 0) {
      const std::string name = extractDefaultMetadataNodeName(std::string_view(value));
      if (!name.empty()) {
        spa_dict_item items[1];
        items[0] = SPA_DICT_ITEM_INIT(key, name.c_str());
        spa_dict dict = SPA_DICT_INIT(items, 1);
        md->service->parseDefaultNodes(&dict);
      }
    }
    return 0;
  }

  const pw_metadata_events kMetadataEvents = {
      .version = PW_VERSION_METADATA_EVENTS,
      .property = onMetadataProperty,
  };

  std::string dictGet(const spa_dict* dict, const char* key) {
    if (dict == nullptr) {
      return {};
    }
    const char* val = spa_dict_lookup(dict, key);
    return val != nullptr ? std::string(val) : std::string{};
  }

  std::string escapeJsonString(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());

    for (const char ch : text) {
      if (ch == '\\' || ch == '"') {
        escaped.push_back('\\');
      }
      escaped.push_back(ch);
    }

    return escaped;
  }

  std::uint32_t parseUint32Or(const std::string& value, std::uint32_t fallback = 0) {
    if (value.empty()) {
      return fallback;
    }
    std::uint32_t out = fallback;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
      return fallback;
    }
    return out;
  }

  std::optional<float> parseFloat(const std::string& value) {
    if (value.empty()) {
      return std::nullopt;
    }
    return StringUtils::parseDotDecimal<float>(value);
  }

  std::optional<bool> parseBool(const std::string& value) {
    if (value.empty()) {
      return std::nullopt;
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
      return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
      return false;
    }
    return std::nullopt;
  }

  bool applyClientPropsFromDict(PipeWireService::ClientData& client, const spa_dict* props) {
    if (props == nullptr) {
      return false;
    }

    bool changed = false;
    auto assignIfBetter = [&changed](std::string& field, std::string value) {
      if (!value.empty() && field != value) {
        field = std::move(value);
        changed = true;
      }
    };

    std::string name = dictGet(props, "application.name");
    if (name.empty()) {
      name = dictGet(props, "client.name");
    }
    assignIfBetter(client.name, std::move(name));

    std::string appId = dictGet(props, "application.id");
    if (appId.ends_with(".desktop")) {
      appId.erase(appId.size() - std::string_view(".desktop").size());
    }
    assignIfBetter(client.appId, std::move(appId));

    assignIfBetter(client.binary, dictGet(props, "application.process.binary"));

    std::string iconName = dictGet(props, "application.icon-name");
    if (iconName.empty()) {
      iconName = dictGet(props, "node.icon-name");
    }
    assignIfBetter(client.iconName, std::move(iconName));

    return changed;
  }

  void parseVolumeArrayProp(const spa_pod_prop* prop, float& outVolume, std::uint32_t* outChannelCount = nullptr) {
    if (prop == nullptr) {
      return;
    }
    std::uint32_t nVals = 0;
    std::uint32_t choiceType = SPA_CHOICE_None;
    const spa_pod* inner = spa_pod_get_values(&prop->value, &nVals, &choiceType);
    (void)nVals;
    (void)choiceType;
    if (inner == nullptr) {
      return;
    }
    if (spa_pod_is_array(inner)) {
      const auto* arr = reinterpret_cast<const spa_pod_array*>(inner);
      const auto n = static_cast<std::uint32_t>(SPA_POD_ARRAY_N_VALUES(arr));
      const std::uint32_t elemSize = SPA_POD_ARRAY_VALUE_SIZE(arr);
      const std::uint32_t elemType = SPA_POD_ARRAY_VALUE_TYPE(arr);
      if (n > 0 && elemType == SPA_TYPE_Float && elemSize == sizeof(float)) {
        const auto* samples = static_cast<const float*>(SPA_POD_ARRAY_VALUES(arr));
        float maxVol = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
          const float cubic = samples[i];
          const float linear = std::cbrt(std::max(0.0f, cubic));
          if (linear > maxVol) {
            maxVol = linear;
          }
        }
        outVolume = maxVol;
        if (outChannelCount != nullptr) {
          *outChannelCount = n;
        }
        return;
      }
    }
    float cubic = 0.0f;
    if (spa_pod_get_float(inner, &cubic) == 0) {
      outVolume = std::cbrt(std::max(0.0f, cubic));
      if (outChannelCount != nullptr) {
        *outChannelCount = 1;
      }
    }
  }

  struct ParsedPropsVolumes {
    float channelVol = 1.0f;
    float scalarVol = 1.0f;
    float softVol = 1.0f;
    std::uint32_t channelCount = 0;
    bool hasChannel = false;
    bool hasScalar = false;
    bool hasSoft = false;
  };

  void parsePropsObjectVolumeFields(const spa_pod* propsPod, ParsedPropsVolumes basis, ParsedPropsVolumes* out) {
    *out = basis;
    out->hasChannel = false;
    out->hasScalar = false;
    out->hasSoft = false;
    if (propsPod == nullptr) {
      return;
    }
    auto* obj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(propsPod));
    spa_pod_prop* prop = nullptr;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
      if (prop->key == SPA_PROP_channelVolumes) {
        parseVolumeArrayProp(prop, out->channelVol, &out->channelCount);
        out->hasChannel = true;
      } else if (prop->key == SPA_PROP_volume) {
        std::uint32_t nVals = 0;
        std::uint32_t choiceType = SPA_CHOICE_None;
        const spa_pod* inner = spa_pod_get_values(&prop->value, &nVals, &choiceType);
        (void)nVals;
        (void)choiceType;
        float cubic = 0.0f;
        if (inner != nullptr && spa_pod_get_float(inner, &cubic) == 0) {
          out->scalarVol = std::cbrt(std::max(0.0f, cubic));
          out->hasScalar = true;
        }
      } else if (prop->key == SPA_PROP_softVolumes) {
        parseVolumeArrayProp(prop, out->softVol);
        out->hasSoft = true;
      }
    }
  }

  void mergeParsedVolumesIntoNode(PipeWireService::NodeData& nd, const ParsedPropsVolumes& p) {
    if (p.hasChannel) {
      nd.volume = p.channelVol;
      nd.channelCount = p.channelCount;
    } else if (p.hasScalar) {
      nd.volume = p.scalarVol;
    } else if (p.hasSoft) {
      nd.volume = p.softVol;
    }
  }

  // Device ParamRoute updates are per-direction; applying every route's volume to all nodes on the same
  // device.id merges playback and capture on combo hardware (see activeRouteForDirection).
  [[nodiscard]] bool routeVolumeDirectionMatchesNode(std::string_view mediaClass, std::uint32_t routeDirection) {
    if (mediaClass == "Audio/Sink") {
      return routeDirection == SPA_DIRECTION_OUTPUT;
    }
    if (mediaClass == "Audio/Source") {
      return routeDirection == SPA_DIRECTION_INPUT;
    }
    return true;
  }

  [[nodiscard]] bool routeIsSelectable(const PipeWireService::DeviceRouteData& route, std::uint32_t wantDir) {
    return route.index >= 0 && route.direction == wantDir && route.available != SPA_PARAM_AVAILABILITY_no;
  }

  [[nodiscard]] bool routeIsBetterCandidate(
      const PipeWireService::DeviceRouteData& candidate, const PipeWireService::DeviceRouteData& current
  ) {
    const bool candidateAvailable = candidate.available == SPA_PARAM_AVAILABILITY_yes;
    const bool currentAvailable = current.available == SPA_PARAM_AVAILABILITY_yes;
    if (candidateAvailable != currentAvailable) {
      return candidateAvailable;
    }
    return candidate.priority > current.priority;
  }

  [[nodiscard]] const PipeWireService::DeviceRouteData*
  activeRouteForDirection(const std::vector<PipeWireService::DeviceRouteData>& routes, std::uint32_t wantDir) {
    const PipeWireService::DeviceRouteData* best = nullptr;
    for (const auto& route : routes) {
      if (!routeIsSelectable(route, wantDir)) {
        continue;
      }
      if (best == nullptr || routeIsBetterCandidate(route, *best)) {
        best = &route;
      }
    }
    return best;
  }

  void upsertRoute(std::vector<PipeWireService::DeviceRouteData>& routes, PipeWireService::DeviceRouteData route) {
    const std::int32_t lookupIndex = route.index >= 0 ? route.index : -1;
    if (lookupIndex < 0) {
      return;
    }
    const auto existing = std::find_if(routes.begin(), routes.end(), [lookupIndex](const auto& entry) {
      return entry.index == lookupIndex;
    });
    if (existing == routes.end()) {
      routes.push_back(route);
      return;
    }
    *existing = route;
  }

  [[nodiscard]] std::uint32_t routeDirectionForMediaClass(std::string_view mediaClass) {
    if (mediaClass == "Audio/Source") {
      return SPA_DIRECTION_INPUT;
    }
    if (mediaClass == "Audio/Sink") {
      return SPA_DIRECTION_OUTPUT;
    }
    return 0;
  }

  constexpr Logger kLog("pipewire");

  bool isProgramStreamClass(std::string_view mediaClass) { return mediaClass == "Stream/Output/Audio"; }

  void logProgramStreamMetadata(std::string_view phase, std::uint32_t id, const PipeWireService::NodeData& nd) {
    if (!isProgramStreamClass(nd.mediaClass)) {
      return;
    }
    kLog.debug(
        "[program-stream] {} id={} clientId={} class='{}' appName='{}' appId='{}' appBinary='{}' streamTitle='{}' "
        "icon='{}' nodeName='{}' nodeDesc='{}'",
        phase, id, nd.clientId, nd.mediaClass, nd.applicationName, nd.applicationId, nd.applicationBinary,
        nd.streamTitle, nd.iconName, nd.name, nd.description
    );
  }

} // namespace

PipeWireService::PipeWireService() {
  pw_init(nullptr, nullptr);

  m_loop = pw_loop_new(nullptr);
  if (m_loop == nullptr) {
    throw std::runtime_error("pipewire: failed to create loop");
  }

  m_context = pw_context_new(m_loop, nullptr, 0);
  if (m_context == nullptr) {
    pw_loop_destroy(m_loop);
    throw std::runtime_error("pipewire: failed to create context");
  }

  m_core = pw_context_connect(m_context, nullptr, 0);
  if (m_core == nullptr) {
    pw_context_destroy(m_context);
    pw_loop_destroy(m_loop);
    throw std::runtime_error("pipewire: failed to connect to daemon");
  }

  m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
  if (m_registry == nullptr) {
    pw_core_disconnect(m_core);
    pw_context_destroy(m_context);
    pw_loop_destroy(m_loop);
    throw std::runtime_error("pipewire: failed to get registry");
  }

  m_registryListener = new spa_hook{};
  spa_zero(*m_registryListener);
  pw_registry_add_listener(m_registry, m_registryListener, &kRegistryEvents, this);

  // Do initial roundtrip to discover existing objects
  auto* loop = m_loop;
  pw_core_sync(m_core, PW_ID_CORE, 0);
  while (pw_loop_iterate(loop, 0) > 0) {
  }

  enumDefaultAudioDeviceParams();
  while (pw_loop_iterate(loop, 0) > 0) {
  }
  rebuildState();

  kLog.info("connected (version {})", pw_get_library_version());
  const auto* sink = defaultSink();
  if (sink != nullptr) {
    kLog.info("default sink \"{}\" vol={:.0f}%", sink->description, sink->volume * 100.0f);
  }
}

PipeWireService::~PipeWireService() {
  m_volumeThrottleTimer.stop();
  m_pendingNodeVolumes.clear();

  // Destroy node proxies and their listeners
  for (auto& [id, nd] : m_nodes) {
    if (nd->listener != nullptr) {
      spa_hook_remove(nd->listener);
      delete nd->listener;
    }
    if (nd->proxy != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(nd->proxy));
    }
  }
  m_nodes.clear();

  for (auto& [id, client] : m_clients) {
    if (client.listener != nullptr) {
      spa_hook_remove(client.listener);
      delete client.listener;
    }
    if (client.proxy != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(client.proxy));
    }
  }
  m_clients.clear();

  for (auto& [id, device] : m_devices) {
    if (device.listener != nullptr) {
      spa_hook_remove(device.listener);
      delete device.listener;
    }
    if (device.proxy != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(device.proxy));
    }
  }
  m_devices.clear();

  for (auto& cleanup : m_metadataCleanups) {
    cleanup();
  }
  m_metadataCleanups.clear();

  if (m_registryListener != nullptr) {
    spa_hook_remove(m_registryListener);
    delete m_registryListener;
  }

  if (m_registry != nullptr) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
  }
  if (m_core != nullptr) {
    pw_core_disconnect(m_core);
  }
  if (m_context != nullptr) {
    pw_context_destroy(m_context);
  }
  if (m_loop != nullptr) {
    pw_loop_destroy(m_loop);
  }

  pw_deinit();
}

int PipeWireService::fd() const noexcept {
  if (m_loop == nullptr) {
    return -1;
  }
  auto* loop = m_loop;
  return pw_loop_get_fd(loop);
}

void PipeWireService::dispatch() {
  if (m_loop == nullptr) {
    return;
  }
  auto* loop = m_loop;
  // Process all pending events without blocking
  while (pw_loop_iterate(loop, 0) > 0) {
  }
  if (m_pendingDefaultAudioDevicePropsEnum) {
    m_pendingDefaultAudioDevicePropsEnum = false;
    enumDefaultAudioDeviceParams();
    while (pw_loop_iterate(loop, 0) > 0) {
    }
  }
}

void PipeWireService::enumDefaultAudioDeviceParams() {
  for (auto& [id, nd] : m_nodes) {
    (void)id;
    if (nd == nullptr || nd->proxy == nullptr) {
      continue;
    }
    if (nd->mediaClass != "Audio/Sink" && nd->mediaClass != "Audio/Source") {
      continue;
    }
    pw_node_enum_params(nd->proxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    pw_node_enum_params(nd->proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
  }
}

const AudioNode* PipeWireService::defaultSink() const noexcept {
  for (const auto& sink : m_state.sinks) {
    if (sink.isDefault) {
      return &sink;
    }
  }
  return nullptr;
}

const AudioNode* PipeWireService::defaultSource() const noexcept {
  for (const auto& source : m_state.sources) {
    if (source.isDefault) {
      return &source;
    }
  }
  return nullptr;
}

void PipeWireService::onRegistryGlobal(std::uint32_t id, const char* type, std::uint32_t, const spa_dict* props) {
  if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
    ClientData client;
    client.service = this;
    client.id = id;
    applyClientPropsFromDict(client, props);
    auto [it, inserted] = m_clients.insert_or_assign(id, std::move(client));

    auto& stored = it->second;
    if (inserted) {
      auto* proxy = static_cast<pw_client*>(pw_registry_bind(m_registry, id, type, PW_VERSION_CLIENT, sizeof(void*)));
      if (proxy != nullptr) {
        stored.proxy = proxy;
        stored.listener = new spa_hook{};
        spa_zero(*stored.listener);
        pw_client_add_listener(proxy, stored.listener, &kClientEvents, &stored);
      }
    }

    for (auto& [_, node] : m_nodes) {
      if (node != nullptr) {
        refreshNodeIdentity(*node);
      }
    }
    // New client metadata can improve already-known stream node identity.
    rebuildState();
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
    DeviceData device;
    device.service = this;
    device.id = id;
    auto [it, inserted] = m_devices.insert_or_assign(id, std::move(device));

    auto& stored = it->second;
    if (inserted) {
      auto* proxy = static_cast<pw_device*>(pw_registry_bind(m_registry, id, type, PW_VERSION_DEVICE, sizeof(void*)));
      if (proxy != nullptr) {
        stored.proxy = proxy;
        stored.listener = new spa_hook{};
        spa_zero(*stored.listener);
        pw_device_add_listener(proxy, stored.listener, &kDeviceEvents, &stored);
        std::uint32_t params[] = {SPA_PARAM_Route};
        pw_device_subscribe_params(proxy, params, 1);
        pw_device_enum_params(proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
      }
    }
    return;
  }

  // Track audio sink/source nodes
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    std::string mediaClass = dictGet(props, PW_KEY_MEDIA_CLASS);
    if (mediaClass != "Audio/Sink" && mediaClass != "Audio/Source" && mediaClass != "Stream/Output/Audio") {
      return;
    }

    auto nd = std::make_unique<NodeData>();
    nd->service = this;
    nd->id = id;
    nd->name = dictGet(props, PW_KEY_NODE_NAME);
    nd->description = dictGet(props, PW_KEY_NODE_DESCRIPTION);
    if (nd->description.empty()) {
      nd->description = dictGet(props, PW_KEY_NODE_NICK);
    }
    if (nd->description.empty()) {
      nd->description = nd->name;
    }
    nd->clientId = parseUint32Or(dictGet(props, "client.id"));
    nd->deviceId = parseUint32Or(dictGet(props, "device.id"));
    nd->applicationName = dictGet(props, "application.name");
    if (nd->applicationName.empty()) {
      nd->applicationName = dictGet(props, "client.name");
    }
    nd->applicationId = dictGet(props, "application.id");
    if (nd->applicationId.ends_with(".desktop")) {
      nd->applicationId.erase(nd->applicationId.size() - std::string_view(".desktop").size());
    }
    nd->applicationBinary = dictGet(props, "application.process.binary");
    if (nd->applicationName.empty()) {
      nd->applicationName = nd->applicationBinary;
    }
    if (nd->applicationName.empty()) {
      nd->applicationName = nd->description;
    }

    nd->streamTitle = dictGet(props, "media.title");
    if (nd->streamTitle.empty()) {
      nd->streamTitle = dictGet(props, "media.name");
    }
    if (nd->streamTitle.empty()) {
      nd->streamTitle = dictGet(props, "node.nick");
    }
    if (nd->streamTitle.empty()) {
      nd->streamTitle = dictGet(props, PW_KEY_NODE_DESCRIPTION);
    }

    nd->iconName = dictGet(props, "application.icon-name");
    if (nd->iconName.empty()) {
      nd->iconName = dictGet(props, "node.icon-name");
    }
    if (nd->iconName.empty()) {
      nd->iconName = nd->applicationBinary;
    }
    nd->mediaClass = mediaClass;
    const bool audioDeviceNode = mediaClass == "Audio/Sink" || mediaClass == "Audio/Source";
    applyVolumePropsFromDict(*nd, props, !audioDeviceNode);
    refreshNodeIdentity(*nd);
    logProgramStreamMetadata("registry-global", id, *nd);

    // Bind to the node to receive param updates
    auto* proxy = static_cast<pw_node*>(pw_registry_bind(m_registry, id, type, PW_VERSION_NODE, sizeof(void*)));
    if (proxy != nullptr) {
      nd->proxy = proxy;
      nd->listener = new spa_hook{};
      spa_zero(*nd->listener);
      pw_node_add_listener(proxy, nd->listener, &kNodeEvents, nd.get());

      // Subscribe to Props param changes
      std::uint32_t params[] = {SPA_PARAM_Props, SPA_PARAM_Route};
      pw_node_subscribe_params(proxy, params, 2);
      // Fetch current props so initial UI state does not sit at 100%.
      pw_node_enum_params(proxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
      pw_node_enum_params(proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
    }

    m_nodes[id] = std::move(nd);
    NodeData& stored = *m_nodes[id];
    if (stored.mediaClass == "Audio/Sink" || stored.mediaClass == "Audio/Source") {
      m_pendingDefaultAudioDevicePropsEnum = true;
    }
    rebuildState();
  }

  // Track metadata for default sink/source names
  if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    std::string name = dictGet(props, PW_KEY_METADATA_NAME);
    if (name == "default") {
      auto* proxy =
          static_cast<pw_metadata*>(pw_registry_bind(m_registry, id, type, PW_VERSION_METADATA, sizeof(void*)));
      if (proxy != nullptr) {
        m_defaultMetadata = proxy;
        auto* md = new MetadataData{this, proxy, new spa_hook{}};
        spa_zero(*md->listener);
        pw_metadata_add_listener(proxy, md->listener, &kMetadataEvents, md);
        pw_core_sync(md->service->coreHandle(), PW_ID_CORE, 0);
        m_metadataCleanups.push_back([md]() {
          if (md->listener != nullptr) {
            spa_hook_remove(md->listener);
            delete md->listener;
          }
          if (md->proxy != nullptr) {
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(md->proxy));
          }
          if (md->service != nullptr && md->service->m_defaultMetadata == md->proxy) {
            md->service->m_defaultMetadata = nullptr;
          }
          delete md;
        });
      }
    }
  }
}

void PipeWireService::onRegistryGlobalRemove(std::uint32_t id) {
  if (auto it = m_clients.find(id); it != m_clients.end()) {
    if (it->second.listener != nullptr) {
      spa_hook_remove(it->second.listener);
      delete it->second.listener;
    }
    if (it->second.proxy != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second.proxy));
    }
    m_clients.erase(it);
    for (auto& [_, node] : m_nodes) {
      if (node != nullptr) {
        refreshNodeIdentity(*node);
      }
    }
    rebuildState();
    return;
  }

  if (auto it = m_devices.find(id); it != m_devices.end()) {
    if (it->second.listener != nullptr) {
      spa_hook_remove(it->second.listener);
      delete it->second.listener;
    }
    if (it->second.proxy != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second.proxy));
    }
    m_devices.erase(it);
    for (auto& [nid, node] : m_nodes) {
      if (node != nullptr && node->deviceId == id) {
        recomputeEffectiveMute(*node);
      }
    }
    rebuildState();
    return;
  }

  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return;
  }

  auto& nd = it->second;
  if (nd->listener != nullptr) {
    spa_hook_remove(nd->listener);
    delete nd->listener;
  }
  if (nd->proxy != nullptr) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(nd->proxy));
  }
  m_nodes.erase(it);
  rebuildState();
}

void PipeWireService::onNodeInfo(std::uint32_t id, const pw_node_info* info) {
  if (info == nullptr) {
    return;
  }

  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return;
  }

  // Update name/description from props if available
  if (info->props != nullptr) {
    std::string desc = dictGet(info->props, PW_KEY_NODE_DESCRIPTION);
    if (!desc.empty()) {
      it->second->description = desc;
    }
    std::string name = dictGet(info->props, PW_KEY_NODE_NAME);
    if (!name.empty()) {
      it->second->name = name;
    }
    std::string appName = dictGet(info->props, "application.name");
    if (appName.empty()) {
      appName = dictGet(info->props, "client.name");
    }
    if (!appName.empty()) {
      it->second->applicationName = appName;
    }
    std::string appId = dictGet(info->props, "application.id");
    if (appId.ends_with(".desktop")) {
      appId.erase(appId.size() - std::string_view(".desktop").size());
    }
    if (!appId.empty()) {
      it->second->applicationId = appId;
    }
    const std::uint32_t clientId = parseUint32Or(dictGet(info->props, "client.id"), it->second->clientId);
    if (clientId != 0) {
      it->second->clientId = clientId;
    }
    const std::uint32_t deviceId = parseUint32Or(dictGet(info->props, "device.id"), it->second->deviceId);
    if (deviceId != 0) {
      it->second->deviceId = deviceId;
    }
    std::string appBinary = dictGet(info->props, "application.process.binary");
    if (!appBinary.empty()) {
      it->second->applicationBinary = appBinary;
      if (it->second->applicationName.empty()) {
        it->second->applicationName = appBinary;
      }
    }
    std::string mediaName = dictGet(info->props, "media.title");
    if (mediaName.empty()) {
      mediaName = dictGet(info->props, "media.name");
    }
    if (!mediaName.empty()) {
      it->second->streamTitle = mediaName;
    }
    std::string iconName = dictGet(info->props, "application.icon-name");
    if (iconName.empty()) {
      iconName = dictGet(info->props, "node.icon-name");
    }
    if (!iconName.empty()) {
      it->second->iconName = iconName;
    }
    const bool audioDevice = it->second->mediaClass == "Audio/Sink" || it->second->mediaClass == "Audio/Source";
    applyVolumePropsFromDict(*it->second, info->props, !audioDevice);
    refreshNodeIdentity(*it->second);
    logProgramStreamMetadata("node-info", id, *it->second);
  }

  // Request Props param enumeration if changes flagged
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) != 0) {
    for (std::uint32_t i = 0; i < info->n_params; ++i) {
      if (info->params[i].id == SPA_PARAM_Props) {
        pw_node_enum_params(it->second->proxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
      } else if (info->params[i].id == SPA_PARAM_Route) {
        pw_node_enum_params(it->second->proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
      }
    }
  }
}

void PipeWireService::onNodeParam(
    std::uint32_t id, std::uint32_t paramId, std::uint32_t, std::uint32_t, const spa_pod* param
) {
  if ((paramId != SPA_PARAM_Props && paramId != SPA_PARAM_Route) || param == nullptr) {
    return;
  }

  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return;
  }

  auto& nd = *it->second;
  if (paramId == SPA_PARAM_Route) {
    std::int32_t routeIndex = -1;
    std::int32_t routeDevice = -1;
    std::uint32_t routeDirection = nd.routeDirection;
    std::int32_t routePriority = 0;
    const spa_pod* routeProps = nullptr;
    if (spa_pod_parse_object(
            param, SPA_TYPE_OBJECT_ParamRoute, nullptr, SPA_PARAM_ROUTE_index, SPA_POD_Int(&routeIndex),
            SPA_PARAM_ROUTE_direction, SPA_POD_Id(&routeDirection), SPA_PARAM_ROUTE_device, SPA_POD_Int(&routeDevice),
            SPA_PARAM_ROUTE_priority, SPA_POD_Int(&routePriority), SPA_PARAM_ROUTE_props, SPA_POD_Pod(&routeProps)
        ) >= 0) {
      const spa_pod_prop* availProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_available);
      std::uint32_t routeAvailable = SPA_PARAM_AVAILABILITY_unknown;
      if (availProp != nullptr) {
        spa_pod_get_id(&availProp->value, &routeAvailable);
      }

      DeviceRouteData route;
      route.index = routeIndex >= 0 ? routeIndex : -1;
      route.device = routeDevice;
      route.direction = routeDirection;
      route.priority = routePriority;
      route.available = routeAvailable;
      if (routeProps != nullptr) {
        spa_pod_prop* prop = nullptr;
        auto* propsObj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(routeProps));
        SPA_POD_OBJECT_FOREACH(propsObj, prop) {
          if (prop->key == SPA_PROP_mute) {
            bool routeMuted = false;
            if (spa_pod_get_bool(&prop->value, &routeMuted) == 0) {
              route.muted = routeMuted;
            }
          }
        }
      }
      upsertRoute(nd.routes, route);

      if (routeAvailable != SPA_PARAM_AVAILABILITY_no && routeProps != nullptr &&
          routeVolumeDirectionMatchesNode(nd.mediaClass, routeDirection)) {
        ParsedPropsVolumes basis{};
        basis.channelVol = nd.volume;
        basis.scalarVol = nd.volume;
        basis.softVol = nd.volume;
        basis.channelCount = nd.channelCount;
        ParsedPropsVolumes fromRoute{};
        parsePropsObjectVolumeFields(routeProps, basis, &fromRoute);
        mergeParsedVolumesIntoNode(nd, fromRoute);
      }
      recomputeEffectiveMute(nd);
      rebuildState();
    }
    return;
  }

  ParsedPropsVolumes basis{};
  basis.channelVol = nd.volume;
  basis.scalarVol = nd.volume;
  basis.softVol = nd.volume;
  basis.channelCount = nd.channelCount;
  ParsedPropsVolumes parsed{};
  parsePropsObjectVolumeFields(param, basis, &parsed);

  auto* obj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(param));
  spa_pod_prop* prop = nullptr;
  SPA_POD_OBJECT_FOREACH(obj, prop) {
    if (prop->key == SPA_PROP_mute) {
      bool swMuted = false;
      if (spa_pod_get_bool(&prop->value, &swMuted) == 0) {
        nd.swMute = swMuted;
      }
    }
  }

  float candidateVol = -1.0f;
  if (parsed.hasChannel) {
    candidateVol = parsed.channelVol;
  } else if (parsed.hasScalar) {
    candidateVol = parsed.scalarVol;
  } else if (parsed.hasSoft) {
    candidateVol = parsed.softVol;
  }
  const bool isAudioDeviceNode = nd.mediaClass == "Audio/Sink" || nd.mediaClass == "Audio/Source";
  const bool rejectStaleFullScaleProps =
      isAudioDeviceNode && candidateVol >= 0.0f && candidateVol >= 0.99f && nd.volume < 0.93f;

  if (!rejectStaleFullScaleProps) {
    mergeParsedVolumesIntoNode(nd, parsed);
  }

  recomputeEffectiveMute(nd);

  rebuildState();
}

void PipeWireService::onClientInfo(std::uint32_t id, const pw_client_info* info) {
  if (info == nullptr || info->props == nullptr) {
    return;
  }

  auto it = m_clients.find(id);
  if (it == m_clients.end()) {
    return;
  }

  if (!applyClientPropsFromDict(it->second, info->props)) {
    return;
  }

  for (auto& [_, node] : m_nodes) {
    if (node != nullptr) {
      refreshNodeIdentity(*node);
    }
  }
  rebuildState();
}

void PipeWireService::onDeviceInfo(std::uint32_t id, const pw_device_info* info) {
  if (info == nullptr) {
    return;
  }
  auto it = m_devices.find(id);
  if (it == m_devices.end() || it->second.proxy == nullptr) {
    return;
  }

  if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) != 0) {
    for (std::uint32_t i = 0; i < info->n_params; ++i) {
      if (info->params[i].id == SPA_PARAM_Route) {
        pw_device_enum_params(it->second.proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
      }
    }
  }
}

void PipeWireService::onDeviceParam(
    std::uint32_t id, std::uint32_t paramId, std::uint32_t index, std::uint32_t, const spa_pod* param
) {
  if (paramId != SPA_PARAM_Route || param == nullptr) {
    return;
  }

  auto it = m_devices.find(id);
  if (it == m_devices.end()) {
    return;
  }

  std::int32_t routeIndex = -1;
  std::int32_t routeDevice = -1;
  std::uint32_t routeDirection = 0;
  std::int32_t routePriority = 0;
  const spa_pod* routeProps = nullptr;
  if (spa_pod_parse_object(
          param, SPA_TYPE_OBJECT_ParamRoute, nullptr, SPA_PARAM_ROUTE_index, SPA_POD_Int(&routeIndex),
          SPA_PARAM_ROUTE_direction, SPA_POD_Id(&routeDirection), SPA_PARAM_ROUTE_device, SPA_POD_Int(&routeDevice),
          SPA_PARAM_ROUTE_priority, SPA_POD_Int(&routePriority), SPA_PARAM_ROUTE_props, SPA_POD_Pod(&routeProps)
      ) < 0) {
    return;
  }

  const spa_pod_prop* availProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_available);
  std::uint32_t routeAvailable = SPA_PARAM_AVAILABILITY_unknown;
  if (availProp != nullptr) {
    spa_pod_get_id(&availProp->value, &routeAvailable);
  }

  ParsedPropsVolumes fromRoute{};
  bool parsedRouteVolume = false;
  if (routeProps != nullptr && routeAvailable != SPA_PARAM_AVAILABILITY_no) {
    ParsedPropsVolumes basis{};
    basis.channelVol = 1.0f;
    basis.scalarVol = 1.0f;
    basis.softVol = 1.0f;
    basis.channelCount = 0;
    parsePropsObjectVolumeFields(routeProps, basis, &fromRoute);
    parsedRouteVolume = fromRoute.hasChannel || fromRoute.hasScalar || fromRoute.hasSoft;
  }

  bool muted = false;
  if (routeProps != nullptr) {
    spa_pod_prop* prop = nullptr;
    auto* propsObj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(routeProps));
    SPA_POD_OBJECT_FOREACH(propsObj, prop) {
      if (prop->key == SPA_PROP_mute) {
        bool routeMuted = false;
        if (spa_pod_get_bool(&prop->value, &routeMuted) == 0) {
          muted = routeMuted;
        }
      }
    }
  }

  DeviceRouteData route;
  route.index = routeIndex >= 0 ? routeIndex : static_cast<std::int32_t>(index);
  route.device = routeDevice;
  route.direction = routeDirection;
  route.priority = routePriority;
  route.available = routeAvailable;
  route.muted = muted;
  upsertRoute(it->second.routes, route);

  if (parsedRouteVolume) {
    for (auto& [nid, node] : m_nodes) {
      (void)nid;
      if (node != nullptr && node->deviceId == id &&
          routeVolumeDirectionMatchesNode(node->mediaClass, routeDirection)) {
        mergeParsedVolumesIntoNode(*node, fromRoute);
      }
    }
  }

  for (auto& [nid, node] : m_nodes) {
    if (node != nullptr && node->deviceId == id) {
      recomputeEffectiveMute(*node);
    }
  }
  rebuildState();
}

void PipeWireService::parseDefaultNodes(const spa_dict* props) {
  std::string sinkName = dictGet(props, "default.audio.sink");
  std::string sourceName = dictGet(props, "default.audio.source");

  bool changed = false;
  if (!sinkName.empty() && sinkName != m_defaultSinkName) {
    m_defaultSinkName = sinkName;
    changed = true;
  }
  if (!sourceName.empty() && sourceName != m_defaultSourceName) {
    m_defaultSourceName = sourceName;
    changed = true;
  }

  if (changed) {
    m_pendingDefaultAudioDevicePropsEnum = true;
    rebuildState();
  }
}

void PipeWireService::refreshNodeIdentity(NodeData& nd) {
  const auto it = m_clients.find(nd.clientId);
  if (it == m_clients.end()) {
    return;
  }
  const ClientData& client = it->second;
  if ((nd.applicationName.empty() || nd.applicationName == "audio-src" || nd.applicationName == "audio-sink" ||
       nd.applicationName == "audio-source") &&
      !client.name.empty()) {
    nd.applicationName = client.name;
  }
  if ((nd.applicationId.empty() || nd.applicationId == "audio-src") && !client.appId.empty()) {
    nd.applicationId = client.appId;
  }
  if ((nd.applicationBinary.empty() || nd.applicationBinary == "audio-src") && !client.binary.empty()) {
    nd.applicationBinary = client.binary;
  }
  if (nd.iconName.empty() && !client.iconName.empty()) {
    nd.iconName = client.iconName;
  }
}

void PipeWireService::rebuildState() {
  AudioState next;

  for (const auto& [id, nd] : m_nodes) {
    AudioNode node;
    node.id = id;
    node.name = nd->name;
    node.description = nd->description;
    node.applicationName = nd->applicationName;
    node.applicationId = nd->applicationId;
    node.applicationBinary = nd->applicationBinary;
    node.streamTitle = nd->streamTitle;
    node.iconName = nd->iconName;
    node.mediaClass = nd->mediaClass;
    node.volume = nd->volume;
    node.muted = nd->muted;
    node.channelCount = nd->channelCount;

    if (nd->mediaClass == "Audio/Sink") {
      node.isDefault = (nd->name == m_defaultSinkName);
      if (node.isDefault) {
        next.defaultSinkId = id;
      }
      next.sinks.push_back(std::move(node));
    } else if (nd->mediaClass == "Audio/Source") {
      node.isDefault = (nd->name == m_defaultSourceName);
      if (node.isDefault) {
        next.defaultSourceId = id;
      }
      next.sources.push_back(std::move(node));
    } else if (nd->mediaClass == "Stream/Output/Audio") {
      next.programOutputs.push_back(std::move(node));
    }
  }

  // Sort by id for stable ordering
  std::ranges::sort(next.sinks, [](const auto& a, const auto& b) { return a.id < b.id; });
  std::ranges::sort(next.sources, [](const auto& a, const auto& b) { return a.id < b.id; });
  std::ranges::sort(next.programOutputs, [](const auto& a, const auto& b) { return a.id < b.id; });

  if (next == m_state) {
    return;
  }

  m_state = std::move(next);
  ++m_changeSerial;
  emitChanged();
}

void PipeWireService::recomputeEffectiveMute(NodeData& nd) {
  const std::uint32_t wantDir = routeDirectionForMediaClass(nd.mediaClass);
  const DeviceRouteData* nodeRoute = wantDir != 0 ? activeRouteForDirection(nd.routes, wantDir) : nullptr;
  const DeviceRouteData* deviceRoute = nullptr;
  if (nd.deviceId != 0 && wantDir != 0) {
    const auto it = m_devices.find(nd.deviceId);
    if (it != m_devices.end()) {
      deviceRoute = activeRouteForDirection(it->second.routes, wantDir);
    }
  }

  bool routeMuted = false;
  if (nodeRoute != nullptr) {
    nd.hasRoute = true;
    nd.routeIndex = nodeRoute->index;
    nd.routeDevice = nodeRoute->device;
    nd.routeDirection = nodeRoute->direction;
    nd.nodeRouteMute = nodeRoute->muted;
    routeMuted = nodeRoute->muted;
  } else {
    nd.hasRoute = false;
    nd.routeIndex = -1;
    nd.routeDevice = -1;
    nd.nodeRouteMute = false;
  }

  const bool deviceRouteMuted = deviceRoute != nullptr && deviceRoute->muted;
  nd.muted = nd.swMute || routeMuted || deviceRouteMuted;
}

void PipeWireService::applyVolumePropsFromDict(NodeData& nd, const spa_dict* props, bool applyMixerFieldsFromDict) {
  if (props == nullptr) {
    return;
  }

  if (applyMixerFieldsFromDict) {
    if (const auto maybeChannelmixVolume = parseFloat(dictGet(props, "channelmix.volume"));
        maybeChannelmixVolume.has_value()) {
      nd.volume = std::clamp(*maybeChannelmixVolume, 0.0f, 1.5f);
    } else if (const auto maybeVolume = parseFloat(dictGet(props, "volume")); maybeVolume.has_value()) {
      nd.volume = std::clamp(*maybeVolume, 0.0f, 1.5f);
    }

    if (const auto maybeChannelmixMuted = parseBool(dictGet(props, "channelmix.mute"));
        maybeChannelmixMuted.has_value()) {
      nd.swMute = *maybeChannelmixMuted;
    } else if (const auto maybeMuted = parseBool(dictGet(props, "mute")); maybeMuted.has_value()) {
      nd.swMute = *maybeMuted;
    }
  }

  recomputeEffectiveMute(nd);
}

void PipeWireService::scheduleVolumeFlush() {
  const auto now = std::chrono::steady_clock::now();
  const auto earliest = m_lastVolumeFlushValid ? (m_lastVolumeFlushAt + kVolumeApplyMinInterval)
                                               : std::chrono::steady_clock::time_point{};

  if (!m_lastVolumeFlushValid || now >= earliest) {
    m_volumeThrottleTimer.stop();
    flushPendingNodeVolumes();
    m_lastVolumeFlushAt = std::chrono::steady_clock::now();
    m_lastVolumeFlushValid = true;
    return;
  }

  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
  const auto wait = std::max(delay, std::chrono::milliseconds{1});
  m_volumeThrottleTimer.start(wait, [this]() {
    flushPendingNodeVolumes();
    m_lastVolumeFlushAt = std::chrono::steady_clock::now();
  });
}

void PipeWireService::flushPendingNodeVolumes() {
  if (m_pendingNodeVolumes.empty()) {
    return;
  }

  bool dirty = false;
  auto pending = std::move(m_pendingNodeVolumes);

  for (const auto& [id, volume] : pending) {
    if (!applyNodeVolumeImmediate(id, volume)) {
      continue;
    }
    dirty = true;
    if (id == m_state.defaultSinkId && m_state.defaultSinkId != 0) {
      emitVolumePreview(false, id, volume);
    } else if (id == m_state.defaultSourceId && m_state.defaultSourceId != 0) {
      emitVolumePreview(true, id, volume);
    }
  }

  if (dirty) {
    rebuildState();
  }
}

bool PipeWireService::applyNodeVolumeImmediate(std::uint32_t id, float volume) {
  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return false;
  }

  auto& nd = *it->second;
  if (nd.proxy == nullptr) {
    return false;
  }

  volume = std::clamp(volume, 0.0f, 1.5f);

  // Keep WirePlumber policy in sync without blocking the main loop.
  // `runAsync` is fire-and-forget, so rapid wheel/slider updates remain responsive.
  const bool isDeviceNode = nd.mediaClass == "Audio/Sink" || nd.mediaClass == "Audio/Source";
  if (isDeviceNode) {
    const bool launched = process::runAsync({"wpctl", "set-volume", std::to_string(id), std::format("{:.4f}", volume)});
    if (launched) {
      // For devices, keep policy changes in WirePlumber path (pavu/wpctl-visible).
      if (std::abs(nd.volume - volume) >= 0.0001f) {
        nd.volume = volume;
        return true;
      }
      return false;
    }
  }

  // Convert linear volume to cubic (PipeWire native)
  float cubic = volume * volume * volume;

  std::uint32_t nChannels = nd.channelCount > 0 ? nd.channelCount : 2;
  std::vector<float> volumes(nChannels, cubic);

  std::uint8_t buffer[1024];
  spa_pod_builder builder;
  spa_pod_builder_init(&builder, buffer, sizeof(buffer));

  spa_pod_frame frame;
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
  spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
  spa_pod_builder_array(&builder, sizeof(float), SPA_TYPE_Float, nChannels, volumes.data());
  auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));

  pw_node_set_param(nd.proxy, SPA_PARAM_Props, 0, pod);

  // Apply optimistic local state while PipeWire publishes props.
  if (std::abs(nd.volume - volume) >= 0.0001f) {
    nd.volume = volume;
    return true;
  }
  return false;
}

void PipeWireService::setNodeVolume(std::uint32_t id, float volume) {
  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return;
  }

  if (it->second->proxy == nullptr) {
    return;
  }

  m_pendingNodeVolumes[id] = std::clamp(volume, 0.0f, 1.5f);
  scheduleVolumeFlush();
}

void PipeWireService::setNodeMuted(std::uint32_t id, bool muted) {
  auto it = m_nodes.find(id);
  if (it == m_nodes.end()) {
    return;
  }

  auto& nd = *it->second;
  if (nd.proxy == nullptr) {
    return;
  }

  // Keep WirePlumber policy in sync, but do not block the UI thread.
  const bool isDeviceNode = nd.mediaClass == "Audio/Sink" || nd.mediaClass == "Audio/Source";
  if (isDeviceNode) {
    const bool launched = process::runAsync({"wpctl", "set-mute", std::to_string(id), muted ? "1" : "0"});
    if (launched) {
      const bool before = nd.muted;
      nd.swMute = muted;
      recomputeEffectiveMute(nd);
      if (before != nd.muted) {
        if (id == m_state.defaultSinkId && m_state.defaultSinkId != 0) {
          emitVolumePreview(false, id, nd.volume);
        } else if (id == m_state.defaultSourceId && m_state.defaultSourceId != 0) {
          emitVolumePreview(true, id, nd.volume);
        }
        rebuildState();
      }
      return;
    }
  }

  // Program streams, and device nodes for immediate local/UI consistency.
  if (nd.hasRoute && nd.routeIndex >= 0) {
    std::uint8_t routeBuffer[512];
    spa_pod_builder routeBuilder;
    spa_pod_builder_init(&routeBuilder, routeBuffer, sizeof(routeBuffer));

    spa_pod_frame routeFrame;
    spa_pod_builder_push_object(&routeBuilder, &routeFrame, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_prop(&routeBuilder, SPA_PARAM_ROUTE_index, 0);
    spa_pod_builder_int(&routeBuilder, nd.routeIndex);
    spa_pod_builder_prop(&routeBuilder, SPA_PARAM_ROUTE_direction, 0);
    spa_pod_builder_id(&routeBuilder, nd.routeDirection);
    spa_pod_builder_prop(&routeBuilder, SPA_PARAM_ROUTE_device, 0);
    spa_pod_builder_int(&routeBuilder, nd.routeDevice);
    spa_pod_builder_prop(&routeBuilder, SPA_PARAM_ROUTE_props, 0);
    spa_pod_frame routePropsFrame;
    spa_pod_builder_push_object(&routeBuilder, &routePropsFrame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&routeBuilder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&routeBuilder, muted);
    spa_pod_builder_pop(&routeBuilder, &routePropsFrame);
    spa_pod_builder_prop(&routeBuilder, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool(&routeBuilder, true);
    auto* routePod = static_cast<spa_pod*>(spa_pod_builder_pop(&routeBuilder, &routeFrame));
    pw_node_set_param(nd.proxy, SPA_PARAM_Route, 0, routePod);
  }

  std::uint8_t buffer[256];
  spa_pod_builder builder;
  spa_pod_builder_init(&builder, buffer, sizeof(buffer));

  spa_pod_frame frame;
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
  spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
  spa_pod_builder_bool(&builder, muted);
  auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));

  pw_node_set_param(nd.proxy, SPA_PARAM_Props, 0, pod);

  const bool before = nd.muted;
  nd.swMute = muted;
  if (nd.hasRoute && nd.routeIndex >= 0) {
    nd.nodeRouteMute = muted;
  }
  recomputeEffectiveMute(nd);
  if (before != nd.muted) {
    if (id == m_state.defaultSinkId && m_state.defaultSinkId != 0) {
      emitVolumePreview(false, id, nd.volume);
    } else if (id == m_state.defaultSourceId && m_state.defaultSourceId != 0) {
      emitVolumePreview(true, id, nd.volume);
    }
    rebuildState();
  }
}

void PipeWireService::setSinkVolume(std::uint32_t id, float volume) {
  setNodeVolume(id, volume);
  if (id == m_state.defaultSinkId && m_state.defaultSinkId != 0) {
    emitVolumePreview(false, id, volume);
  }
}
void PipeWireService::setSinkMuted(std::uint32_t id, bool muted) { setNodeMuted(id, muted); }
void PipeWireService::setDefaultSink(std::uint32_t id) { setDefaultNode(id, "default.audio.sink"); }
void PipeWireService::setSourceVolume(std::uint32_t id, float volume) {
  setNodeVolume(id, volume);
  if (id == m_state.defaultSourceId && m_state.defaultSourceId != 0) {
    emitVolumePreview(true, id, volume);
  }
}
void PipeWireService::setSourceMuted(std::uint32_t id, bool muted) { setNodeMuted(id, muted); }
void PipeWireService::setDefaultSource(std::uint32_t id) { setDefaultNode(id, "default.audio.source"); }

void PipeWireService::setProgramOutputVolume(std::uint32_t id, float volume) { setNodeVolume(id, volume); }
void PipeWireService::setProgramOutputMuted(std::uint32_t id, bool muted) { setNodeMuted(id, muted); }

void PipeWireService::setDefaultNode(std::uint32_t id, const char* key) {
  const auto it = m_nodes.find(id);
  if (it == m_nodes.end() || key == nullptr) {
    return;
  }

  // Prefer wpctl so WirePlumber persists the default. Metadata API alone often does not survive reboot.
  if (process::runSync({"wpctl", "set-default", std::to_string(id)})) {
    if (std::strcmp(key, "default.audio.sink") == 0) {
      m_defaultSinkName = it->second->name;
    } else if (std::strcmp(key, "default.audio.source") == 0) {
      m_defaultSourceName = it->second->name;
    }
    rebuildState();
    return;
  }

  if (m_defaultMetadata == nullptr) {
    kLog.warn("unable to set {} - default metadata unavailable", key);
    return;
  }

  const std::string payload = "{\"name\":\"" + escapeJsonString(it->second->name) + "\"}";
  const int rc = pw_metadata_set_property(m_defaultMetadata, PW_ID_CORE, key, "Spa:String:JSON", payload.c_str());
  if (rc < 0) {
    kLog.warn("failed to set {} to \"{}\" ({})", key, it->second->name, spa_strerror(rc));
    return;
  }

  if (std::strcmp(key, "default.audio.sink") == 0) {
    m_defaultSinkName = it->second->name;
  } else if (std::strcmp(key, "default.audio.source") == 0) {
    m_defaultSourceName = it->second->name;
  }
  rebuildState();
}

void PipeWireService::setVolume(float volume) {
  const auto* sink = defaultSink();
  if (sink == nullptr) {
    return;
  }
  volume = std::clamp(volume, 0.0f, 1.5f);
  setNodeVolume(sink->id, volume);
  emitVolumePreview(false, sink->id, volume);
}

void PipeWireService::setMuted(bool muted) {
  const auto* sink = defaultSink();
  if (sink == nullptr) {
    return;
  }
  setNodeMuted(sink->id, muted);
  emitVolumePreview(false, sink->id, sink->volume);
}

void PipeWireService::setMicVolume(float volume) {
  const auto* source = defaultSource();
  if (source == nullptr) {
    return;
  }
  volume = std::clamp(volume, 0.0f, 1.5f);
  setNodeVolume(source->id, volume);
  emitVolumePreview(true, source->id, volume);
}

void PipeWireService::setMicMuted(bool muted) {
  const auto* source = defaultSource();
  if (source == nullptr) {
    return;
  }
  setNodeMuted(source->id, muted);
  emitVolumePreview(true, source->id, source->volume);
}

void PipeWireService::emitVolumePreview(bool isInput, std::uint32_t id, float volume) const {
  if (!m_volumePreviewCallback) {
    return;
  }
  const auto it = m_nodes.find(id);
  const bool muted = (it != m_nodes.end()) ? it->second->muted : false;
  m_volumePreviewCallback(isInput, id, std::clamp(volume, 0.0f, 1.5f), muted);
}

void PipeWireService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void PipeWireService::registerIpc(IpcService& ipc, const ConfigService& config) {
  const auto maxVolume = [&config] { return config.config().audio.enableOverdrive ? 1.5f : 1.0f; };
  const auto parseVolumeValueError =
      "error: invalid volume value (use percent like 65 or 65%, or normalized like 0.65)\n";
  const auto parseVolumeStepError = "error: invalid volume step (use percent like 5 or 5%, or normalized like 0.05)\n";

  ipc.registerHandler(
      "volume-set",
      [this, maxVolume, parseVolumeValueError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: volume-set requires <value>\n";
        }
        const auto* sink = defaultSink();
        if (!sink)
          return "error: no default output\n";

        const auto amount = noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!amount.has_value()) {
          return parseVolumeValueError;
        }

        setVolume(std::clamp(*amount, 0.0f, maxVolume()));
        return "ok\n";
      },
      "volume-set <value>", "Set speaker volume"
  );

  ipc.registerHandler(
      "volume-up",
      [this, maxVolume, parseVolumeStepError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: volume-up accepts at most one optional [step]\n";
        }
        const auto* sink = defaultSink();
        if (!sink)
          return "error: no default output\n";

        const auto step = parts.empty() ? std::optional<float>(kDefaultVolumeStep)
                                        : noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!step.has_value()) {
          return parseVolumeStepError;
        }

        setVolume(std::clamp(sink->volume + *step, 0.0f, maxVolume()));
        return "ok\n";
      },
      "volume-up [step]", "Increase speaker volume"
  );

  ipc.registerHandler(
      "volume-down",
      [this, maxVolume, parseVolumeStepError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: volume-down accepts at most one optional [step]\n";
        }
        const auto* sink = defaultSink();
        if (!sink)
          return "error: no default output\n";

        const auto step = parts.empty() ? std::optional<float>(kDefaultVolumeStep)
                                        : noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!step.has_value()) {
          return parseVolumeStepError;
        }

        setVolume(std::clamp(sink->volume - *step, 0.0f, maxVolume()));
        return "ok\n";
      },
      "volume-down [step]", "Decrease speaker volume"
  );

  ipc.registerHandler(
      "volume-mute",
      [this](const std::string&) -> std::string {
        const auto* sink = defaultSink();
        if (!sink)
          return "error: no default output\n";
        setMuted(!sink->muted);
        return "ok\n";
      },
      "volume-mute", "Toggle speaker mute"
  );

  ipc.registerHandler(
      "mic-volume-set",
      [this, maxVolume, parseVolumeValueError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: mic-volume-set requires <value>\n";
        }
        const auto* source = defaultSource();
        if (!source)
          return "error: no default input\n";

        const auto amount = noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!amount.has_value()) {
          return parseVolumeValueError;
        }

        setMicVolume(std::clamp(*amount, 0.0f, maxVolume()));
        return "ok\n";
      },
      "mic-volume-set <value>", "Set microphone volume"
  );

  ipc.registerHandler(
      "mic-volume-up",
      [this, maxVolume, parseVolumeStepError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: mic-volume-up accepts at most one optional [step]\n";
        }
        const auto* source = defaultSource();
        if (!source)
          return "error: no default input\n";

        const auto step = parts.empty() ? std::optional<float>(kDefaultVolumeStep)
                                        : noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!step.has_value()) {
          return parseVolumeStepError;
        }

        setMicVolume(std::clamp(source->volume + *step, 0.0f, maxVolume()));
        return "ok\n";
      },
      "mic-volume-up [step]", "Increase microphone volume"
  );

  ipc.registerHandler(
      "mic-volume-down",
      [this, maxVolume, parseVolumeStepError](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: mic-volume-down accepts at most one optional [step]\n";
        }
        const auto* source = defaultSource();
        if (!source)
          return "error: no default input\n";

        const auto step = parts.empty() ? std::optional<float>(kDefaultVolumeStep)
                                        : noctalia::ipc::parseNormalizedOrPercent(parts[0], maxVolume() * 100.0f);
        if (!step.has_value()) {
          return parseVolumeStepError;
        }

        setMicVolume(std::clamp(source->volume - *step, 0.0f, maxVolume()));
        return "ok\n";
      },
      "mic-volume-down [step]", "Decrease microphone volume"
  );

  ipc.registerHandler(
      "mic-mute",
      [this](const std::string&) -> std::string {
        const auto* source = defaultSource();
        if (!source)
          return "error: no default input\n";
        setMicMuted(!source->muted);
        return "ok\n";
      },
      "mic-mute", "Toggle microphone mute"
  );
}
