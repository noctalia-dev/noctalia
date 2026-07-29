#pragma once

#include "wayland/wayland_toplevels.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ext_foreign_toplevel_handle_v1;
struct ext_foreign_toplevel_list_v1;
struct wl_display;

// ext-foreign-toplevel-list enumerates all mapped toplevels on bind (unlike wlr foreign-toplevel on Hyprland).
class WaylandExtForeignToplevels {
public:
  using ChangeCallback = std::function<void()>;

  void bind(ext_foreign_toplevel_list_v1* list, wl_display* display);
  void setChangeCallback(ChangeCallback callback);
  void cleanup();

  [[nodiscard]] bool isBound() const noexcept { return m_list != nullptr; }
  [[nodiscard]] std::vector<std::string> allAppIds() const;
  [[nodiscard]] std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower) const;
  [[nodiscard]] std::vector<ToplevelInfo> windowsWithoutAppId() const;

  template <typename Fn> void visitExtHandles(Fn&& fn) const {
    for (const auto& [handle, _] : m_handles) {
      if (handle != nullptr) {
        fn(handle);
      }
    }
  }

  void onToplevelCreated(ext_foreign_toplevel_handle_v1* handle);
  void onListFinished();
  void onHandleClosed(ext_foreign_toplevel_handle_v1* handle);
  void onHandleDone(ext_foreign_toplevel_handle_v1* handle);
  void onHandleTitle(ext_foreign_toplevel_handle_v1* handle, const char* title);
  void onHandleAppId(ext_foreign_toplevel_handle_v1* handle, const char* appId);
  void onHandleIdentifier(ext_foreign_toplevel_handle_v1* handle, const char* identifier);

private:
  struct ToplevelState {
    std::string title;
    std::string appId;
    std::string identifier;
    std::uint64_t order = 0;
  };

  void requestInitialSync();
  void notifyChanged();

  ext_foreign_toplevel_list_v1* m_list = nullptr;
  wl_display* m_display = nullptr;
  std::unordered_map<ext_foreign_toplevel_handle_v1*, ToplevelState> m_handles;
  std::uint64_t m_nextOrder = 0;
  bool m_initialSyncDone = false;
  ChangeCallback m_changeCallback;
};
