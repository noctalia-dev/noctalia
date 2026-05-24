#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct hyprland_toplevel_mapping_manager_v1;
struct hyprland_toplevel_window_mapping_handle_v1;
struct zwlr_foreign_toplevel_handle_v1;
struct ext_foreign_toplevel_handle_v1;

namespace compositors::hyprland {

  class HyprlandToplevelMapping {
  public:
    using ChangeCallback = std::function<void()>;

    void initialize(hyprland_toplevel_mapping_manager_v1* manager);
    void cleanup();

    void setChangeCallback(ChangeCallback callback);

    void syncWlrHandles(const std::vector<zwlr_foreign_toplevel_handle_v1*>& handles);
    void syncExtHandles(const std::vector<ext_foreign_toplevel_handle_v1*>& handles);

    [[nodiscard]] std::optional<std::string> windowIdForWlrHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
    [[nodiscard]] std::optional<std::string> windowIdForExtHandle(ext_foreign_toplevel_handle_v1* handle) const;
    [[nodiscard]] zwlr_foreign_toplevel_handle_v1* wlrHandleForWindowId(std::string_view windowId) const;
    [[nodiscard]] ext_foreign_toplevel_handle_v1* extHandleForWindowId(std::string_view windowId) const;
    [[nodiscard]] bool isWindowIdKnown(std::string_view windowId) const;

    [[nodiscard]] bool available() const noexcept { return m_manager != nullptr; }

    static void handleWindowAddress(
        void* data, hyprland_toplevel_window_mapping_handle_v1* request, std::uint32_t addressHi,
        std::uint32_t addressLo
    );
    static void handleFailed(void* data, hyprland_toplevel_window_mapping_handle_v1* request);

  private:
    enum class TargetKind {
      Wlr,
      Ext,
    };

    struct MappingRequest {
      hyprland_toplevel_window_mapping_handle_v1* request = nullptr;
      TargetKind kind = TargetKind::Wlr;
      zwlr_foreign_toplevel_handle_v1* wlrToplevel = nullptr;
      ext_foreign_toplevel_handle_v1* extToplevel = nullptr;
    };

    void requestWlrMapping(zwlr_foreign_toplevel_handle_v1* handle);
    void requestExtMapping(ext_foreign_toplevel_handle_v1* handle);
    void clearRequest(MappingRequest& request);
    void setWlrWindowId(zwlr_foreign_toplevel_handle_v1* handle, std::uint64_t address);
    void setExtWindowId(ext_foreign_toplevel_handle_v1* handle, std::uint64_t address);
    void notifyChanged();

    hyprland_toplevel_mapping_manager_v1* m_manager = nullptr;
    std::unordered_map<zwlr_foreign_toplevel_handle_v1*, std::string> m_windowIdByWlrHandle;
    std::unordered_map<ext_foreign_toplevel_handle_v1*, std::string> m_windowIdByExtHandle;
    std::unordered_map<std::string, zwlr_foreign_toplevel_handle_v1*> m_wlrHandleByWindowId;
    std::unordered_map<std::string, ext_foreign_toplevel_handle_v1*> m_extHandleByWindowId;
    std::unordered_map<hyprland_toplevel_window_mapping_handle_v1*, MappingRequest> m_pending;
    ChangeCallback m_changeCallback;
  };

} // namespace compositors::hyprland
