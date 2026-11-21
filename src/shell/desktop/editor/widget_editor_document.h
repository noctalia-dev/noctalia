#pragma once

#include "config/config_types.h"
#include "shell/desktop/desktop_widget_settings_registry.h"

#include <memory>
#include <string>
#include <vector>

class DesktopWidget;
class DesktopWidgetsEditor;
class Flex;
class Node;
class Renderer;
struct WaylandOutput;

// Presentation records belong to the editor. Each document maps them to its own saved schema.
class WidgetEditorDocument {
public:
  virtual ~WidgetEditorDocument() = default;
  virtual std::vector<DesktopWidgetState>& items() = 0;
  virtual void flush() = 0;
  virtual void measured(const std::string& id, float width, float height) = 0;
  virtual void activateOutput(const std::string& name) = 0;
  virtual std::string activeOutput() const = 0;
  virtual DesktopWidgetState presentation(const DesktopWidgetState& item, const WaylandOutput& output) const = 0;
  virtual bool visibleOnOutput(const DesktopWidgetState& item, const WaylandOutput& output) const = 0;
  virtual bool isCore(const std::string& id) const = 0;
  virtual bool isRequired(const std::string& id) const = 0;
  virtual std::vector<desktop_settings::DesktopWidgetTypeOption> typeOptions() const = 0;
  virtual std::unique_ptr<DesktopWidget> createPreview(const DesktopWidgetState& item) const = 0;
  virtual std::vector<settings::WidgetSettingSpec>
  settingSpecs(const DesktopWidgetState& item, bool background) const = 0;
  virtual std::string itemLabel(const DesktopWidgetState& item) const = 0;
  virtual void buildInspector(Flex& content, DesktopWidgetsEditor& editor, const DesktopWidgetState& item) = 0;
  virtual bool applyProperty(const std::string& id, const std::string& key, const WidgetSettingValue& value) = 0;
  virtual void resetItem(const std::string& id) = 0;
  virtual void showCore(const std::string& id) = 0;
  virtual void hideCore(const std::string& id) = 0;
  virtual void cancel() = 0;
  virtual void buildOverlay(
      Node& root, Renderer& renderer, DesktopWidgetsEditor& editor, const std::string& outputName, float width,
      float height
  ) = 0;
};
