#include "shell/wayland/output_management_globals.h"

#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>

namespace {

  constexpr std::uint32_t kOutputManagerVersion = 4;
  constexpr std::uint32_t kOutputManagerMinVersion = 3;

  OutputManagementGlobals& self(void* data) { return *static_cast<OutputManagementGlobals*>(data); }

  const zwlr_output_mode_v1_listener kModeListener = {
      .size = [](void*, zwlr_output_mode_v1*, int32_t, int32_t) {},
      .refresh = [](void*, zwlr_output_mode_v1*, int32_t) {},
      .preferred = [](void*, zwlr_output_mode_v1*) {},
      .finished = [](void* data, zwlr_output_mode_v1* mode) { self(data).onModeFinished(mode); },
  };

  // libwayland aborts on an event with a null listener slot, even one we don't need.
  const zwlr_output_head_v1_listener kHeadListener = {
      .name = [](void* data, zwlr_output_head_v1* head, const char* name) { self(data).onHeadName(head, name); },
      .description = [](void*, zwlr_output_head_v1*, const char*) {},
      .physical_size = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
      .mode = [](void* data, zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* mode) { self(data).onHeadMode(mode); },
      .enabled = [](void*, zwlr_output_head_v1*, int32_t) {},
      .current_mode = [](void*, zwlr_output_head_v1*, zwlr_output_mode_v1*) {},
      .position = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
      .transform = [](void* data, zwlr_output_head_v1* head,
                      int32_t transform) { self(data).onHeadTransform(head, transform); },
      .scale = [](void* data, zwlr_output_head_v1* head,
                  wl_fixed_t scale) { self(data).onHeadScale(head, wl_fixed_to_double(scale)); },
      .finished = [](void* data, zwlr_output_head_v1* head) { self(data).onHeadFinished(head); },
      .make = [](void* data, zwlr_output_head_v1* head, const char* make) { self(data).onHeadMake(head, make); },
      .model = [](void* data, zwlr_output_head_v1* head, const char* model) { self(data).onHeadModel(head, model); },
      .serial_number = [](void* data, zwlr_output_head_v1* head,
                          const char* serialNumber) { self(data).onHeadSerialNumber(head, serialNumber); },
      .adaptive_sync = [](void*, zwlr_output_head_v1*, uint32_t) {},
  };

  const zwlr_output_manager_v1_listener kManagerListener = {
      .head =
          [](void* data, zwlr_output_manager_v1* /*manager*/, zwlr_output_head_v1* head) {
            self(data).onHead(head);
            zwlr_output_head_v1_add_listener(head, &kHeadListener, data);
          },
      .done = [](void* data, zwlr_output_manager_v1* /*manager*/, std::uint32_t /*serial*/) { self(data).onDone(); },
      .finished = [](void* data, zwlr_output_manager_v1* /*manager*/) { self(data).onFinished(); },
  };

} // namespace

OutputManagementGlobals::OutputManagementGlobals(WaylandConnection& connection) : m_connection(connection) {}

void OutputManagementGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  // head/mode release requests need v3; nothing useful to bind below that anyway.
  if (interface != zwlr_output_manager_v1_interface.name || version < kOutputManagerMinVersion) {
    return;
  }
  const auto bindVersion = std::min(version, kOutputManagerVersion);
  m_manager = static_cast<zwlr_output_manager_v1*>(
      wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, bindVersion)
  );
  zwlr_output_manager_v1_add_listener(m_manager, &kManagerListener, this);
  m_connection.setOutputMetadataSource([this] { return std::span<const WaylandOutputMetadata>(m_metadata); });
}

void OutputManagementGlobals::releaseGlobals() {
  releaseHeadsAndModes();
  if (m_manager != nullptr) {
    zwlr_output_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
  m_connection.setOutputMetadataSource({});
}

void OutputManagementGlobals::onHead(zwlr_output_head_v1* head) {
  if (std::ranges::contains(m_heads, head)) {
    return;
  }
  m_heads.push_back(head);
  m_metadata.emplace_back();
}

void OutputManagementGlobals::onDone() { m_connection.applyOutputMetadata(); }

void OutputManagementGlobals::onFinished() {
  releaseHeadsAndModes();
  zwlr_output_manager_v1_destroy(m_manager);
  m_manager = nullptr;
}

void OutputManagementGlobals::onHeadName(zwlr_output_head_v1* head, const char* name) {
  if (auto* metadata = metadataFor(head); metadata != nullptr && name != nullptr) {
    metadata->connectorName = name;
  }
}

void OutputManagementGlobals::onHeadMake(zwlr_output_head_v1* head, const char* make) {
  if (auto* metadata = metadataFor(head); metadata != nullptr && make != nullptr) {
    metadata->make = make;
  }
}

void OutputManagementGlobals::onHeadModel(zwlr_output_head_v1* head, const char* model) {
  if (auto* metadata = metadataFor(head); metadata != nullptr && model != nullptr) {
    metadata->model = model;
  }
}

void OutputManagementGlobals::onHeadSerialNumber(zwlr_output_head_v1* head, const char* serialNumber) {
  if (auto* metadata = metadataFor(head); metadata != nullptr && serialNumber != nullptr) {
    metadata->serialNumber = serialNumber;
  }
}

void OutputManagementGlobals::onHeadMode(zwlr_output_mode_v1* mode) {
  if (mode == nullptr) {
    return;
  }
  m_modes.insert(mode);
  zwlr_output_mode_v1_add_listener(mode, &kModeListener, this);
}

void OutputManagementGlobals::onHeadScale(zwlr_output_head_v1* head, double scaleFactor) {
  if (auto* metadata = metadataFor(head); metadata != nullptr) {
    metadata->scaleFactor = scaleFactor;
  }
}

void OutputManagementGlobals::onHeadTransform(zwlr_output_head_v1* head, std::int32_t transform) {
  if (auto* metadata = metadataFor(head); metadata != nullptr) {
    metadata->transform = transform;
  }
}

void OutputManagementGlobals::onHeadFinished(zwlr_output_head_v1* head) {
  if (const auto it = std::ranges::find(m_heads, head); it != m_heads.end()) {
    const auto index = std::distance(m_heads.begin(), it);
    m_heads.erase(it);
    m_metadata.erase(m_metadata.begin() + index);
  }
  zwlr_output_head_v1_release(head);
}

void OutputManagementGlobals::onModeFinished(zwlr_output_mode_v1* mode) {
  if (m_modes.erase(mode) > 0) {
    zwlr_output_mode_v1_release(mode);
  }
}

WaylandOutputMetadata* OutputManagementGlobals::metadataFor(zwlr_output_head_v1* head) {
  const auto it = std::ranges::find(m_heads, head);
  if (it == m_heads.end()) {
    return nullptr;
  }
  return &m_metadata[static_cast<std::size_t>(std::distance(m_heads.begin(), it))];
}

void OutputManagementGlobals::releaseHeadsAndModes() {
  for (auto* mode : m_modes) {
    zwlr_output_mode_v1_release(mode);
  }
  m_modes.clear();
  for (auto* head : m_heads) {
    zwlr_output_head_v1_release(head);
  }
  m_heads.clear();
  m_metadata.clear();
}
