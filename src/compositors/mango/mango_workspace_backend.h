#pragma once

#include "compositors/output_backend.h"
#include "compositors/workspace_backend.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace compositors::mango {
  class MangoRuntime;
}

class MangoWorkspaceBackend final : public WorkspaceBackend,
                                    public OutputLifecycleObserver,
                                    public WorkspaceOutputNameResolver,
                                    public WorkspaceSocketConnector {
public:
  explicit MangoWorkspaceBackend(compositors::mango::MangoRuntime& runtime);

  const char* backendName() const override { return "mango-ipc"; }
  bool isAvailable() const noexcept override;
  void setChangeCallback(ChangeCallback callback) override;
  void setOutputNameResolver(WorkspaceOutputNameResolver::Resolver resolver) override;
  bool connectSocket() override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  std::vector<Workspace> all() const override;
  std::vector<Workspace> forOutput(wl_output* output) const override;
  std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace(wl_output* output) const override;
  std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  void focusWindow(const std::string& windowId) override;
  void cleanup() override;
  void onOutputAdded(wl_output* output) override;
  void onOutputRemoved(wl_output* output) override;
  int pollFd() const noexcept override;
  int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;

  wl_output* ipcSelectedOutput() const;

  std::optional<std::pair<std::string, std::string>> ipcFocusedClientForOutput(wl_output* output) const;

private:
  struct TagInfo {
    std::uint32_t index = 0;
    bool active = false;
    bool urgent = false;
    bool occupied = false;
    bool hasFocusedClient = false;
  };

  struct OutputState {
    std::string name;
    bool active = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string activeClientId;
    std::string activeClientTitle;
    std::string activeClientAppId;
    std::vector<TagInfo> tags;
  };

  struct ClientState {
    std::string id;
    std::string title;
    std::string appId;
    std::string monitorName;
    std::vector<std::uint32_t> tags;
    bool focused = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
  };

  [[nodiscard]] bool openWatchSocket();
  void closeWatchSocket();
  void readWatchSocket();
  bool handleMessage(std::string_view line);
  void refreshClients();
  void syncFocusedClientTags();
  void notifyChanged();
  std::string outputName(wl_output* output) const;
  OutputState* activeOutputState();
  const OutputState* activeOutputState() const;
  const OutputState* outputStateFor(wl_output* output) const;
  static std::optional<std::size_t> parseTagIndex(const Workspace& workspace);
  static std::optional<std::size_t> parseTagIndex(const std::string& id);
  static Workspace makeWorkspace(const TagInfo& tag);
  static std::optional<OutputState> parseMonitor(const nlohmann::json& json);
  static std::optional<ClientState> parseClient(const nlohmann::json& json);

  compositors::mango::MangoRuntime& m_runtime;
  WorkspaceOutputNameResolver::Resolver m_outputNameResolver;
  int m_watchFd = -1;
  std::string m_readBuffer;
  std::vector<wl_output*> m_knownOutputs;
  std::unordered_map<std::string, OutputState> m_outputsByName;
  std::vector<ClientState> m_clients;
  ChangeCallback m_changeCallback;
};
