#pragma once

#include "compositors/compositor_platform.h"
#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ContextMenuPopup;
class Flex;
class InputArea;
struct wl_output;
struct zwlr_foreign_toplevel_handle_v1;
struct PointerEvent;

enum class WorkspaceLabelPlacement {
  Corner,
  Centered,
  Inside,
};

class TaskbarWidget : public Widget {
public:
  TaskbarWidget(
      CompositorPlatform& platform, wl_output* output, bool groupByWorkspace, bool showAllOutputs,
      bool onlyActiveWorkspace, bool showWorkspaceLabel, WorkspaceLabelPlacement workspaceLabelPlacement,
      bool hideEmptyWorkspaces, bool workspaceGroupCapsule, ColorSpec focusedColor, ColorSpec occupiedColor,
      ColorSpec emptyColor, std::string barPosition, ShellConfig::ShadowConfig shadowConfig
  );
  ~TaskbarWidget() override;

  void create() override;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event) override;

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
    std::uint64_t workspaceOrder = std::numeric_limits<std::uint64_t>::max();
    bool active = false;
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
  [[nodiscard]] static std::string toLower(std::string value);
  [[nodiscard]] static std::string workspaceLabel(const Workspace& workspace, std::size_t index);
  [[nodiscard]] bool
  modelsEqual(const std::vector<TaskModel>& tasks, const std::vector<WorkspaceModel>& workspaces) const;
  void buildDesktopIconIndex();
  [[nodiscard]] std::string resolveIconPath(const std::string& appId, const std::string& iconNameOrPath);
  void openTaskContextMenu(const TaskModel& task, InputArea& area);
  void activateAdjacentWorkspace(int direction);
  [[nodiscard]] bool activeWorkspaceIndex(std::size_t& index) const;
  [[nodiscard]] wl_output* toplevelOutputFilter() const noexcept;
  [[nodiscard]] bool useMultiOutputWorkspaceKeys() const noexcept;
  [[nodiscard]] std::string workspaceKeyPrefixForOutput(wl_output* out) const;
  [[nodiscard]] wl_output* workspaceHostOutput(const WorkspaceModel& model) const noexcept;
  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static bool taskInWorkspaceGroup(const TaskModel& task, const WorkspaceModel& ws);

  CompositorPlatform& m_platform;
  wl_output* m_output = nullptr;
  bool m_groupByWorkspace = false;
  bool m_showAllOutputs = false;
  bool m_onlyActiveWorkspace = false;
  bool m_showWorkspaceLabel = true;
  WorkspaceLabelPlacement m_workspaceLabelPlacement = WorkspaceLabelPlacement::Corner;
  bool m_hideEmptyWorkspaces = false;
  bool m_workspaceGroupCapsule = true;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
  std::string m_barPosition;
  ShellConfig::ShadowConfig m_shadowConfig;
  bool m_rebuildPending = true;
  bool m_vertical = false;
  std::uint64_t m_textMetricsGeneration = 0;

  Flex* m_root = nullptr;
  Flex* m_taskStrip = nullptr;

  std::vector<TaskModel> m_tasks;
  std::vector<WorkspaceModel> m_workspaces;
  std::unordered_map<std::uintptr_t, PendingWorkspaceTransition> m_pendingWorkspaceTransitions;
  std::unordered_map<std::string, std::string> m_appIconsByLower;
  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  std::vector<zwlr_foreign_toplevel_handle_v1*> m_contextMenuHandles;
  zwlr_foreign_toplevel_handle_v1* m_contextMenuPrimaryHandle = nullptr;
  std::uint64_t m_desktopEntriesVersion = 0;
  IconResolver m_iconResolver;
};
