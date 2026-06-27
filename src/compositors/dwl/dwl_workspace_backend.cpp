#include "compositors/dwl/dwl_workspace_backend.h"

#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <utility>

namespace {

  void managerTags(void* data, zdwl_ipc_manager_v2* /*manager*/, std::uint32_t amount) {
    static_cast<DwlWorkspaceBackend*>(data)->onTagCount(amount);
  }

  void managerLayout(void* data, zdwl_ipc_manager_v2* /*manager*/, const char* name) {
    static_cast<DwlWorkspaceBackend*>(data)->onLayoutAnnounced(name);
  }

  const zdwl_ipc_manager_v2_listener kManagerListener = {
      .tags = managerTags,
      .layout = managerLayout,
  };

  void outputToggleVisibility(void* /*data*/, zdwl_ipc_output_v2* /*output*/) {}

  void outputActive(void* data, zdwl_ipc_output_v2* output, std::uint32_t active) {
    static_cast<DwlWorkspaceBackend*>(data)->onOutputActive(output, active);
  }

  void outputTag(
      void* data, zdwl_ipc_output_v2* output, std::uint32_t tag, std::uint32_t state, std::uint32_t clients,
      std::uint32_t focused
  ) {
    static_cast<DwlWorkspaceBackend*>(data)->onOutputTag(output, tag, state, clients, focused);
  }

  void outputLayout(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::uint32_t /*layout*/) {}

  void outputTitle(void* data, zdwl_ipc_output_v2* output, const char* title) {
    static_cast<DwlWorkspaceBackend*>(data)->onOutputTitle(output, title);
  }

  void outputAppId(void* data, zdwl_ipc_output_v2* output, const char* appId) {
    static_cast<DwlWorkspaceBackend*>(data)->onOutputAppId(output, appId);
  }

  void outputLayoutSymbol(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*layout*/) {}

  void outputFrame(void* data, zdwl_ipc_output_v2* output) {
    static_cast<DwlWorkspaceBackend*>(data)->onOutputFrame(output);
  }

  void outputFullscreen(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::uint32_t /*state*/) {}
  void outputFloating(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::uint32_t /*state*/) {}
  void outputX(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::int32_t /*x*/) {}
  void outputY(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::int32_t /*y*/) {}
  void outputWidth(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::int32_t /*width*/) {}
  void outputHeight(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::int32_t /*height*/) {}
  void outputLastLayer(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*layer*/) {}
  void outputKbLayout(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*layout*/) {}
  void outputKeymode(void* /*data*/, zdwl_ipc_output_v2* /*output*/, const char* /*keymode*/) {}
  void outputScaleFactor(void* /*data*/, zdwl_ipc_output_v2* /*output*/, std::uint32_t /*scale*/) {}

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

void DwlWorkspaceBackend::bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) {
  if (manager == nullptr || manager == m_manager) {
    return;
  }
  if (m_manager != nullptr) {
    cleanup();
  }
  m_manager = manager;
  zdwl_ipc_manager_v2_add_listener(m_manager, &kManagerListener, this);
  for (const auto& [output, _] : m_outputs) {
    ensureOutputBound(output);
  }
}

void DwlWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void DwlWorkspaceBackend::activate(const std::string& id) {
  auto* state = activeOutputState();
  if (state == nullptr) {
    state = const_cast<OutputState*>(preferredOutputState());
  }
  if (state != nullptr) {
    activateForOutput(state->output, id);
  }
}

void DwlWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  auto it = m_outputs.find(output);
  if (it == m_outputs.end()) {
    return;
  }

  const auto displayIndex = parseTagIndex(id);
  if (!displayIndex.has_value() || *displayIndex >= it->second.tags.size() || it->second.handle == nullptr) {
    return;
  }

  const std::size_t protocolIndex = protocolIndexForDisplay(*displayIndex);
  zdwl_ipc_output_v2_set_tags(it->second.handle, 1u << protocolIndex, 0);
}

void DwlWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  const auto index = parseTagIndex(workspace);
  if (!index.has_value()) {
    return;
  }
  activateForOutput(output, std::to_string(*index + 1));
}

std::vector<Workspace> DwlWorkspaceBackend::all() const {
  const auto* state = preferredOutputState();
  return state != nullptr ? forOutput(state->output) : std::vector<Workspace>{};
}

