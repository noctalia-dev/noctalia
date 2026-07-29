#include "wayland/wayland_toplevels.h"

#include "system/app_identity.h"
#include "system/internal_app_metadata.h"
#include "util/string_utils.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace {

  void
  managerToplevel(void* data, zwlr_foreign_toplevel_manager_v1* /*manager*/, zwlr_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandToplevels*>(data)->onToplevelCreated(handle);
  }

  void managerFinished(void* data, zwlr_foreign_toplevel_manager_v1* /*manager*/) {
    static_cast<WaylandToplevels*>(data)->onManagerFinished();
  }

  const zwlr_foreign_toplevel_manager_v1_listener kManagerListener = {
      .toplevel = managerToplevel,
      .finished = managerFinished,
  };

  void handleClosed(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandToplevels*>(data)->onHandleClosed(handle);
  }

  void handleDone(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    static_cast<WaylandToplevels*>(data)->onHandleDone(handle);
  }

  void handleTitle(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
    static_cast<WaylandToplevels*>(data)->onHandleTitle(handle, title);
  }

  void handleAppId(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* appId) {
    static_cast<WaylandToplevels*>(data)->onHandleAppId(handle, appId);
  }

  void handleState(void* data, zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
    static_cast<WaylandToplevels*>(data)->onHandleState(handle, state);
  }

  void handleOutputEnter(void* data, zwlr_foreign_toplevel_handle_v1* handle, wl_output* output) {
    static_cast<WaylandToplevels*>(data)->onHandleOutputEnter(handle, output);
  }
  void handleOutputLeave(void* data, zwlr_foreign_toplevel_handle_v1* handle, wl_output* output) {
    static_cast<WaylandToplevels*>(data)->onHandleOutputLeave(handle, output);
  }
  void handleParent(
      void* /*data*/, zwlr_foreign_toplevel_handle_v1* /*handle*/, zwlr_foreign_toplevel_handle_v1* /*parent*/
  ) {}

  const zwlr_foreign_toplevel_handle_v1_listener kHandleListener = {
      .title = handleTitle,
      .app_id = handleAppId,
      .output_enter = handleOutputEnter,
      .output_leave = handleOutputLeave,
      .state = handleState,
      .done = handleDone,
      .closed = handleClosed,
      .parent = handleParent,
  };

  std::string effectiveAppId(const std::string& appId, const std::string& title) {
    if (!appId.empty()) {
      return appId;
    }
    if (const auto* app = internal_apps::appDefinitionForWindowTitle(title); app != nullptr) {
      return std::string(app->appId);
    }
    return {};
  }

} // namespace

void WaylandToplevels::bind(zwlr_foreign_toplevel_manager_v1* manager) {
  m_manager = manager;
  zwlr_foreign_toplevel_manager_v1_add_listener(m_manager, &kManagerListener, this);
}

