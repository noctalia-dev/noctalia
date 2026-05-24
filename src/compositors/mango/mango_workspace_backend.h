#pragma once

#include "compositors/output_backend.h"
#include "compositors/workspace_backend.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct zdwl_ipc_output_v2;

class MangoWorkspaceBackend final : public WorkspaceBackend,
                                    public OutputLifecycleObserver,
                                    public DwlIpcWorkspaceProtocolBinder {
public:
  void bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) override;

  [[nodiscard]] const char* backendName() const override { return "dwl-ipc"; }
  [[nodiscard]] bool isAvailable() const noexcept override { return m_manager != nullptr; }
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  void cleanup() override;

  void onOutputAdded(wl_output* output) override;
  void onOutputRemoved(wl_output* output) override;

  void onTagCount(std::uint32_t amount);
  void onLayoutAnnounced(const char* name);
  void onOutputActive(zdwl_ipc_output_v2* handle, std::uint32_t active);
  void onOutputTitle(zdwl_ipc_output_v2* handle, const char* title);
  void onOutputAppId(zdwl_ipc_output_v2* handle, const char* appId);
  void onOutputTag(
      zdwl_ipc_output_v2* handle, std::uint32_t tag, std::uint32_t state, std::uint32_t clients, std::uint32_t focused
  );
  void onOutputFrame(zdwl_ipc_output_v2* handle);

  [[nodiscard]] wl_output* ipcSelectedOutput() const;

  [[nodiscard]] std::optional<std::pair<std::string, std::string>> ipcFocusedClientForOutput(wl_output* output) const;

private:
  struct TagInfo {
    bool active = false;
    bool urgent = false;
    bool occupied = false;
    bool hasFocusedClient = false;
  };

  struct OutputState {
    wl_output* output = nullptr;
    zdwl_ipc_output_v2* handle = nullptr;
    bool active = false;
    bool pendingIpcActive = false;
    bool hasPendingIpcActive = false;
    std::string dwlTitle;
    std::string dwlAppId;
    bool hasPendingTitle = false;
    std::string pendingTitle;
    bool hasPendingAppId = false;
    std::string pendingAppId;
    std::vector<TagInfo> tags;
  };

  void ensureOutputBound(wl_output* output);
  [[nodiscard]] OutputState* activeOutputState();
  [[nodiscard]] const OutputState* preferredOutputState() const;
  [[nodiscard]] static std::optional<std::size_t> parseTagIndex(const Workspace& workspace);
  [[nodiscard]] static std::optional<std::size_t> parseTagIndex(const std::string& id);
  [[nodiscard]] std::size_t protocolIndexForDisplay(std::size_t displayIndex) const;
  [[nodiscard]] static Workspace makeWorkspace(std::size_t index, const TagInfo& tag, bool shellActive);
  [[nodiscard]] std::optional<std::size_t> shellActiveTagIndex(const std::vector<TagInfo>& tags) const;
  [[nodiscard]] std::string summarizeTags(const OutputState& state) const;
  void notifyChanged();

  zdwl_ipc_manager_v2* m_manager = nullptr;
  std::uint32_t m_tagCount = 0;
  std::vector<std::string> m_layouts;
  std::unordered_map<wl_output*, OutputState> m_outputs;
  std::unordered_map<zdwl_ipc_output_v2*, wl_output*> m_outputByHandle;
  ChangeCallback m_changeCallback;
};
