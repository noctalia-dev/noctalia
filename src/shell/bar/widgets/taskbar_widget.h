#pragma once

#include "compositors/compositor_platform.h"
#include "shell/bar/widget.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ContextMenuPopup;
class ConfigService;
class Flex;
class InputArea;
struct wl_output;
struct zwlr_foreign_toplevel_handle_v1;

enum class WorkspaceLabelPlacement {
  Corner,
  Centered,
  Inside,
};

enum class WorkspaceGroupContent {
  Icons,
  Count,
  Dots,
};

struct TaskbarWidgetOptions {
  bool groupByWorkspace = false;
  bool showAllOutputs = false;
  bool onlyActiveWorkspace = false;
  bool showWorkspaceLabel = true;
  WorkspaceLabelPlacement workspaceLabelPlacement = WorkspaceLabelPlacement::Corner;
  WorkspaceGroupContent workspaceGroupContent = WorkspaceGroupContent::Icons;
  bool hideEmptyWorkspaces = false;
  bool workspaceGroupCapsule = true;
  bool focusedOutputOnly = false;
  bool minimal = false;
  bool groupSingleIconPerApp = false;
  bool enableScroll = true;
  bool showActiveIndicator = true;
  float activeOpacity = 1.0f;
  float inactiveOpacity = 1.0f;
  float pinnedOpacity = 0.5f;
  ColorSpec focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec emptyColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec urgentColor = colorSpecFromRole(ColorRole::Error);
  bool showWindowTitle = false;
  float windowTitleMaxWidth = 100.0f;
  float taskbarMaxWidth = 8192.0f;
  std::string barPosition;
  std::string barName;
  // Config key for [widget.<name>] overrides (pin list lives here).
  std::string widgetName;
};

class TaskbarWidget : public Widget {
public:
  TaskbarWidget(CompositorPlatform& platform, ConfigService& config, wl_output* output, TaskbarWidgetOptions options);
  ~TaskbarWidget() override;

  void create() override;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event) override;
  [[nodiscard]] bool wantsBarHoverHighlight() const noexcept override { return false; }
  [[nodiscard]] bool reservesMiddleClick(float sceneX, float sceneY) const noexcept override;

