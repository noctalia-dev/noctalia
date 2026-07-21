#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct zwlr_foreign_toplevel_handle_v1;
struct zwlr_foreign_toplevel_manager_v1;
struct ext_foreign_toplevel_handle_v1;
struct wl_array;
struct wl_output;
struct wl_seat;

struct ActiveToplevel {
  std::string title;
  std::string appId;
  std::string identifier;
  zwlr_foreign_toplevel_handle_v1* handle = nullptr;
};

struct ToplevelInfo {
  std::string title;
  std::string appId;
  std::string identifier;
  std::uint64_t order = 0;
  zwlr_foreign_toplevel_handle_v1* handle = nullptr;
  ext_foreign_toplevel_handle_v1* extHandle = nullptr;
};

struct WlrToplevelSnapshot {
  zwlr_foreign_toplevel_handle_v1* handle = nullptr;
  std::string title;
  std::string appId;
  wl_output* output = nullptr;
  bool activated = false;
  bool minimized = false;
  std::uint64_t order = 0;
};

class WaylandToplevels {
public:
  using ChangeCallback = std::function<void()>;

  void bind(zwlr_foreign_toplevel_manager_v1* manager);
  void setChangeCallback(ChangeCallback callback);
  void cleanup();

  [[nodiscard]] std::optional<ActiveToplevel> current() const;
  [[nodiscard]] wl_output* currentOutput() const;
  [[nodiscard]] std::optional<ActiveToplevel>
  matchByTitleAndAppId(std::string_view title, std::string_view appId, wl_output* preferredOutput) const;
  [[nodiscard]] std::vector<std::string> allAppIds(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  // Toplevels with no app id / class — still focusable via handle.
  [[nodiscard]] std::vector<ToplevelInfo> windowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] bool containsWlrHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
  void activateHandle(zwlr_foreign_toplevel_handle_v1* handle, wl_seat* seat);
  void closeHandle(zwlr_foreign_toplevel_handle_v1* handle);

  // Listener entrypoints called by C callbacks
  void onToplevelCreated(zwlr_foreign_toplevel_handle_v1* handle);
  void onManagerFinished();
  void onHandleClosed(zwlr_foreign_toplevel_handle_v1* handle);
  void onHandleDone(zwlr_foreign_toplevel_handle_v1* handle);
  void onHandleTitle(zwlr_foreign_toplevel_handle_v1* handle, const char* title);
  void onHandleAppId(zwlr_foreign_toplevel_handle_v1* handle, const char* appId);
  void onHandleState(zwlr_foreign_toplevel_handle_v1* handle, wl_array* state);
  void onHandleOutputEnter(zwlr_foreign_toplevel_handle_v1* handle, wl_output* output);
  void onHandleOutputLeave(zwlr_foreign_toplevel_handle_v1* handle, wl_output* output);

  template <typename Fn> void visitWlrHandles(Fn&& fn) const {
    for (const auto& [handle, _] : m_handles) {
      if (handle != nullptr) {
        fn(handle);
      }
    }
  }

  template <typename Fn> void visitWlrToplevels(Fn&& fn) const {
    for (const auto& [handle, state] : m_handles) {
      if (handle == nullptr) {
        continue;
      }
      fn(WlrToplevelSnapshot{
          .handle = handle,
          .title = state.title,
          .appId = state.appId,
          .output = state.output,
          .activated = state.activated,
          .minimized = state.minimized,
          .order = state.order,
      });
    }
  }

private:
  struct ToplevelState {
    std::string title;
    std::string appId;
    wl_output* output = nullptr;
    std::vector<wl_output*> activeOutputs;
    bool activated = false;
    bool minimized = false;
    bool dirty = false;
    std::uint64_t generation = 0;
    std::uint64_t order = 0;
  };

  [[nodiscard]] bool notifyIfChanged(const std::optional<ActiveToplevel>& before);
  [[nodiscard]] zwlr_foreign_toplevel_handle_v1* latestActivatedHandle() const;

  zwlr_foreign_toplevel_manager_v1* m_manager = nullptr;
  std::unordered_map<zwlr_foreign_toplevel_handle_v1*, ToplevelState> m_handles;
  zwlr_foreign_toplevel_handle_v1* m_currentHandle = nullptr;
  std::uint64_t m_generation = 0;
  std::uint64_t m_nextOrder = 0;
  ChangeCallback m_changeCallback;
};
