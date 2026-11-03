#include "shell/plugin_layer/plugin_layer_host.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_runtime_context.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-layer");

  bool sameDefinition(const PluginLayerHost::Definition& a, const PluginLayerHost::Definition& b) {
    return a.entryId == b.entryId
        && a.sourcePath == b.sourcePath
        && a.pluginDir == b.pluginDir
        && a.config == b.config
        && a.visible == b.visible
        && scripting::settingsEqual(a.settings, b.settings);
  }

} // namespace

PluginLayerHost::PluginLayerHost(
    PluginLayerServices services, scripting::ScriptApiContext& scriptApi, FileWatcher* fileWatcher,
    HttpClient* httpClient, ClipboardService* clipboard
)
    : m_services(services), m_scriptApi(scriptApi), m_fileWatcher(fileWatcher), m_httpClient(httpClient),
      m_clipboard(clipboard) {}

PluginLayerHost::~PluginLayerHost() = default;

void PluginLayerHost::reload(std::vector<Definition> definitions) {
  std::vector<std::unique_ptr<Entry>> next;
  std::vector<Entry*> created;
  next.reserve(definitions.size());
  for (auto& definition : definitions) {
    const auto existing = std::ranges::find_if(m_entries, [&](const auto& entry) {
      return entry != nullptr && sameDefinition(entry->definition, definition);
    });
    if (existing != m_entries.end()) {
      next.push_back(std::move(*existing));
      continue;
    }
    auto entry = std::make_unique<Entry>();
    entry->config = definition.config;
    entry->definition = std::move(definition);
    created.push_back(entry.get());
    next.push_back(std::move(entry));
  }
  // Entries left behind were removed or changed; their instances die with them.
  m_entries = std::move(next);
  for (Entry* entry : created) {
    if (entry->definition.visible) {
      show(*entry);
    }
  }
}

void PluginLayerHost::command(
    scripting::PluginLayerCommand command, const std::string& entryId, const scripting::PluginLayerConfigPatch& patch
) {
  Entry* entry = findEntry(entryId);
  if (entry == nullptr) {
    kLog.warn("'{}' is not a [[layer]] entry of an enabled plugin", entryId);
    return;
  }
  switch (command) {
  case scripting::PluginLayerCommand::Show:
    show(*entry);
    break;
  case scripting::PluginLayerCommand::Hide:
    hide(*entry);
    break;
  case scripting::PluginLayerCommand::Configure: {
    const std::string previousOutput = entry->config.output;
    scripting::applyPluginLayerPatch(entry->config, patch);
    if (!entry->shown) {
      break;
    }
    if (entry->config.output != previousOutput) {
      entry->focusedOutput = m_services.platform.preferredInteractiveOutput();
      std::erase_if(entry->instances, [&](const auto& instance) {
        const WaylandOutput* output = m_services.platform.findOutputByWl(instance->output());
        return output == nullptr || !targetsOutput(*entry, *output);
      });
    }
    for (auto& instance : entry->instances) {
      instance->applyConfig(entry->config);
    }
    syncInstances(*entry);
    break;
  }
  }
}

void PluginLayerHost::onOutputChange() {
  for (auto& entry : m_entries) {
    if (entry->shown) {
      syncInstances(*entry);
    }
  }
}

void PluginLayerHost::onConfigReload() {
  for (auto& entry : m_entries) {
    for (auto& instance : entry->instances) {
      instance->requestLayout();
    }
  }
}

bool PluginLayerHost::onPointerEvent(const PointerEvent& event) {
  PluginLayer* instance = findInstance(event.surface);
  if (instance == nullptr && event.surface == nullptr) {
    // Motion and axis events carry no surface on some compositors; route to the entered layer.
    for (auto& entry : m_entries) {
      for (auto& candidate : entry->instances) {
        if (candidate->pointerInside()) {
          instance = candidate.get();
        }
      }
    }
  }
  if (instance == nullptr) {
    return false;
  }
  instance->onPointerEvent(event);
  return true;
}

bool PluginLayerHost::onKeyboardEvent(const KeyboardEvent& event) {
  // Only the surface the compositor reports as keyboard-focused receives keys; a layer with
  // keyboard = "none" never matches.
  PluginLayer* instance = findInstance(m_services.platform.lastKeyboardSurface());
  if (instance == nullptr) {
    return false;
  }
  instance->onKeyboardEvent(event);
  return true;
}

PluginLayerHost::Entry* PluginLayerHost::findEntry(const std::string& entryId) {
  for (auto& entry : m_entries) {
    if (entry->definition.entryId == entryId) {
      return entry.get();
    }
  }
  return nullptr;
}

void PluginLayerHost::show(Entry& entry) {
  if (entry.shown) {
    return;
  }
  entry.shown = true;
  entry.focusedOutput = m_services.platform.preferredInteractiveOutput();
  syncInstances(entry);
}

void PluginLayerHost::hide(Entry& entry) {
  entry.shown = false;
  entry.focusedOutput = nullptr;
  entry.instances.clear();
}

bool PluginLayerHost::targetsOutput(const Entry& entry, const WaylandOutput& output) const {
  const std::string& target = entry.config.output;
  if (target == "all") {
    return true;
  }
  if (target == "focused") {
    return entry.focusedOutput != nullptr && output.output == entry.focusedOutput;
  }
  return output.connectorName == target;
}

void PluginLayerHost::syncInstances(Entry& entry) {
  const auto& outputs = m_services.platform.outputs();
  std::erase_if(entry.instances, [&](const auto& instance) {
    return std::ranges::none_of(outputs, [&](const WaylandOutput& output) {
      return output.output == instance->output() && targetsOutput(entry, output);
    });
  });
  for (const auto& output : outputs) {
    if (output.output == nullptr || !output.done || !targetsOutput(entry, output)) {
      continue;
    }
    const bool covered =
        std::ranges::any_of(entry.instances, [&](const auto& instance) { return instance->output() == output.output; });
    if (covered) {
      continue;
    }
    auto instance = std::make_unique<PluginLayer>(
        m_services,
        scripting::PluginRuntimeContext{
            .entryId = entry.definition.entryId,
            .sourcePath = entry.definition.sourcePath,
            .pluginDir = entry.definition.pluginDir,
            .settings = entry.definition.settings,
            .scriptApi = m_scriptApi,
            .fileWatcher = m_fileWatcher,
            .httpClient = m_httpClient,
            .clipboard = m_clipboard,
        },
        output
    );
    if (instance->initialize(entry.config)) {
      entry.instances.push_back(std::move(instance));
    }
  }
}

PluginLayer* PluginLayerHost::findInstance(wl_surface* surface) {
  if (surface == nullptr) {
    return nullptr;
  }
  for (auto& entry : m_entries) {
    for (auto& instance : entry->instances) {
      if (instance->ownsSurface(surface)) {
        return instance.get();
      }
    }
  }
  return nullptr;
}
