#include "compositors/mango/mango_keyboard_backend.h"

#include "core/process.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "wayland-client-core.h"
#include "wayland-client-protocol.h"

#include <algorithm>

namespace {

  constexpr auto kSyncTtl = std::chrono::milliseconds(75);

  void
  registryGlobal(void* data, wl_registry* /*registry*/, uint32_t name, const char* interfaceName, uint32_t version) {
    static_cast<MangoKeyboardBackend*>(data)->onRegistryGlobal(name, interfaceName, version);
  }

  void registryGlobalRemove(void* data, wl_registry* /*registry*/, uint32_t name) {
    static_cast<MangoKeyboardBackend*>(data)->onRegistryGlobalRemove(name);
  }

  const wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void managerTags(void* /*data*/, zdwl_ipc_manager_v2* /*manager*/, uint32_t /*amount*/) {}
  void managerLayout(void* /*data*/, zdwl_ipc_manager_v2* /*manager*/, const char* /*name*/) {}

  const zdwl_ipc_manager_v2_listener kManagerListener = {
      .tags = managerTags,
      .layout = managerLayout,
  };

  void outputToggleVisibility(void* /*data*/, zdwl_ipc_output_v2* /*output*/) {}

  void outputActive(void* data, zdwl_ipc_output_v2* output, uint32_t active) {
    static_cast<MangoKeyboardBackend*>(data)->onOutputActive(output, active);
  }

  void outputTag(
      void* /*data*/, zdwl_ipc_output_v2* /*output*/, uint32_t /*tag*/, uint32_t /*state*/, uint32_t /*clients*/,
      uint32_t /*focused*/
  ) {}

  void outputLayout(void* /*data*/, zdwl_ipc_output_v2* /*output*/, uint32_t /*layout*/) {}
  void outputTitle(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*title*/) {}
  void outputAppId(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*appid*/) {}
  void outputLayoutSymbol(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*layout*/) {}

  void outputFrame(void* data, zdwl_ipc_output_v2* output) {
    static_cast<MangoKeyboardBackend*>(data)->onOutputFrame(output);
  }

  void outputFullscreen(void* /*data*/, zdwl_ipc_output_v2* /*output*/, uint32_t /*state*/) {}
  void outputFloating(void* /*data*/, zdwl_ipc_output_v2* /*output*/, uint32_t /*state*/) {}
  void outputX(void* /*data*/, zdwl_ipc_output_v2* /*output*/, int32_t /*x*/) {}
  void outputY(void* /*data*/, zdwl_ipc_output_v2* /*output*/, int32_t /*y*/) {}
  void outputWidth(void* /*data*/, zdwl_ipc_output_v2* /*output*/, int32_t /*width*/) {}
  void outputHeight(void* /*data*/, zdwl_ipc_output_v2* /*output*/, int32_t /*height*/) {}
  void outputLastLayer(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*layer*/) {}

  void outputKbLayout(void* data, zdwl_ipc_output_v2* output, const char* layout) {
    static_cast<MangoKeyboardBackend*>(data)->onOutputKeyboardLayout(output, layout);
  }

  void outputKeymode(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*keymode*/) {}
  void outputScaleFactor(void* /*data*/, zdwl_ipc_output_v2* /*output*/, uint32_t /*scale*/) {}

  const zdwl_ipc_output_v2_listener kOutputListener = {
      .toggle_visibility = outputToggleVisibility,
      .active = outputActive,
      .tag = outputTag,
      .layout = outputLayout,
      .title = outputTitle,
      .appid = outputAppId,
      .layout_symbol = outputLayoutSymbol,
      .frame = outputFrame,
      .fullscreen = outputFullscreen,
      .floating = outputFloating,
      .x = outputX,
      .y = outputY,
      .width = outputWidth,
      .height = outputHeight,
      .last_layer = outputLastLayer,
      .kb_layout = outputKbLayout,
      .keymode = outputKeymode,
      .scalefactor = outputScaleFactor,
  };

} // namespace

MangoKeyboardBackend::MangoKeyboardBackend() = default;

MangoKeyboardBackend::~MangoKeyboardBackend() { cleanup(); }

