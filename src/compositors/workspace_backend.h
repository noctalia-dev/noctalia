#pragma once

#include <cstdint>
#include <functional>
#include <poll.h>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_output;
struct ext_workspace_manager_v1;
struct org_kde_plasma_virtual_desktop_management;
struct zdwl_ipc_manager_v2;

struct Workspace {
  std::string id;
  std::string name;
  std::vector<std::uint32_t> coordinates;
  std::uint32_t index = 0;
  bool active = false;
  bool urgent = false;
  bool occupied = false;
};

struct WorkspaceWindow {
  std::string windowId;
  std::string workspaceKey;
  std::string appId;
  std::string title;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::string outputName;
};

struct WorkspaceWindowAssignment {
  std::string windowId;
  std::string workspaceKey;
  std::string appId;
  std::string title;
  std::int32_t x = 0;
  std::int32_t y = 0;
};

struct TaskbarWindowCandidate {
  std::uintptr_t handleKey = 0;
  std::vector<std::string> appIds;
  std::string title;
};

enum class TaskbarAssignmentMode {
  Generic,
  WorkspaceOccurrenceTitle,
};

class WorkspaceBackend {
public:
  using ChangeCallback = std::function<void()>;

  virtual ~WorkspaceBackend() = default;

  [[nodiscard]] virtual const char* backendName() const = 0;
  [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
  virtual void setChangeCallback(ChangeCallback callback) = 0;
  virtual void activate(const std::string& id) = 0;
  virtual void activateForOutput(wl_output* output, const std::string& id) = 0;
  virtual void activateForOutput(wl_output* output, const Workspace& workspace) = 0;
  [[nodiscard]] virtual std::vector<Workspace> all() const = 0;
  [[nodiscard]] virtual std::vector<Workspace> forOutput(wl_output* output) const = 0;
  [[nodiscard]] virtual std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* /*output*/) const {
    return {};
  }
  [[nodiscard]] virtual TaskbarAssignmentMode taskbarAssignmentMode() const noexcept {
    return TaskbarAssignmentMode::Generic;
  }
  [[nodiscard]] virtual std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& /*windows*/, wl_output* /*output*/) const {
    return {};
  }
  [[nodiscard]] virtual std::vector<WorkspaceWindow> workspaceWindows(wl_output* /*output*/) const { return {}; }
  virtual void focusWindow(const std::string& /*windowId*/) {}
  virtual void cleanup() = 0;

  [[nodiscard]] virtual int pollFd() const noexcept { return -1; }
  [[nodiscard]] virtual short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
  [[nodiscard]] virtual int pollTimeoutMs() const noexcept { return -1; }
  virtual void dispatchPoll(short /*revents*/) {}
};

class ExtWorkspaceProtocolBinder {
public:
  virtual ~ExtWorkspaceProtocolBinder() = default;
  virtual void bindExtWorkspace(ext_workspace_manager_v1* manager) = 0;
};

class DwlIpcWorkspaceProtocolBinder {
public:
  virtual ~DwlIpcWorkspaceProtocolBinder() = default;
  virtual void bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) = 0;
};

class KdeVirtualDesktopProtocolBinder {
public:
  virtual ~KdeVirtualDesktopProtocolBinder() = default;
  virtual void bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management) = 0;
};

class WorkspaceOutputNameResolver {
public:
  using Resolver = std::function<std::string(wl_output*)>;

  virtual ~WorkspaceOutputNameResolver() = default;
  virtual void setOutputNameResolver(Resolver resolver) = 0;
};

class WorkspaceSocketConnector {
public:
  virtual ~WorkspaceSocketConnector() = default;
  [[nodiscard]] virtual bool connectSocket() = 0;
};

namespace compositors {

  class WorkspaceMetadataBackend {
  public:
    using ChangeCallback = std::function<void()>;

    virtual ~WorkspaceMetadataBackend() = default;
    virtual void setChangeCallback(ChangeCallback callback) = 0;
    virtual void setOverviewChangeCallback(ChangeCallback callback) { (void)callback; }
    [[nodiscard]] virtual int pollFd() const noexcept { return -1; }
    [[nodiscard]] virtual short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
    [[nodiscard]] virtual int pollTimeoutMs() const noexcept { return -1; }
    virtual void dispatchPoll(short /*revents*/) {}
    virtual void apply(std::vector<Workspace>& /*workspaces*/, const std::string& /*outputName*/ = {}) const {}
    [[nodiscard]] virtual std::vector<std::string> workspaceKeys(const std::string& /*outputName*/ = {}) const {
      return {};
    }
    [[nodiscard]] virtual std::unordered_map<std::string, std::vector<std::string>>
    appIdsByWorkspace(const std::string& /*outputName*/ = {}) const {
      return {};
    }
    [[nodiscard]] virtual std::vector<WorkspaceWindow> workspaceWindows(const std::string& /*outputName*/ = {}) const {
      return {};
    }
    // Focus a window by its compositor-specific id. Returns true if the backend
    // handled the request (so the caller can skip other focus paths). Named
    // distinctly from WorkspaceBackend::focusWindow so backends that implement
    // both interfaces don't hit a conflicting-return-type override.
    virtual bool focusWindowById(const std::string& /*windowId*/) { return false; }
    [[nodiscard]] virtual bool canTrackOverviewState() const noexcept { return false; }
    [[nodiscard]] virtual bool hasOverviewState() const noexcept { return false; }
    [[nodiscard]] virtual bool isOverviewOpen() const noexcept { return true; }
    virtual void cleanup() = 0;
  };

} // namespace compositors
