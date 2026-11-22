#pragma once

#include "core/files/file_watcher.h"
#include "shell/desktop/desktop_widget_services.h"
#include "shell/desktop/editor/widget_editor_document.h"
#include "shell/dialogs/layer_popup_host.h"
#include "shell/greeter/greeter_config_draft.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

class DesktopWidgetsEditor;
class IpcService;
class WaylandConnection;
struct PointerEvent;
struct KeyboardEvent;
struct LayerPopupParentContext;
struct wl_surface;

class GreeterEditor final : public WidgetEditorDocument {
public:
  GreeterEditor();
  ~GreeterEditor() override;
  void initialize(const DesktopWidgetServices& services);
  void setLifecycleCallbacks(std::function<bool()> enter, std::function<void()> exit);
  void setApplySavedCallback(std::function<void()> callback) { m_applySaved = std::move(callback); }
  void setApplyStatus(std::string message, bool pending = false);
  void registerIpc(IpcService& ipc);
  void open();
  void close();
  bool isOpen() const;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onOutputChange();
  void onSecondTick();
  bool wantsSecondTicks() const;
  void requestUpdate();
  void requestLayout();
  std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;

  std::vector<DesktopWidgetState>& items() override { return m_items; }
  void flush() override;
  void measured(const std::string& id, float width, float height) override;
  void activateOutput(const std::string& name) override;
  std::string activeOutput() const override { return m_activeOutput; }
  DesktopWidgetState presentation(const DesktopWidgetState& item, const WaylandOutput& output) const override;
  bool visibleOnOutput(const DesktopWidgetState& item, const WaylandOutput& output) const override;
  bool isCore(const std::string& id) const override;
  bool isRequired(const std::string& id) const override;
  std::vector<desktop_settings::DesktopWidgetTypeOption> typeOptions() const override;
  std::unique_ptr<DesktopWidget> createPreview(const DesktopWidgetState& item) const override;
  std::vector<settings::WidgetSettingSpec> settingSpecs(const DesktopWidgetState& item, bool background) const override;
  std::string itemLabel(const DesktopWidgetState& item) const override;
  void buildInspector(Flex& content, DesktopWidgetsEditor& editor, const DesktopWidgetState& item) override;
  bool applyProperty(const std::string& id, const std::string& key, const WidgetSettingValue& value) override;
  void resetItem(const std::string& id) override;
  void showCore(const std::string& id) override;
  void hideCore(const std::string& id) override;
  void cancel() override { close(); }
  void buildOverlay(
      Node& root, Renderer& renderer, DesktopWidgetsEditor& editor, const std::string& outputName, float width,
      float height
  ) override;

private:
  GreeterProfile& profile();
  const GreeterProfile& profile() const;
  std::vector<GreeterWidgetConfig> effectiveWidgets() const;
  std::vector<GreeterWidgetConfig>& customWidgets();
  const WaylandOutput* currentOutput() const;
  void rebuildItems();
  bool minimal() const;
  std::pair<float, float> coreSize(const std::string& id) const;
  GreeterAnchor coreAnchor(const std::string& id, float width, float height) const;
  bool save();
  void refreshBacking();

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  FileWatcher* m_fileWatcher = nullptr;
  FileWatcher::WatchId m_profileWatch = 0;
  std::unique_ptr<DesktopWidgetsEditor> m_editor;
  std::unique_ptr<GreeterConfigDraft> m_draft;
  std::vector<DesktopWidgetState> m_items;
  std::vector<DesktopWidgetState> m_previous;
  std::unordered_map<std::string, std::pair<float, float>> m_naturalSizes;
  std::string m_activeOutput;
  float m_referenceWidth = 2560;
  float m_referenceHeight = 1440;
  bool m_compactSample = false;
  bool m_conflict = false;
  std::string m_status;
  std::string m_applyStatus;
  bool m_applyPending = false;
  std::function<void()> m_applySaved;
  std::string m_userName;
  std::string m_displayName;
  std::function<bool()> m_onEnter;
  std::function<void()> m_onExit;
};
