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
  [[nodiscard]] bool canTrackOverviewState() const noexcept override;
  [[nodiscard]] bool hasOverviewState() const noexcept override { return m_overviewKnown; }
  [[nodiscard]] bool isOverviewOpen() const noexcept override { return m_overviewOpen; }
  [[nodiscard]] int pollFd() const noexcept override;
  [[nodiscard]] short pollEvents() const noexcept override;
  [[nodiscard]] int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void handleEvent(std::string_view key, const nlohmann::json& value) override;
  void handleStreamReset() override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  bool focusWindowById(const std::string& windowId) override;
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

  [[nodiscard]] bool handleWorkspacesChanged(const nlohmann::json& payload);
  [[nodiscard]] bool handleWindowsChanged(const nlohmann::json& payload);
  [[nodiscard]] bool handleOverviewChanged(const nlohmann::json& payload);
  [[nodiscard]] bool handleWindowOpenedOrChanged(const nlohmann::json& payload);
  [[nodiscard]] bool handleWindowLayoutsChanged(const nlohmann::json& payload);
  [[nodiscard]] bool handleWindowClosed(const nlohmann::json& payload);
  [[nodiscard]] static std::optional<WorkspaceState> parseWorkspace(const nlohmann::json& json);
  [[nodiscard]] static std::optional<std::pair<std::uint64_t, WindowState>> parseWindow(const nlohmann::json& json);
  [[nodiscard]] static bool applyWindowFields(const nlohmann::json& json, WindowState& state);
  [[nodiscard]] static bool sameWindowMembership(const WindowState& lhs, const WindowState& rhs) noexcept;
  [[nodiscard]] static bool sameWindowMembership(
      const std::unordered_map<std::uint64_t, WindowState>& lhs,
      const std::unordered_map<std::uint64_t, WindowState>& rhs
  ) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t> parseUnsigned(const std::string& value);
  [[nodiscard]] static std::optional<std::size_t> parseLeadingNumber(const std::string& value);
  [[nodiscard]] static std::string workspaceKey(const WorkspaceState& workspace);
  [[nodiscard]] std::vector<const WorkspaceState*>
  sortedWorkspaceCandidatesForOutput(const std::string& outputName) const;
  void recomputeOccupancy();
  void notifyChanged() const;
  void notifyOverviewChanged() const;

  std::unordered_map<std::uint64_t, WindowState> m_windows;
  std::unordered_map<std::uint64_t, std::size_t> m_occupancy;
  std::unordered_map<std::uint64_t, WorkspaceState> m_workspaces;
  bool m_overviewKnown = false;
  bool m_overviewOpen = false;
  ChangeCallback m_changeCallback;
  ChangeCallback m_overviewChangeCallback;
};
