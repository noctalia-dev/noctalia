#pragma once

#include "compositors/workspace_backend.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class WorkspaceAlertService {
public:
  [[nodiscard]] bool add(std::string_view workspaceKey);
  [[nodiscard]] bool clear(std::string_view workspaceKey);
  void clearAll();

  [[nodiscard]] bool contains(std::string_view workspaceKey) const;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::vector<std::string> keys() const;

  void applyOverlay(std::vector<Workspace>& workspaces) const;
  [[nodiscard]] std::size_t clearActive(const std::vector<Workspace>& workspaces);

  [[nodiscard]] static bool isKnownWorkspaceKey(
      std::string_view workspaceKey, const std::vector<Workspace>& workspaces
  );
  [[nodiscard]] static std::optional<std::string> workspaceKeyForWindow(
      std::string_view windowId, const std::vector<WorkspaceWindowAssignment>& assignments
  );

private:
  std::set<std::string, std::less<>> m_alerts;
};