void WaylandToplevels::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void WaylandToplevels::cleanup() {
  for (auto& [handle, _] : m_handles) {
    if (handle != nullptr) {
      zwlr_foreign_toplevel_handle_v1_destroy(handle);
    }
  }
  m_handles.clear();
  m_currentHandle = nullptr;

  if (m_manager != nullptr) {
    zwlr_foreign_toplevel_manager_v1_stop(m_manager);
    zwlr_foreign_toplevel_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

std::optional<ActiveToplevel> WaylandToplevels::current() const {
  if (m_currentHandle == nullptr) {
    return std::nullopt;
  }
  const auto it = m_handles.find(m_currentHandle);
  if (it == m_handles.end()) {
    return std::nullopt;
  }
  return ActiveToplevel{
      .title = it->second.title,
      .appId = it->second.appId,
      .identifier = it->second.appId + ":" + it->second.title,
      .handle = m_currentHandle,
  };
}

std::optional<ActiveToplevel> WaylandToplevels::matchByTitleAndAppId(
    std::string_view title, std::string_view appId, wl_output* preferredOutput
) const {
  std::optional<ActiveToplevel> best;
  std::uint64_t bestScore = 0;

  for (const auto& [handle, state] : m_handles) {
    if (state.title != title || state.appId != appId) {
      continue;
    }
    std::uint64_t score = state.generation;
    if (preferredOutput != nullptr && state.output == preferredOutput) {
      score += (1ULL << 62);
    }
    if (state.activated) {
      score += (1ULL << 61);
    }
    if (!best.has_value() || score > bestScore) {
      best = ActiveToplevel{
          .title = state.title,
          .appId = state.appId,
          .identifier = state.appId + ":" + state.title,
          .handle = handle,
      };
      bestScore = score;
    }
  }
  return best;
}

void WaylandToplevels::onToplevelCreated(zwlr_foreign_toplevel_handle_v1* handle) {
  if (handle == nullptr) {
    return;
  }
  auto [it, inserted] = m_handles.try_emplace(handle, ToplevelState{});
  if (inserted) {
    it->second.order = m_nextOrder++;
  }
  zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, this);
}

void WaylandToplevels::onManagerFinished() {
  if (m_manager != nullptr) {
    zwlr_foreign_toplevel_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

void WaylandToplevels::onHandleClosed(zwlr_foreign_toplevel_handle_v1* handle) {
  const auto before = current();

  if (handle != nullptr) {
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    m_handles.erase(handle);
  }
  if (m_currentHandle == handle) {
    m_currentHandle = nullptr;
    m_currentHandle = latestActivatedHandle();
  }

  const bool activeChanged = notifyIfChanged(before);
  if (!activeChanged && m_changeCallback) {
    m_changeCallback();
  }
}

void WaylandToplevels::onHandleDone(zwlr_foreign_toplevel_handle_v1* handle) {
  auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }

  const auto before = current();
  const bool hadModelChanges = it->second.dirty;
  if (it->second.activated) {
    m_currentHandle = handle;
  } else if (m_currentHandle == handle) {
    m_currentHandle = latestActivatedHandle();
  }
  it->second.dirty = false;

  const bool activeChanged = notifyIfChanged(before);
  if (hadModelChanges && !activeChanged && m_changeCallback) {
    m_changeCallback();
  }
}

void WaylandToplevels::onHandleTitle(zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
  auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.title = StringUtils::windowTitleSingleLine(title != nullptr ? title : "");
  it->second.dirty = true;
  it->second.generation = ++m_generation;
}

void WaylandToplevels::onHandleAppId(zwlr_foreign_toplevel_handle_v1* handle, const char* appId) {
  auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.appId = appId != nullptr ? appId : "";
  it->second.dirty = true;
  it->second.generation = ++m_generation;
}

void WaylandToplevels::onHandleState(zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
  auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }

  bool activated = false;
  bool minimized = false;
  if (state != nullptr) {
    auto* value = static_cast<const std::uint32_t*>(state->data);
    const auto count = state->size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
      if (value[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
        activated = true;
      } else if (value[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
        minimized = true;
      }
    }
  }
  if (activated) {
    for (auto& [otherHandle, otherState] : m_handles) {
      if (otherHandle == handle || !otherState.activated) {
        continue;
      }
      otherState.activated = false;
      otherState.dirty = true;
      otherState.generation = ++m_generation;
    }
  }

  it->second.activated = activated;
  it->second.minimized = minimized;
  it->second.dirty = true;
  it->second.generation = ++m_generation;
}

wl_output* WaylandToplevels::currentOutput() const {
  if (m_currentHandle == nullptr) {
    return nullptr;
  }
  const auto it = m_handles.find(m_currentHandle);
  if (it == m_handles.end()) {
    return nullptr;
  }
  return it->second.output;
}

std::vector<std::string> WaylandToplevels::allAppIds(wl_output* outputFilter) const {
  std::vector<const ToplevelState*> ordered;
  ordered.reserve(m_handles.size());
  for (const auto& [handle, state] : m_handles) {
    (void)handle;
    if (outputFilter != nullptr && state.output != outputFilter) {
      continue;
    }
    ordered.push_back(&state);
  }
  std::ranges::sort(ordered, [](const ToplevelState* lhs, const ToplevelState* rhs) {
    return lhs->order < rhs->order;
  });

  std::vector<std::string> ids;
  ids.reserve(ordered.size());
  for (const auto* state : ordered) {
    const auto appId = effectiveAppId(state->appId, state->title);
    if (!appId.empty()) {
      ids.push_back(appId);
    }
  }
  return ids;
}

std::vector<ToplevelInfo> WaylandToplevels::windowsForApp(
    const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter
) const {
  struct MatchedWindow {
    std::uint64_t order = 0;
    ToplevelInfo info;
  };

  std::vector<MatchedWindow> matched;
  std::vector<ToplevelInfo> out;
  for (const auto& [handle, state] : m_handles) {
    if (outputFilter != nullptr && state.output != outputFilter) {
      continue;
    }
    const auto appId = effectiveAppId(state.appId, state.title);
    if (appId.empty())
      continue;
    const auto appLower = [&] {
      std::string s = appId;
      for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }();
    if (app_identity::matchesLower(appLower, idLower, wmClassLower, {})) {
      matched.push_back(
          MatchedWindow{
              .order = state.order,
              .info = ToplevelInfo{
                  .title = state.title,
                  .appId = appId,
                  .identifier = appId + ":" + state.title,
                  .order = state.order,
                  .handle = handle,
              },
          }
      );
    }
  }
  std::ranges::sort(matched, {}, &MatchedWindow::order);
  out.reserve(matched.size());
  for (auto& window : matched) {
    out.push_back(std::move(window.info));
  }
  return out;
}

std::vector<ToplevelInfo> WaylandToplevels::windowsWithoutAppId(wl_output* outputFilter) const {
  struct OrphanWindow {
    std::uint64_t order = 0;
    ToplevelInfo info;
  };

  std::vector<OrphanWindow> orphans;
  for (const auto& [handle, state] : m_handles) {
    if (outputFilter != nullptr && state.output != outputFilter) {
      continue;
    }
    if (!effectiveAppId(state.appId, state.title).empty()) {
      continue;
    }
    orphans.push_back(
        OrphanWindow{
            .order = state.order,
            .info = ToplevelInfo{
                .title = state.title,
                .appId = {},
                .identifier = state.title,
                .order = state.order,
                .handle = handle,
            },
        }
    );
  }
  std::ranges::sort(orphans, {}, &OrphanWindow::order);

  std::vector<ToplevelInfo> out;
  out.reserve(orphans.size());
  for (auto& window : orphans) {
    out.push_back(std::move(window.info));
  }
  return out;
}

bool WaylandToplevels::containsWlrHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return handle != nullptr && m_handles.contains(handle);
}

void WaylandToplevels::activateHandle(zwlr_foreign_toplevel_handle_v1* handle, wl_seat* seat) {
  if (handle == nullptr || seat == nullptr)
    return;
  zwlr_foreign_toplevel_handle_v1_activate(handle, seat);
}

void WaylandToplevels::closeHandle(zwlr_foreign_toplevel_handle_v1* handle) {
  if (handle == nullptr)
    return;
  zwlr_foreign_toplevel_handle_v1_close(handle);
}

void WaylandToplevels::onHandleOutputEnter(zwlr_foreign_toplevel_handle_v1* handle, wl_output* output) {
  auto it = m_handles.find(handle);
  if (it != m_handles.end()) {
    if (!std::ranges::contains(it->second.activeOutputs, output)) {
      it->second.activeOutputs.push_back(output);
    }
    if (it->second.output != output) {
      it->second.output = output;
      it->second.dirty = true;
      it->second.generation = ++m_generation;
    }
  }
}

void WaylandToplevels::onHandleOutputLeave(zwlr_foreign_toplevel_handle_v1* handle, wl_output* output) {
  auto it = m_handles.find(handle);
  if (it != m_handles.end()) {
    const auto pos = std::ranges::find(it->second.activeOutputs, output);
    if (pos != it->second.activeOutputs.end()) {
      it->second.activeOutputs.erase(pos);
    }
    if (it->second.output == output) {
      it->second.output = it->second.activeOutputs.empty() ? nullptr : it->second.activeOutputs.back();
      it->second.dirty = true;
      it->second.generation = ++m_generation;
    }
  }
}

bool WaylandToplevels::notifyIfChanged(const std::optional<ActiveToplevel>& before) {
  const auto now = current();
  if (before.has_value() != now.has_value()) {
    if (m_changeCallback) {
      m_changeCallback();
    }
    return true;
  }
  if (!before.has_value() || !now.has_value()) {
    return false;
  }
  if (before->title != now->title
      || before->appId != now->appId
      || before->identifier != now->identifier
      || before->handle != now->handle) {
    if (m_changeCallback) {
      m_changeCallback();
    }
    return true;
  }
  return false;
}

zwlr_foreign_toplevel_handle_v1* WaylandToplevels::latestActivatedHandle() const {
  zwlr_foreign_toplevel_handle_v1* bestHandle = nullptr;
  std::uint64_t bestGeneration = 0;

  for (const auto& [handle, state] : m_handles) {
    if (!state.activated) {
      continue;
    }
    if (bestHandle == nullptr || state.generation > bestGeneration) {
      bestHandle = handle;
      bestGeneration = state.generation;
    }
  }

  return bestHandle;
}