std::vector<Workspace> DwlWorkspaceBackend::forOutput(wl_output* output) const {
  const auto* state = output != nullptr ? outputStateFor(output) : preferredOutputState();
  if (state == nullptr) {
    return {};
  }

  std::vector<Workspace> result;
  result.reserve(state->tags.size());
  for (std::size_t displayIndex = 0; displayIndex < state->tags.size(); ++displayIndex) {
    const std::size_t protocolIndex = protocolIndexForDisplay(displayIndex);
    result.push_back(makeWorkspace(displayIndex, state->tags[protocolIndex]));
  }
  return result;
}

void DwlWorkspaceBackend::cleanup() {
  for (auto& [handle, _] : m_outputByHandle) {
    if (handle != nullptr) {
      zdwl_ipc_output_v2_release(handle);
    }
  }
  m_outputByHandle.clear();
  for (auto& [_, state] : m_outputs) {
    state.handle = nullptr;
  }
  if (m_manager != nullptr) {
    zdwl_ipc_manager_v2_release(m_manager);
    m_manager = nullptr;
  }
}

void DwlWorkspaceBackend::onOutputAdded(wl_output* output) {
  if (output == nullptr) {
    return;
  }
  OutputState state;
  state.output = output;
  m_outputs.try_emplace(output, std::move(state));
  ensureOutputBound(output);
}

void DwlWorkspaceBackend::onOutputRemoved(wl_output* output) {
  const auto it = m_outputs.find(output);
  if (it == m_outputs.end()) {
    return;
  }

  if (it->second.handle != nullptr) {
    m_outputByHandle.erase(it->second.handle);
    zdwl_ipc_output_v2_release(it->second.handle);
  }
  m_outputs.erase(it);
}

void DwlWorkspaceBackend::onTagCount(std::uint32_t amount) {
  m_tagCount = std::max(m_tagCount, amount);
  for (auto& [_, state] : m_outputs) {
    state.tags.resize(m_tagCount);
  }
}

void DwlWorkspaceBackend::onLayoutAnnounced(const char* name) { m_layouts.emplace_back(name != nullptr ? name : ""); }

void DwlWorkspaceBackend::onOutputActive(zdwl_ipc_output_v2* handle, std::uint32_t active) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }
  auto state = m_outputs.find(it->second);
  if (state != m_outputs.end()) {
    state->second.hasPendingIpcActive = true;
    state->second.pendingIpcActive = active != 0;
  }
}

void DwlWorkspaceBackend::onOutputTitle(zdwl_ipc_output_v2* handle, const char* title) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }
  auto state = m_outputs.find(it->second);
  if (state != m_outputs.end()) {
    state->second.hasPendingTitle = true;
    state->second.pendingTitle = StringUtils::windowTitleSingleLine(title != nullptr ? title : "");
  }
}

void DwlWorkspaceBackend::onOutputAppId(zdwl_ipc_output_v2* handle, const char* appId) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }
  auto state = m_outputs.find(it->second);
  if (state != m_outputs.end()) {
    state->second.hasPendingAppId = true;
    state->second.pendingAppId = appId != nullptr ? appId : "";
  }
}

void DwlWorkspaceBackend::onOutputTag(
    zdwl_ipc_output_v2* handle, std::uint32_t tag, std::uint32_t stateValue, std::uint32_t clients,
    std::uint32_t /*focused*/
) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }

  auto output = m_outputs.find(it->second);
  if (output == m_outputs.end()) {
    return;
  }

  const std::uint32_t inferredCount = tag + 1;
  if (inferredCount > m_tagCount) {
    m_tagCount = inferredCount;
    for (auto& [_, otherState] : m_outputs) {
      otherState.tags.resize(m_tagCount);
    }
  }

  if (tag >= output->second.tags.size()) {
    output->second.tags.resize(tag + 1);
  }

  auto& tagInfo = output->second.tags[tag];
  tagInfo.active = (stateValue & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE) != 0;
  tagInfo.urgent = (stateValue & ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT) != 0;
  tagInfo.occupied = clients > 0;
}