bool MangoKeyboardBackend::isAvailable() const noexcept { return syncState() && preferredOutputState() != nullptr; }

bool MangoKeyboardBackend::cycleLayout() const {
  if (process::commandExists("mmsg")) {
    const bool ok = process::runSync({"mmsg", "-s", "-d", "switch_keyboard_layout"});
    if (ok) {
      invalidateCachedState();
    }
    return ok;
  }

  if (!syncState()) {
    return false;
  }

  const auto* state = preferredOutputState();
  if (state == nullptr || state->handle == nullptr || zdwl_ipc_output_v2_get_version(state->handle) < 2) {
    return false;
  }

  zdwl_ipc_output_v2_dispatch(state->handle, "switch_keyboard_layout", "", "", "", "", "");
  const bool ok = m_display != nullptr && wl_display_roundtrip(m_display) >= 0;
  if (ok) {
    invalidateCachedState();
  }
  return ok;
}

void MangoKeyboardBackend::invalidateCachedState() const { m_hasSynced = false; }

std::optional<KeyboardLayoutState> MangoKeyboardBackend::layoutState() const {
  const auto current = currentLayoutName();
  if (!current.has_value()) {
    return std::nullopt;
  }
  return KeyboardLayoutState{{*current}, 0};
}

std::optional<std::string> MangoKeyboardBackend::currentLayoutName() const {
  if (!syncState()) {
    return std::nullopt;
  }

  const auto* state = preferredOutputState();
  if (state == nullptr || state->keyboardLayout.empty()) {
    return std::nullopt;
  }
  return state->keyboardLayout;
}

void MangoKeyboardBackend::ensureConnected() const {
  if (m_initialized || m_failed) {
    return;
  }

  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    m_failed = true;
    return;
  }

  m_registry = wl_display_get_registry(m_display);
  if (m_registry == nullptr ||
      wl_registry_add_listener(m_registry, &kRegistryListener, const_cast<MangoKeyboardBackend*>(this)) != 0) {
    const_cast<MangoKeyboardBackend*>(this)->cleanup();
    m_failed = true;
    return;
  }

  m_initialized = true;

  if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0) {
    const_cast<MangoKeyboardBackend*>(this)->cleanup();
    m_failed = true;
  }
}

bool MangoKeyboardBackend::syncState() const {
  ensureConnected();
  if (!m_initialized || m_display == nullptr || m_manager == nullptr) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_hasSynced && now - m_lastSync < kSyncTtl) {
    return true;
  }

  if (wl_display_roundtrip(m_display) < 0) {
    auto* self = const_cast<MangoKeyboardBackend*>(this);
    self->cleanup();
    self->m_failed = true;
    return false;
  }

  m_lastSync = now;
  m_hasSynced = true;
  return true;
}

void MangoKeyboardBackend::cleanup() {
  for (auto& [handle, _] : m_outputByHandle) {
    if (handle != nullptr) {
      zdwl_ipc_output_v2_release(handle);
    }
  }
  m_outputByHandle.clear();

  for (auto& [_, state] : m_outputs) {
    state.handle = nullptr;
    if (state.output != nullptr) {
      wl_output_destroy(state.output);
    }
  }
  m_outputs.clear();
  m_outputsByName.clear();

  if (m_manager != nullptr) {
    zdwl_ipc_manager_v2_release(m_manager);
    m_manager = nullptr;
  }

  if (m_registry != nullptr) {
    wl_registry_destroy(m_registry);
    m_registry = nullptr;
  }

  if (m_display != nullptr) {
    wl_display_disconnect(m_display);
    m_display = nullptr;
  }

  m_initialized = false;
  m_hasSynced = false;
}

void MangoKeyboardBackend::onRegistryGlobal(std::uint32_t name, const char* interfaceName, std::uint32_t version) {
  if (interfaceName == nullptr) {
    return;
  }

  if (std::string_view(interfaceName) == wl_output_interface.name) {
    bindOutput(name);
    return;
  }

  if (std::string_view(interfaceName) != zdwl_ipc_manager_v2_interface.name || m_registry == nullptr ||
      m_manager != nullptr) {
    return;
  }

  const auto bindVersion = std::min<std::uint32_t>(version, 2);
  m_manager = static_cast<zdwl_ipc_manager_v2*>(
      wl_registry_bind(m_registry, name, &zdwl_ipc_manager_v2_interface, bindVersion)
  );
  if (m_manager == nullptr) {
    return;
  }

  zdwl_ipc_manager_v2_add_listener(m_manager, &kManagerListener, this);
  for (const auto& [output, _] : m_outputs) {
    bindOutputHandle(output);
  }
}