private:
  struct TaskModel {
    std::uintptr_t handleKey = 0;
    std::uint64_t order = 0;
    std::string appId;
    std::string idLower;
    std::string startupWmClassLower;
    std::string nameLower;
    std::string appIdLower;
    std::string title;
    std::string iconPath;
    std::string workspaceKey;
    std::string workspaceWindowId;
    // Desktop entry id used for pin persistence / launch (empty for unmatched windows).
    std::string desktopEntryId;
    std::uint64_t workspaceOrder = std::numeric_limits<std::uint64_t>::max();
    std::size_t instanceCount = 1;
    bool active = false;
    // Pinned flat-strip slot (empty when not running). Ignored while group_by_workspace is on.
    bool pinned = false;
    bool running = true;
    zwlr_foreign_toplevel_handle_v1* firstHandle = nullptr;
  };

  struct WorkspaceModel {
    Workspace workspace;
    std::string key;
    std::string label;
    wl_output* hostOutput = nullptr;
  };

  struct PendingWorkspaceTransition {
    std::string targetWorkspaceKey;
    std::uint8_t votes = 0;
  };

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;

  void rebuild(Renderer& renderer);
  void clearChildren(Flex* flex) const;
  void buildTaskButtons(Renderer& renderer);
  void updateModels();
  void syncWorkspaceGroupingCapability();
  [[nodiscard]] static std::string toLower(std::string value);
  [[nodiscard]] static std::string workspaceLabel(const Workspace& workspace, std::size_t index);
  [[nodiscard]] bool
  modelsEqual(const std::vector<TaskModel>& tasks, const std::vector<WorkspaceModel>& workspaces) const;
  void buildDesktopIconIndex();
  [[nodiscard]] std::string resolveIconPath(const std::string& appId, const std::string& iconNameOrPath);
  void openTaskContextMenu(const TaskModel& task, InputArea& area);
  void activateAdjacentWorkspace(int direction);
  void activateAdjacentTask(int direction);
  [[nodiscard]] bool activeWorkspaceIndex(std::size_t& index) const;
  [[nodiscard]] const std::vector<WorkspaceModel>& navigationWorkspaces() const noexcept;
  [[nodiscard]] wl_output* toplevelOutputFilter() const noexcept;
  [[nodiscard]] bool useMultiOutputWorkspaceKeys() const noexcept;
  [[nodiscard]] std::string workspaceKeyPrefixForOutput(wl_output* out) const;
  [[nodiscard]] wl_output* workspaceHostOutput(const WorkspaceModel& model) const noexcept;
  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] bool isFocusedOutput() const;
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static bool taskInWorkspaceGroup(const TaskModel& task, const WorkspaceModel& ws);
  void activateTaskModel(const TaskModel& task);
  void closeTaskModel(const TaskModel& task);
  void applyPinnedMerge(std::vector<TaskModel>& tasks);
  void activateOrLaunchPinned(const TaskModel& task);
  void launchDesktopEntry(const TaskModel& task);
  [[nodiscard]] std::vector<std::string> pinnedConfigIds() const;
  [[nodiscard]] static bool taskMatchesDesktopEntry(const TaskModel& task, const DesktopEntry& entry);
  void setEntryPinned(const DesktopEntry& entry, bool pinned);
  [[nodiscard]] std::optional<DesktopEntry> desktopEntryForTask(const TaskModel& task) const;

  CompositorPlatform& m_platform;
  ConfigService& m_configService;
  wl_output* m_output = nullptr;
  TaskbarWidgetOptions m_configOptions;
  bool m_groupByWorkspace = false;
  bool m_showAllOutputs = false;
  bool m_onlyActiveWorkspace = false;
  bool m_showWorkspaceLabel = true;
  WorkspaceLabelPlacement m_workspaceLabelPlacement = WorkspaceLabelPlacement::Corner;
  WorkspaceGroupContent m_workspaceGroupContent = WorkspaceGroupContent::Icons;
  bool m_hideEmptyWorkspaces = false;
  bool m_workspaceGroupCapsule = true;
  bool m_focusedOutputOnly = false;
  bool m_wasFocusedOutput = true;
  bool m_activeUsesFocusedColor = true;
  bool m_minimal = false;
  bool m_groupSingleIconPerApp = false;
  bool m_enableScroll = true;
  bool m_showActiveIndicator = true;
  float m_activeOpacity = 1.0f;
  float m_inactiveOpacity = 1.0f;
  float m_pinnedOpacity = 0.5f;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_urgentColor = colorSpecFromRole(ColorRole::Error);
  bool m_showWindowTitle = false;
  float m_windowTitleMaxWidth = 100.0;
  float m_taskbarMaxWidth = 8192.0;
  std::string m_barPosition;
  std::string m_barName;
  std::string m_widgetName;
  bool m_rebuildPending = true;
  bool m_vertical = false;
  float m_containerWidth = 0.0f;
  float m_containerHeight = 0.0f;
  std::uint64_t m_textMetricsGeneration = 0;

  Flex* m_root = nullptr;
  Flex* m_taskStrip = nullptr;

  std::vector<TaskModel> m_tasks;
  std::vector<WorkspaceModel> m_workspaces;
  // Full workspace list before "hide empty" filtering; used for scroll navigation.
  std::vector<WorkspaceModel> m_allWorkspaces;
  std::unordered_map<std::uintptr_t, PendingWorkspaceTransition> m_pendingWorkspaceTransitions;
  std::unordered_map<std::string, std::size_t> m_groupedAppCycleCursor;
  std::unordered_map<std::string, std::string> m_appIconsByLower;
  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  std::vector<zwlr_foreign_toplevel_handle_v1*> m_contextMenuHandles;
  zwlr_foreign_toplevel_handle_v1* m_contextMenuPrimaryHandle = nullptr;
  // KDE has no wlr foreign-toplevel handles; close targets use title/appId/uuid instead.
  std::vector<ToplevelInfo> m_contextMenuKdeWindows;
  ToplevelInfo m_contextMenuKdePrimary;
  std::uint64_t m_desktopEntriesVersion = 0;
  IconResolver m_iconResolver;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
