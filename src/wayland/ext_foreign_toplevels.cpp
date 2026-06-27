#include "wayland/ext_foreign_toplevels.h"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "system/app_identity.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>

namespace {

  void listToplevel(void* data, ext_foreign_toplevel_list_v1* /*list*/, ext_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandExtForeignToplevels*>(data)->onToplevelCreated(handle);
  }

  void listFinished(void* data, ext_foreign_toplevel_list_v1* /*list*/) {
    static_cast<WaylandExtForeignToplevels*>(data)->onListFinished();
  }

  const ext_foreign_toplevel_list_v1_listener kListListener = {
      .toplevel = listToplevel,
      .finished = listFinished,
  };

  void handleClosed(void* data, ext_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandExtForeignToplevels*>(data)->onHandleClosed(handle);
  }

  void handleDone(void* data, ext_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandExtForeignToplevels*>(data)->onHandleDone(handle);
  }

  void handleTitle(void* data, ext_foreign_toplevel_handle_v1* handle, const char* title) {
    static_cast<WaylandExtForeignToplevels*>(data)->onHandleTitle(handle, title);
  }

  void handleAppId(void* data, ext_foreign_toplevel_handle_v1* handle, const char* appId) {
    static_cast<WaylandExtForeignToplevels*>(data)->onHandleAppId(handle, appId);
  }

  void handleIdentifier(void* data, ext_foreign_toplevel_handle_v1* handle, const char* identifier) {
    static_cast<WaylandExtForeignToplevels*>(data)->onHandleIdentifier(handle, identifier);
  }

  const ext_foreign_toplevel_handle_v1_listener kHandleListener = {
      .closed = handleClosed,
      .done = handleDone,
      .title = handleTitle,
      .app_id = handleAppId,
      .identifier = handleIdentifier,
  };

  std::string effectiveAppId(const std::string& appId, const std::string& /*title*/) { return appId; }

} // namespace

void WaylandExtForeignToplevels::bind(ext_foreign_toplevel_list_v1* list, wl_display* display) {
  m_list = list;
  m_display = display;
  ext_foreign_toplevel_list_v1_add_listener(m_list, &kListListener, this);
  requestInitialSync();
}

void WaylandExtForeignToplevels::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void WaylandExtForeignToplevels::cleanup() {
  for (auto& [handle, _] : m_handles) {
    if (handle != nullptr) {
      ext_foreign_toplevel_handle_v1_destroy(handle);
    }
  }
  m_handles.clear();

  if (m_list != nullptr) {
    ext_foreign_toplevel_list_v1_destroy(m_list);
    m_list = nullptr;
  }
  m_display = nullptr;
  m_initialSyncDone = false;
}

void WaylandExtForeignToplevels::requestInitialSync() {
  if (m_display == nullptr || m_initialSyncDone) {
    return;
  }
  (void)wl_display_roundtrip(m_display);
  m_initialSyncDone = true;
  notifyChanged();
}

std::vector<std::string> WaylandExtForeignToplevels::allAppIds() const {
  std::vector<const ToplevelState*> ordered;
  ordered.reserve(m_handles.size());
  for (const auto& [_, state] : m_handles) {
    const auto appId = effectiveAppId(state.appId, state.title);
    if (!appId.empty()) {
      ordered.push_back(&state);
    }
  }
  std::ranges::sort(ordered, [](const ToplevelState* lhs, const ToplevelState* rhs) {
    return lhs->order < rhs->order;
  });

  std::vector<std::string> ids;
  ids.reserve(ordered.size());
  for (const auto* state : ordered) {
    ids.push_back(effectiveAppId(state->appId, state->title));
  }
  return ids;
}

std::vector<ToplevelInfo>
WaylandExtForeignToplevels::windowsForApp(const std::string& idLower, const std::string& wmClassLower) const {
  struct MatchedWindow {
    std::uint64_t order = 0;
    ToplevelInfo info;
  };

  std::vector<MatchedWindow> matched;
  for (const auto& [handle, state] : m_handles) {
    const auto appId = effectiveAppId(state.appId, state.title);
    if (appId.empty()) {
      continue;
    }
    std::string appLower = appId;
    for (auto& c : appLower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!app_identity::matchesLower(appLower, idLower, wmClassLower, {})) {
      continue;
    }
    matched.push_back(
        MatchedWindow{
            .order = state.order,
            .info = ToplevelInfo{
                .title = state.title,
                .appId = appId,
                .identifier = state.identifier,
                .order = state.order,
                .handle = nullptr,
                .extHandle = handle,
            },
        }
    );
  }
  std::ranges::sort(matched, {}, &MatchedWindow::order);

  std::vector<ToplevelInfo> out;
  out.reserve(matched.size());
  for (auto& window : matched) {
    out.push_back(std::move(window.info));
  }
  return out;
}

void WaylandExtForeignToplevels::onToplevelCreated(ext_foreign_toplevel_handle_v1* handle) {
  if (handle == nullptr) {
    return;
  }
  auto [it, inserted] = m_handles.try_emplace(handle, ToplevelState{});
  if (inserted) {
    it->second.order = m_nextOrder++;
  }
  ext_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, this);
}

void WaylandExtForeignToplevels::onListFinished() {
  if (m_list != nullptr) {
    ext_foreign_toplevel_list_v1_destroy(m_list);
    m_list = nullptr;
  }
}

void WaylandExtForeignToplevels::onHandleClosed(ext_foreign_toplevel_handle_v1* handle) {
  if (handle != nullptr) {
    ext_foreign_toplevel_handle_v1_destroy(handle);
    m_handles.erase(handle);
    notifyChanged();
  }
}

void WaylandExtForeignToplevels::onHandleDone(ext_foreign_toplevel_handle_v1* /*handle*/) { notifyChanged(); }

void WaylandExtForeignToplevels::onHandleTitle(ext_foreign_toplevel_handle_v1* handle, const char* title) {
  const auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.title = StringUtils::windowTitleSingleLine(title != nullptr ? title : "");
}

void WaylandExtForeignToplevels::onHandleAppId(ext_foreign_toplevel_handle_v1* handle, const char* appId) {
  const auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.appId = appId != nullptr ? appId : "";
}

void WaylandExtForeignToplevels::onHandleIdentifier(ext_foreign_toplevel_handle_v1* handle, const char* identifier) {
  const auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.identifier = identifier != nullptr ? identifier : "";
}

void WaylandExtForeignToplevels::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