void MangoKeyboardBackend::onRegistryGlobalRemove(std::uint32_t name) {
  const auto it = m_outputsByName.find(name);
  if (it == m_outputsByName.end()) {
    return;
  }

  releaseOutput(it->second);
}

void MangoKeyboardBackend::onOutputActive(zdwl_ipc_output_v2* handle, std::uint32_t active) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }

  auto output = m_outputs.find(it->second);
  if (output == m_outputs.end()) {
    return;
  }

  output->second.pending.hasActive = true;
  output->second.pending.active = active != 0;
}

void MangoKeyboardBackend::onOutputKeyboardLayout(zdwl_ipc_output_v2* handle, const char* layout) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }

  auto output = m_outputs.find(it->second);
  if (output == m_outputs.end()) {
    return;
  }

  output->second.pending.hasKeyboardLayout = true;
  output->second.pending.keyboardLayout = layout != nullptr ? layout : "";
}

void MangoKeyboardBackend::onOutputFrame(zdwl_ipc_output_v2* handle) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }

  auto output = m_outputs.find(it->second);
  if (output == m_outputs.end()) {
    return;
  }

  auto& state = output->second;
  if (state.pending.hasActive) {
    state.active = state.pending.active;
  }
  if (state.pending.hasKeyboardLayout) {
    state.keyboardLayout = state.pending.keyboardLayout;
  }
  state.pending = {};
}

void MangoKeyboardBackend::bindOutput(std::uint32_t name) {
  if (m_registry == nullptr) {
    return;
  }

  auto* output = static_cast<wl_output*>(wl_registry_bind(m_registry, name, &wl_output_interface, 1));
  if (output == nullptr) {
    return;
  }

  m_outputsByName.emplace(name, output);
  m_outputs.try_emplace(
      output, OutputState{.pending = {}, .output = output, .handle = nullptr, .active = false, .keyboardLayout = {}}
  );
  bindOutputHandle(output);
}

void MangoKeyboardBackend::bindOutputHandle(wl_output* output) {
  auto it = m_outputs.find(output);
  if (m_manager == nullptr || it == m_outputs.end() || it->second.handle != nullptr) {
    return;
  }

  auto* handle = zdwl_ipc_manager_v2_get_output(m_manager, output);
  if (handle == nullptr) {
    return;
  }

  it->second.handle = handle;
  m_outputByHandle.emplace(handle, output);
  zdwl_ipc_output_v2_add_listener(handle, &kOutputListener, this);
}

void MangoKeyboardBackend::releaseOutput(wl_output* output) {
  const auto it = m_outputs.find(output);
  if (it == m_outputs.end()) {
    return;
  }

  for (auto byName = m_outputsByName.begin(); byName != m_outputsByName.end(); ++byName) {
    if (byName->second == output) {
      m_outputsByName.erase(byName);
      break;
    }
  }

  if (it->second.handle != nullptr) {
    m_outputByHandle.erase(it->second.handle);
    zdwl_ipc_output_v2_release(it->second.handle);
  }
  if (it->second.output != nullptr) {
    wl_output_destroy(it->second.output);
  }
  m_outputs.erase(it);
}

const MangoKeyboardBackend::OutputState* MangoKeyboardBackend::preferredOutputState() const {
  for (const auto& [_, state] : m_outputs) {
    if (state.active && !state.keyboardLayout.empty()) {
      return &state;
    }
  }
  for (const auto& [_, state] : m_outputs) {
    if (state.active) {
      return &state;
    }
  }
  for (const auto& [_, state] : m_outputs) {
    if (!state.keyboardLayout.empty()) {
      return &state;
    }
  }
  if (!m_outputs.empty()) {
    return &m_outputs.begin()->second;
  }
  return nullptr;
}
