#pragma once

#include "compositors/umbriel/umbriel_event_handler.h"
#include "compositors/workspace_backend.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace compositors::umbriel {
  class UmbrielRuntime;
} // namespace compositors::umbriel

// Workspace metadata from the Umbriel IPC windows event, joined to the ext
// protocol lists the platform already owns. Workspace order and names come from
// the ext-workspace list (apply refreshes the per-output cache); window rows
// come from the IPC stream. Umbriel workspace ids are "<output>:<serial>", the
// same string in both surfaces, so the cache can key windows to workspaces
// without a workspace IPC event family.
class UmbrielWorkspaceBackend final : public compositors::WorkspaceMetadataBackend,
                                      public compositors::umbriel::UmbrielEventHandler {
public:
  explicit UmbrielWorkspaceBackend(compositors::umbriel::UmbrielRuntime& runtime);
  ~UmbrielWorkspaceBackend() override;

  UmbrielWorkspaceBackend(const UmbrielWorkspaceBackend&) = delete;
  UmbrielWorkspaceBackend& operator=(const UmbrielWorkspaceBackend&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void setOverviewChangeCallback(ChangeCallback callback) override;
  [[nodiscard]] bool canTrackOverviewState() const noexcept override;
  [[nodiscard]] bool hasOverviewState() const noexcept override { return m_overviewKnown; }
  [[nodiscard]] bool isOverviewOpen() const noexcept override { return m_overviewOpen; }
  [[nodiscard]] int pollFd() const noexcept override;
  [[nodiscard]] short pollEvents() const noexcept override;
  [[nodiscard]] int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void handleEvent(std::string_view event, const nlohmann::json& data) override;
  void handleStreamReset() override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  [[nodiscard]] bool hasExactWindowIdentity() const noexcept override { return true; }
  [[nodiscard]] std::optional<std::string> focusedWindowId() const override;
  bool focusWindowById(const std::string& windowId) override;
  bool closeWindowById(const std::string& windowId) override;
  void cleanup() override;

private:
  struct Window {
    std::string workspaceId;
    std::string appId;
    std::string title;
    bool active = false;
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const Window&) const = default;
  };

  // Workspaces as observed through the ext-workspace list, cached per output in
  // list order as (id, name). The ""-key entry holds the unqualified list from
  // the all-outputs apply path. Mutable: apply() is const on the interface but
  // refreshes this cache.
  using WorkspaceList = std::vector<std::pair<std::string, std::string>>;
  mutable std::vector<std::pair<std::string, WorkspaceList>> m_workspacesByOutput;
  // IPC window snapshot in wire order: (window id, window).
  std::vector<std::pair<std::string, Window>> m_windows;
  std::optional<std::string> m_focusedWindowId;
  bool m_overviewKnown = false;
  bool m_overviewOpen = false;
  ChangeCallback m_changeCallback;
  ChangeCallback m_overviewChangeCallback;

  [[nodiscard]] const WorkspaceList* cachedWorkspaces(const std::string& outputName) const;
  [[nodiscard]] std::string workspaceKeyForId(const std::string& workspaceId) const;
  [[nodiscard]] static std::string workspaceKey(const std::string& id, const std::string& name);
  void notifyChanged() const;
  void notifyOverviewChanged() const;
};
