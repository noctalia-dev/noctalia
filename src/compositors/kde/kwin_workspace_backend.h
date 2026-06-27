#pragma once

#include "compositors/workspace_backend.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct org_kde_plasma_virtual_desktop;
struct org_kde_plasma_virtual_desktop_management;

class KwinWorkspaceBackend final : public WorkspaceBackend,
                                   public WorkspaceOutputNameResolver,
                                   public KdeVirtualDesktopProtocolBinder {
public:
  using OutputNameResolver = WorkspaceOutputNameResolver::Resolver;

  void bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management) override;
  void setOutputNameResolver(OutputNameResolver resolver) override;

  [[nodiscard]] const char* backendName() const override { return "kwin-plasma-vd"; }
  [[nodiscard]] bool isAvailable() const noexcept override { return m_management != nullptr; }
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  void cleanup() override;

  void onDesktopCreated(const char* desktopId, std::uint32_t position);
  void onDesktopRemoved(const char* desktopId);
  void onManagementDone();
  void onManagementRows(std::uint32_t rows);
  void onDesktopIdChanged(org_kde_plasma_virtual_desktop* desktop, const char* id);
  void onDesktopNameChanged(org_kde_plasma_virtual_desktop* desktop, const char* name);
  void onDesktopActivated(org_kde_plasma_virtual_desktop* desktop);
  void onDesktopDeactivated(org_kde_plasma_virtual_desktop* desktop);
  void onDesktopPositionChanged(org_kde_plasma_virtual_desktop* desktop, std::uint32_t position);
  void onDesktopOutputEntered(org_kde_plasma_virtual_desktop* desktop, const char* outputName);
  void onDesktopDone(org_kde_plasma_virtual_desktop* desktop);
  void onDesktopRemovedHandle(org_kde_plasma_virtual_desktop* desktop);

private:
  struct DesktopState {
    org_kde_plasma_virtual_desktop* handle = nullptr;
    std::string id;
    std::string name;
    std::uint32_t position = 0;
    bool globallyActive = false;
    std::vector<std::string> activeOutputs;
  };

  void requestDesktopHandle(const std::string& desktopId, std::uint32_t position);
  void removeDesktopById(const std::string& desktopId);
  [[nodiscard]] DesktopState* findDesktop(org_kde_plasma_virtual_desktop* desktop);
  [[nodiscard]] const DesktopState* findDesktop(org_kde_plasma_virtual_desktop* desktop) const;
  [[nodiscard]] DesktopState* findDesktopById(const std::string& id);
  [[nodiscard]] const DesktopState* findDesktopById(const std::string& id) const;
  [[nodiscard]] Workspace toWorkspace(const DesktopState& desktop, const std::string& outputName) const;
  [[nodiscard]] std::uint32_t gridColumns() const;
  void notifyChanged() const;

  org_kde_plasma_virtual_desktop_management* m_management = nullptr;
  std::uint32_t m_managementVersion = 0;
  std::uint32_t m_rows = 1;
  std::unordered_map<org_kde_plasma_virtual_desktop*, DesktopState> m_desktops;
  OutputNameResolver m_outputNameResolver;
  ChangeCallback m_changeCallback;
};
