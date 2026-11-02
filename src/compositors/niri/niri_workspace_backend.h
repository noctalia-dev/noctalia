#pragma once

#include "compositors/niri/niri_event_handler.h"
#include "compositors/workspace_backend.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace compositors::niri {
  class NiriRuntime;
} // namespace compositors::niri

class NiriWorkspaceBackend final : public compositors::WorkspaceMetadataBackend,
                                   public compositors::niri::NiriEventHandler {
public:
  explicit NiriWorkspaceBackend(compositors::niri::NiriRuntime& runtime);
  ~NiriWorkspaceBackend() override;

  NiriWorkspaceBackend(const NiriWorkspaceBackend&) = delete;
  NiriWorkspaceBackend& operator=(const NiriWorkspaceBackend&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void setOverviewChangeCallback(ChangeCallback callback) override;
  bool canTrackOverviewState() const noexcept override;
  bool hasOverviewState() const noexcept override { return m_overviewKnown; }
  bool isOverviewOpen() const noexcept override { return m_overviewOpen; }
  int pollFd() const noexcept override;
  short pollEvents() const noexcept override;
  int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void handleEvent(std::string_view key, const nlohmann::json& value) override;
  void handleStreamReset() override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  bool hasExactWindowIdentity() const noexcept override { return true; }
  std::optional<std::string> focusedWindowId() const override;
  bool focusWindowById(const std::string& windowId, bool warpPointer = false) override;
  bool closeWindowById(const std::string& windowId) override;
  void cleanup() override;

private:
  struct WindowState {
    std::optional<std::uint64_t> workspaceId;
    std::string appId;
    std::string title;
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const WindowState&) const = default;
  };

  struct WorkspaceState {
    std::uint64_t id = 0;
    std::uint8_t idx = 0;
    std::string name;
    std::string output;

    bool operator==(const WorkspaceState&) const = default;
  };

  bool handleWorkspacesChanged(const nlohmann::json& payload);
  bool handleWindowsChanged(const nlohmann::json& payload);
  bool handleOverviewChanged(const nlohmann::json& payload);
  bool handleWindowOpenedOrChanged(const nlohmann::json& payload);
  bool handleWindowLayoutsChanged(const nlohmann::json& payload);
  bool handleWindowClosed(const nlohmann::json& payload);
  bool handleWindowFocusChanged(const nlohmann::json& payload);
  bool updateFocusedWindowIdFromWindowsJson(const nlohmann::json& windows);
  static std::optional<WorkspaceState> parseWorkspace(const nlohmann::json& json);
  static std::optional<std::pair<std::uint64_t, WindowState>> parseWindow(const nlohmann::json& json);
  static bool applyWindowFields(const nlohmann::json& json, WindowState& state);
  static bool sameWindowMembership(const WindowState& lhs, const WindowState& rhs) noexcept;
  static bool sameWindowMembership(
      const std::unordered_map<std::uint64_t, WindowState>& lhs,
      const std::unordered_map<std::uint64_t, WindowState>& rhs
  ) noexcept;
  static bool sameWindowLayout(const WindowState& lhs, const WindowState& rhs) noexcept;
  static std::optional<std::uint64_t> parseUnsigned(const std::string& value);
  static std::optional<std::size_t> parseLeadingNumber(const std::string& value);
  static std::string workspaceKey(const WorkspaceState& workspace);
  std::vector<const WorkspaceState*> sortedWorkspaceCandidatesForOutput(const std::string& outputName) const;
  void recomputeOccupancy();
  void notifyChanged() const;
  void notifyOverviewChanged() const;

  std::unordered_map<std::uint64_t, WindowState> m_windows;
  std::unordered_map<std::uint64_t, std::size_t> m_occupancy;
  std::unordered_map<std::uint64_t, WorkspaceState> m_workspaces;
  std::optional<std::uint64_t> m_focusedWindowId;
  bool m_overviewKnown = false;
  bool m_overviewOpen = false;
  ChangeCallback m_changeCallback;
  ChangeCallback m_overviewChangeCallback;
};
