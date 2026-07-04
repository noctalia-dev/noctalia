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

  [[nodiscard]] const char* backendName() const override { return "mango-ipc"; }
  [[nodiscard]] bool isAvailable() const noexcept override;
  void setChangeCallback(ChangeCallback callback) override;
  void setOutputNameResolver(WorkspaceOutputNameResolver::Resolver resolver) override;
  bool connectSocket() override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* output) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  void focusWindow(const std::string& windowId) override;
  void cleanup() override;
  void onOutputAdded(wl_output* output) override;
  void onOutputRemoved(wl_output* output) override;
  [[nodiscard]] int pollFd() const noexcept override;
  [[nodiscard]] int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;

  [[nodiscard]] wl_output* ipcSelectedOutput() const;

  [[nodiscard]] std::optional<std::pair<std::string, std::string>> ipcFocusedClientForOutput(wl_output* output) const;

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
  [[nodiscard]] std::string outputName(wl_output* output) const;
  [[nodiscard]] OutputState* activeOutputState();
  [[nodiscard]] const OutputState* activeOutputState() const;
  [[nodiscard]] const OutputState* outputStateFor(wl_output* output) const;
  [[nodiscard]] static std::optional<std::size_t> parseTagIndex(const Workspace& workspace);
  [[nodiscard]] static std::optional<std::size_t> parseTagIndex(const std::string& id);
  [[nodiscard]] std::optional<std::size_t> shellActiveTagIndex(const std::vector<TagInfo>& tags) const;
  [[nodiscard]] static Workspace makeWorkspace(const TagInfo& tag, bool shellActive);
  [[nodiscard]] static std::optional<OutputState> parseMonitor(const nlohmann::json& json);
  [[nodiscard]] static std::optional<ClientState> parseClient(const nlohmann::json& json);

  compositors::mango::MangoRuntime& m_runtime;
  WorkspaceOutputNameResolver::Resolver m_outputNameResolver;
  int m_watchFd = -1;
  std::string m_readBuffer;
  std::vector<wl_output*> m_knownOutputs;
  std::unordered_map<std::string, OutputState> m_outputsByName;
  std::vector<ClientState> m_clients;
  ChangeCallback m_changeCallback;
};
