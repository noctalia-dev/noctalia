#pragma once

#include "shell/wayland/ext_foreign_toplevels.h"
#include "shell/wayland/wayland_toplevels.h"
#include "wayland/wayland_extension.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class WaylandConnection;
struct ext_foreign_toplevel_handle_v1;
struct wl_output;
struct wl_surface;
struct zwlr_foreign_toplevel_handle_v1;

// wlr-foreign-toplevel-management and ext-foreign-toplevel-list clients. Both stay bound where available: ext
// handles carry the exact identifiers the compositor IPC joins on and feed toplevel capture, while the wlr manager
// carries the richer state the rest of the shell consumes.
class ForeignToplevels final : public WaylandExtension {
public:
  using ChangeCallback = std::function<void()>;

  explicit ForeignToplevels(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  void setChangeCallback(ChangeCallback callback);

  bool hasWlrManager() const noexcept { return m_hasWlrManagerGlobal; }
  bool hasExtList() const noexcept { return m_hasExtListGlobal; }

  std::optional<ActiveToplevel> activeToplevel() const;
  std::optional<ActiveToplevel>
  matchToplevelByTitleAndAppId(std::string_view title, std::string_view appId, wl_output* preferredOutput) const;
  wl_output* activeToplevelOutput() const;
  bool hasFullscreenToplevel() const;
  std::vector<std::string> runningAppIds(wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo> windowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo> extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const;
  std::vector<ToplevelInfo> extWindowsWithoutAppId() const;
  ext_foreign_toplevel_handle_v1* extHandleForIdentifier(std::string_view identifier) const;
  bool containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
  void activateToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  [[nodiscard]] bool minimizeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  std::optional<bool> toplevelMinimized(zwlr_foreign_toplevel_handle_v1* handle) const;
  void closeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void setToplevelRectangle(
      zwlr_foreign_toplevel_handle_v1* handle, wl_surface* surface, std::int32_t x, std::int32_t y, std::int32_t width,
      std::int32_t height
  );

  template <typename Fn> void visitExtToplevelHandles(Fn&& fn) const { m_ext.visitExtHandles(std::forward<Fn>(fn)); }
  template <typename Fn> void visitWlrToplevelHandles(Fn&& fn) const { m_wlr.visitWlrHandles(std::forward<Fn>(fn)); }
  template <typename Fn> void visitWlrToplevels(Fn&& fn) const { m_wlr.visitWlrToplevels(std::forward<Fn>(fn)); }

private:
  // On KDE without a wlr manager, app queries read the ext list.
  bool usesExtForAppQueries() const;

  WaylandConnection& m_connection;
  WaylandToplevels m_wlr;
  WaylandExtForeignToplevels m_ext;
  bool m_hasWlrManagerGlobal = false;
  bool m_hasExtListGlobal = false;
};