void DwlWorkspaceBackend::onOutputFrame(zdwl_ipc_output_v2* handle) {
  const auto it = m_outputByHandle.find(handle);
  if (it != m_outputByHandle.end()) {
    const auto stateIt = m_outputs.find(it->second);
    if (stateIt != m_outputs.end()) {
      auto& state = stateIt->second;
      if (state.hasPendingIpcActive) {
        if (state.pendingIpcActive) {
          for (auto& [_, other] : m_outputs) {
            other.active = false;
          }
          state.active = true;
        } else {
          state.active = false;
        }
        state.hasPendingIpcActive = false;
      }
      if (state.hasPendingTitle) {
        state.title = std::move(state.pendingTitle);
        state.hasPendingTitle = false;
      }
      if (state.hasPendingAppId) {
        state.appId = std::move(state.pendingAppId);
        state.hasPendingAppId = false;
      }
    }
  }
  notifyChanged();
}

void DwlWorkspaceBackend::ensureOutputBound(wl_output* output) {
  auto it = m_outputs.find(output);
  if (m_manager == nullptr || it == m_outputs.end() || it->second.handle != nullptr) {
    return;
  }

  auto* handle = zdwl_ipc_manager_v2_get_output(m_manager, output);
  if (handle == nullptr) {
    return;
  }

  it->second.handle = handle;
  it->second.tags.resize(m_tagCount);
  m_outputByHandle.emplace(handle, output);
  zdwl_ipc_output_v2_add_listener(handle, &kOutputListener, this);
}

DwlWorkspaceBackend::OutputState* DwlWorkspaceBackend::activeOutputState() {
  for (auto& [_, state] : m_outputs) {
    if (state.active) {
      return &state;
    }
  }
  return nullptr;
}

const DwlWorkspaceBackend::OutputState* DwlWorkspaceBackend::preferredOutputState() const {
  for (const auto& [_, state] : m_outputs) {
    if (state.active) {
      return &state;
    }
  }
  if (!m_outputs.empty()) {
    return &m_outputs.begin()->second;
  }
  return nullptr;
}

const DwlWorkspaceBackend::OutputState* DwlWorkspaceBackend::outputStateFor(wl_output* output) const {
  const auto it = m_outputs.find(output);
  return it != m_outputs.end() ? &it->second : nullptr;
}

std::optional<std::size_t> DwlWorkspaceBackend::parseTagIndex(const Workspace& workspace) {
  if (!workspace.coordinates.empty()) {
    return static_cast<std::size_t>(workspace.coordinates[0]);
  }
  return parseTagIndex(workspace.id.empty() ? workspace.name : workspace.id);
}

std::optional<std::size_t> DwlWorkspaceBackend::parseTagIndex(const std::string& id) {
  if (id.empty()) {
    return std::nullopt;
  }

  std::size_t value = 0;
  const char* start = id.data();
  const char* end = id.data() + id.size();
  const auto [ptr, ec] = std::from_chars(start, end, value);
  if (ec != std::errc{} || ptr != end || value == 0) {
    return std::nullopt;
  }
  return value - 1;
}

std::size_t DwlWorkspaceBackend::protocolIndexForDisplay(std::size_t displayIndex) const { return displayIndex; }

Workspace DwlWorkspaceBackend::makeWorkspace(std::size_t index, const TagInfo& tag) {
  return Workspace{
      .id = std::to_string(index + 1),
      .name = std::to_string(index + 1),
      .coordinates = {static_cast<std::uint32_t>(index)},
      .index = static_cast<std::uint32_t>(index + 1),
      .active = tag.active,
      .urgent = tag.urgent,
      .occupied = tag.occupied,
  };
}

void DwlWorkspaceBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

wl_output* DwlWorkspaceBackend::ipcSelectedOutput() const {
  for (const auto& [_, state] : m_outputs) {
    if (state.active) {
      return state.output;
    }
  }
  return nullptr;
}

std::optional<std::pair<std::string, std::string>>
DwlWorkspaceBackend::ipcFocusedClientForOutput(wl_output* output) const {
  if (output == nullptr) {
    return std::nullopt;
  }
  const auto it = m_outputs.find(output);
  if (it == m_outputs.end()) {
    return std::nullopt;
  }
  return std::pair<std::string, std::string>{it->second.title, it->second.appId};
}
