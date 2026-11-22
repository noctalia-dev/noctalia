#include "shell/greeter/greeter_editor.h"

#include "cli/schema_msg.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/schema/greeter_schema.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/greeter/greeter_preview.h"
#include "system/distro_info.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glib.h>
#include <limits>
#include <numbers>
#include <utility>

namespace {
  constexpr std::array<std::string_view, 5> kCores = {"login", "branding", "keyboard", "appearance", "power"};
  constexpr std::array<std::string_view, 5> kTypes = {"clock", "label", "sticker", "sysmon", "button"};
  std::pair<float, float>
  anchored(GreeterAnchor anchor, float width, float height, float itemWidth, float itemHeight, float edge) {
    const auto index = static_cast<unsigned>(anchor);
    const auto col = index % 3, row = index / 3;
    return {
        col == 0       ? edge + itemWidth / 2
            : col == 2 ? width - edge - itemWidth / 2
                       : width / 2,
        row == 0       ? edge + itemHeight / 2
            : row == 2 ? height - edge - itemHeight / 2
                       : height / 2
    };
  }
  double radians(double value) {
    value = std::fmod(value, 2 * std::numbers::pi);
    if (value >= std::numbers::pi) {
      value -= 2 * std::numbers::pi;
    }
    if (value < -std::numbers::pi) {
      value += 2 * std::numbers::pi;
    }
    return value;
  }
  const GreeterWidgetConfig* findWidget(const std::vector<GreeterWidgetConfig>& widgets, const std::string& id) {
    const auto it = std::ranges::find(widgets, id, &GreeterWidgetConfig::id);
    return it == widgets.end() ? nullptr : &*it;
  }
} // namespace

GreeterEditor::GreeterEditor()
    : m_editor(
          std::make_unique<DesktopWidgetsEditor>(DesktopWidgetsEditorProfile{
              .logSection = "greeter-editor",
              .layerNamespace = "noctalia-greeter-editor",
              .widgetIdPrefix = "greeter-widget-",
              .titleKey = "greeter-editor.title"
          })
      ) {}
GreeterEditor::~GreeterEditor() {
  if (m_profileWatch && m_fileWatcher) {
    m_fileWatcher->unwatch(m_profileWatch);
  }
}

void GreeterEditor::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_platform = services.runtime.platform;
  m_config = services.config;
  m_fileWatcher = services.runtime.scriptDeps.fileWatcher;
  m_userName = sessionUserName();
  m_displayName = sessionDisplayName();
  auto preview = services;
  preview.runtime = {.sysmon = services.runtime.sysmon};
  m_editor->initialize(preview);
  m_editor->setExitRequestedCallback([this] {
    if (save()) {
      close();
      if (m_applySaved) {
        m_applySaved();
      }
    }
  });
  m_config->addReloadCallback(
      [this] {
        if (isOpen()) {
          refreshBacking();
          m_editor->requestLayout();
        }
      },
      "greeter-editor"
  );
}
void GreeterEditor::setLifecycleCallbacks(std::function<bool()> enter, std::function<void()> exit) {
  m_onEnter = std::move(enter);
  m_onExit = std::move(exit);
}
void GreeterEditor::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::greeterEdit, [this](const std::string&) -> std::string {
    open();
    return isOpen() ? "ok\n" : "error: greeter editor unavailable while locked\n";
  });
  ipc.bind(noctalia::cli::msg::greeterExit, [this](const std::string&) -> std::string {
    close();
    return "ok\n";
  });
}
void GreeterEditor::open() {
  if (isOpen() || m_config == nullptr || m_wayland == nullptr) {
    return;
  }
  if (m_onEnter && !m_onEnter()) {
    return;
  }
  m_draft = std::make_unique<GreeterConfigDraft>();
  if (m_fileWatcher && !m_draft->path().empty()) {
    std::error_code ec;
    const auto parent = m_draft->path().parent_path();
    if (std::filesystem::create_directories(parent, ec)) {
      std::filesystem::permissions(parent, std::filesystem::perms::owner_all, ec);
    }
    if (!ec) {
      m_profileWatch = m_fileWatcher->watch(
          m_draft->path(),
          [this] {
            if (isOpen()) {
              refreshBacking();
              m_editor->requestLayout();
            }
          },
          FileWatcher::WatchTrigger::WriteCompleted
      );
    }
  }
  m_activeOutput = m_config->stateString("greeter_editor", "output").value_or("");
  if (m_activeOutput.empty()) {
    for (const auto& output : m_wayland->outputs()) {
      if (output.done && output.hasUsableGeometry()) {
        m_activeOutput = desktop_widgets::outputKey(output);
        break;
      }
    }
  }
  m_compactSample = m_config->stateBool("greeter_editor", "compact_sample").value_or(false);
  m_status = m_draft->error();
  m_conflict = m_draft->backingChanged();
  activateOutput(m_activeOutput);
  m_editor->open(*this, m_config->stateBool("greeter_editor", "grid_visible").value_or(false));
  if (!m_status.empty()) {
    m_editor->selectItem(coreId("login"));
  }
}
void GreeterEditor::close() {
  if (!isOpen()) {
    return;
  }
  const auto snapshot = m_editor->close();
  m_config->setStateBool("greeter_editor", "grid_visible", snapshot.grid.visible);
  if (m_profileWatch && m_fileWatcher) {
    m_fileWatcher->unwatch(m_profileWatch);
  }
  m_profileWatch = 0;
  m_draft.reset();
  m_items.clear();
  m_previous.clear();
  m_naturalSizes.clear();
  if (m_onExit) {
    m_onExit();
  }
}
bool GreeterEditor::isOpen() const { return m_editor->isOpen(); }
bool GreeterEditor::onPointerEvent(const PointerEvent& event) { return m_editor->onPointerEvent(event); }
void GreeterEditor::onKeyboardEvent(const KeyboardEvent& event) { m_editor->onKeyboardEvent(event); }
void GreeterEditor::onOutputChange() {
  if (isOpen()) {
    activateOutput(m_activeOutput);
    m_editor->onOutputChange();
  }
}
void GreeterEditor::onSecondTick() {
  if (isOpen()) {
    refreshBacking();
    m_editor->onSecondTick();
  }
}
void GreeterEditor::requestLayout() {
  if (isOpen()) {
    m_editor->requestLayout();
  }
}
std::optional<LayerPopupParentContext> GreeterEditor::popupParentContextForSurface(wl_surface* surface) const {
  return m_editor->popupParentContextForSurface(surface);
}
std::optional<LayerPopupParentContext> GreeterEditor::fallbackPopupParentContext() const {
  return m_editor->fallbackPopupParentContext();
}
GreeterProfile& GreeterEditor::profile() { return m_draft->profile(); }
const GreeterProfile& GreeterEditor::profile() const { return m_draft->profile(); }
std::string GreeterEditor::coreType(const std::string& id) const { return id.substr(0, id.find('@')); }
std::string GreeterEditor::coreOutput(const std::string& id) const {
  const auto at = id.find('@');
  return at == std::string::npos ? m_activeOutput : id.substr(at + 1);
}
std::string GreeterEditor::coreId(const std::string& type) const { return type + "@" + m_activeOutput; }
bool GreeterEditor::isCore(const std::string& id) const { return std::ranges::contains(kCores, coreType(id)); }
bool GreeterEditor::isRequired(const std::string& id) const { return coreType(id) == "login"; }
GreeterLayoutConfig& GreeterEditor::editableLayout(const std::string& id) {
  if (!profile().layout) {
    profile().layout.emplace();
  }
  return profile().layout->outputs[coreOutput(id)];
}
GreeterCoreOverride GreeterEditor::coreSettings(const std::string& id) const {
  GreeterCoreOverride result;
  if (!profile().layout) {
    return result;
  }
  const auto kind = coreType(id);
  if (const auto base = profile().layout->components.find(kind); base != profile().layout->components.end()) {
    result = base->second;
  }
  if (const auto local = profile().layout->outputs.find(coreOutput(id)); local != profile().layout->outputs.end()) {
    if (const auto entry = local->second.components.find(kind); entry != local->second.components.end()) {
      const auto prefer = [](auto& target, const auto& value) {
        if (value) {
          target = value;
        }
      };
      const auto& value = entry->second;
      prefer(result.visible, value.visible);
      prefer(result.anchor, value.anchor);
      prefer(result.x, value.x);
      prefer(result.y, value.y);
      prefer(result.width, value.width);
      prefer(result.output, value.output);
      prefer(result.rotation, value.rotation);
      prefer(result.scale, value.scale);
      prefer(result.flipX, value.flipX);
      prefer(result.flipY, value.flipY);
    }
  }
  return result;
}
std::optional<GreeterLoginMode> GreeterEditor::loginMode(const std::string& output) const {
  if (!profile().layout) {
    return {};
  }
  const auto local = profile().layout->outputs.find(output);
  return local != profile().layout->outputs.end() && local->second.loginMode ? local->second.loginMode
                                                                             : profile().layout->loginMode;
}
bool GreeterEditor::minimal(const std::string& output) const {
  const auto mode = loginMode(output.empty() ? m_activeOutput : output).value_or(GreeterLoginMode::Auto);
  return mode == GreeterLoginMode::Minimal || (mode == GreeterLoginMode::Auto && m_compactSample);
}
std::pair<float, float> GreeterEditor::coreSize(const std::string& id) const {
  const auto kind = coreType(id);
  const bool compact = minimal(coreOutput(id));
  const float width = kind == "login" ? (compact ? 360.0F : 420.0F)
      : kind == "branding"            ? 28.0F
      : kind == "power"               ? 88.0F
      : kind == "appearance"          ? 140.0F
                                      : 148.0F;
  return {
      static_cast<float>(coreSettings(id).width.value_or(width)),
      kind == "login"          ? (compact ? 270.0F : 468.0F)
          : kind == "branding" ? 28.0F
                               : 36.0F
  };
}
GreeterAnchor GreeterEditor::coreAnchor(const std::string& id, float width, float height) const {
  if (const auto anchor = coreSettings(id).anchor) {
    return *anchor;
  }
  if (isRequired(id)) {
    return width < 1000 || height < 600 ? GreeterAnchor::Center : GreeterAnchor::CenterRight;
  }
  return coreType(id) == "branding" ? GreeterAnchor::TopLeft : GreeterAnchor::BottomRight;
}
std::vector<GreeterWidgetConfig> GreeterEditor::effectiveWidgets() const {
  const auto scopedId = [](const std::string& id, const std::string& output) {
    const auto input = id + "\n" + output;
    gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input.c_str(), static_cast<gssize>(input.size()));
    std::string result(hash);
    g_free(hash);
    if (id == "greeter-default-clock" || id == "greeter-default-date") {
      result = id + "-" + result.substr(0, 32);
    }
    return result;
  };
  std::vector<GreeterWidgetConfig> defaults;
  if (profile().widgets && profile().widgets->instances) {
    for (const auto& source : *profile().widgets->instances) {
      if (source.output) {
        defaults.push_back(source);
        continue;
      }
      for (const auto& target : m_wayland->outputs()) {
        if (!target.done || !target.hasUsableGeometry()) {
          continue;
        }
        auto copy = source;
        copy.output = desktop_widgets::outputKey(target);
        copy.id = scopedId(source.id, *copy.output);
        defaults.push_back(std::move(copy));
      }
    }
    return defaults;
  }
  for (const auto& target : m_wayland->outputs()) {
    if (!target.done || !target.hasUsableGeometry()) {
      continue;
    }
    const auto output = desktop_widgets::outputKey(target);
    const float width = desktop_widgets::outputLogicalWidth(target),
                height = desktop_widgets::outputLogicalHeight(target);
    const bool narrow = width < 1000 || height < 600;
    const auto measuredHeight = [this](const std::string& id, float fallback) {
      const auto it = m_naturalSizes.find(id);
      return it == m_naturalSizes.end() ? fallback : it->second.second;
    };
    const float timeHeight = measuredHeight(scopedId("greeter-default-clock", output), narrow ? 42 : 84);
    const float dateHeight = measuredHeight(scopedId("greeter-default-date", output), narrow ? 14 : 19);
    for (const bool date : {false, true}) {
      GreeterWidgetConfig clock;
      clock.id = scopedId(date ? "greeter-default-date" : "greeter-default-clock", output);
      clock.output = output;
      clock.type = "clock";
      const float edge =
          narrow ? std::min(24.0F, std::min(width, height) * .04F) : std::clamp(width * .06F, 64.0F, 128.0F);
      const double x = narrow ? (width - std::min(400.0F, width - 2 * edge)) / 2 - edge : 0;
      const double y = narrow ? 48 + (date ? timeHeight + 12 : 0)
          : date              ? (timeHeight + 12) / 2
                              : -(dateHeight + 12) / 2;
      clock.placement = GreeterAnchorPlacement{
          .anchor = narrow ? GreeterAnchor::TopLeft : GreeterAnchor::CenterLeft, .x = x + (date ? 5 : 0), .y = y
      };
      clock.settings = {
          {"background", false},
          {"center_text", false},
          {"tight_text", true},
          {"font_size", static_cast<double>(date ? (narrow ? 14 : 19) : (narrow ? 56 : 112))},
          {"font_weight", std::int64_t{date ? 500 : 300}},
          {"shadow", date},
          {"format", std::string(date ? "%A, %x" : "{:%H:%M}")}
      };
      defaults.push_back(std::move(clock));
    }
  }
  return defaults;
}
std::vector<GreeterWidgetConfig>& GreeterEditor::customWidgets() {
  auto widgets = effectiveWidgets();
  if (!profile().widgets) {
    profile().widgets.emplace();
  }
  if (!profile().widgets->instances
      || std::ranges::any_of(*profile().widgets->instances, [](const auto& widget) { return !widget.output; })) {
    profile().widgets->instances = std::move(widgets);
  }
  return *profile().widgets->instances;
}
const WaylandOutput* GreeterEditor::currentOutput() const {
  return m_wayland != nullptr ? desktop_widgets::findOutputByKey(*m_wayland, m_activeOutput) : nullptr;
}
void GreeterEditor::activateOutput(const std::string& name) {
  flush();
  if (isOpen() && m_activeOutput != name) {
    m_config->setStateString("greeter_editor", "output", name);
  }
  m_activeOutput = name;
  if (const auto* output = currentOutput()) {
    m_referenceWidth = desktop_widgets::outputLogicalWidth(*output);
    m_referenceHeight = desktop_widgets::outputLogicalHeight(*output);
  }
  rebuildItems();
}
void GreeterEditor::rebuildItems() {
  m_items.clear();
  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    for (const auto kind : kCores) {
      DesktopWidgetState item;
      item.outputName = desktop_widgets::outputKey(output);
      item.id = std::string(kind) + "@" + item.outputName;
      item.type = "greeter-core";
      m_items.push_back(std::move(item));
    }
  }
  for (const auto& widget : effectiveWidgets()) {
    DesktopWidgetState item;
    item.id = widget.id;
    item.type = widget.type;
    item.outputName = widget.output.value_or("");
    m_items.push_back(std::move(item));
  }
  WaylandOutput reference;
  reference.logicalWidth = static_cast<int>(m_referenceWidth);
  reference.logicalHeight = static_cast<int>(m_referenceHeight);
  const auto* output = currentOutput();
  for (auto& item : m_items) {
    const auto* owner =
        item.outputName.empty() ? output : desktop_widgets::findOutputByKey(*m_wayland, item.outputName);
    item = presentation(item, owner != nullptr ? *owner : reference);
  }
  m_previous = m_items;
}
DesktopWidgetState GreeterEditor::presentation(const DesktopWidgetState& item, const WaylandOutput& output) const {
  const float width = desktop_widgets::outputLogicalWidth(output),
              height = desktop_widgets::outputLogicalHeight(output);
  DesktopWidgetState state = item;
  state.placementWidth = width;
  state.placementHeight = height;
  const bool narrow = width < 1000 || height < 600;
  const float edge =
      narrow ? std::min(24.0F, std::min(width, height) * 0.04F) : std::clamp(width * 0.06F, 64.0F, 128.0F);
  if (isCore(item.id)) {
    const auto core = coreSettings(item.id);
    const auto [baseWidth, baseHeight] = coreSize(item.id);
    float scale = static_cast<float>(core.scale.value_or(1));
    if (isRequired(item.id)) {
      const float rotation = static_cast<float>(core.rotation.value_or(0));
      const float aabbWidth = std::abs(std::cos(rotation)) * baseWidth + std::abs(std::sin(rotation)) * baseHeight;
      const float aabbHeight = std::abs(std::sin(rotation)) * baseWidth + std::abs(std::cos(rotation)) * baseHeight;
      scale =
          std::min({scale, std::max(0.05F, (width - 48) / aabbWidth), std::max(0.05F, (height - 192) / aabbHeight)});
    }
    state.boxWidth = baseWidth * scale;
    state.boxHeight = baseHeight * scale;
    auto [cx, cy] = anchored(
        coreAnchor(item.id, width, height), width, height, baseWidth, baseHeight,
        isRequired(item.id) ? edge : (narrow ? edge : 48)
    );
    if (!core.anchor) {
      if (coreType(item.id) == "appearance") {
        cx -= 98;
      } else if (coreType(item.id) == "keyboard") {
        cx -= 248;
      }
    }
    state.cx = cx + static_cast<float>(core.x.value_or(0));
    state.cy = cy + static_cast<float>(core.y.value_or(0));
    if (isRequired(item.id)) {
      state.cx = std::clamp(
          state.cx, state.boxWidth / 2 + 16, std::max(state.boxWidth / 2 + 16, width - state.boxWidth / 2 - 16)
      );
      state.cy = std::clamp(
          state.cy, state.boxHeight / 2 + 96, std::max(state.boxHeight / 2 + 96, height - state.boxHeight / 2 - 96)
      );
    }
    state.enabled = core.visible.value_or(true);
    state.outputName = item.outputName;
    state.rotationRad = static_cast<float>(core.rotation.value_or(0));
    state.flipX = core.flipX.value_or(false);
    state.flipY = core.flipY.value_or(false);
    return state;
  }
  const auto widgets = effectiveWidgets();
  const auto* widget = findWidget(widgets, item.id);
  if (widget == nullptr) {
    return state;
  }
  state.type = widget->type;
  state.settings = widget->settings;
  state.enabled = widget->enabled;
  state.outputName = widget->output.value_or("");
  state.boxWidth = static_cast<float>(widget->boxWidth);
  state.boxHeight = static_cast<float>(widget->boxHeight);
  state.rotationRad = static_cast<float>(widget->rotation);
  state.flipX = widget->flipX;
  state.flipY = widget->flipY;
  if (const auto* anchor = std::get_if<GreeterAnchorPlacement>(&widget->placement)) {
    auto [cx, cy] = anchored(
        anchor->anchor, width, height,
        state.boxWidth > 0 ? state.boxWidth
                           : (m_naturalSizes.contains(item.id) ? m_naturalSizes.at(item.id).first : 320),
        state.boxHeight > 0 ? state.boxHeight
                            : (m_naturalSizes.contains(item.id) ? m_naturalSizes.at(item.id).second : 160),
        edge
    );
    state.cx = cx + static_cast<float>(anchor->x);
    state.cy = cy + static_cast<float>(anchor->y);
  } else {
    const auto& relative = std::get<GreeterRelativePlacement>(widget->placement);
    state.cx = static_cast<float>(relative.cx / relative.placementWidth) * width;
    state.cy = static_cast<float>(relative.cy / relative.placementHeight) * height;
  }
  return state;
}
bool GreeterEditor::visibleOnOutput(const DesktopWidgetState& item, const WaylandOutput& output) const {
  const auto projected = presentation(item, output);
  if (isCore(item.id)) {
    const auto selector = coreSettings(item.id).output;
    if (selector && *selector != desktop_widgets::outputKey(output) && !outputMatchesSelector(*selector, output)) {
      return false;
    }
  }
  if (isCore(item.id) && !isRequired(item.id) && !projected.enabled) {
    return false;
  }
  return projected.outputName.empty()
      || projected.outputName == desktop_widgets::outputKey(output)
      || outputMatchesSelector(projected.outputName, output);
}
void GreeterEditor::flush() {
  if (!m_draft || m_items == m_previous) {
    return;
  }
  const auto beforeWidgets = effectiveWidgets();
  const auto previous = m_previous;
  bool widgetsChanged = false;
  for (const auto& item : m_items) {
    const auto old = std::ranges::find(previous, item.id, &DesktopWidgetState::id);
    if (old != previous.end() && item == *old) {
      continue;
    }
    if (isCore(item.id)) {
      if (!profile().layout) {
        profile().layout.emplace();
      }
      auto& core = editableLayout(item.id).components[coreType(item.id)];
      if (old == previous.end()) {
        continue;
      }
      const auto inherited = coreSettings(item.id);
      const auto [previousWidth, previousHeight] = coreSize(item.id);
      if (item.boxWidth != old->boxWidth || item.boxHeight != old->boxHeight) {
        const double oldScale = inherited.scale.value_or(1);
        const double ratio = old->boxHeight > 0 ? item.boxHeight / old->boxHeight : 1;
        if (std::abs(ratio - 1) > 0.00001) {
          core.scale = std::clamp(oldScale * ratio, 0.5, 4.0);
        }
        const double baseWidth = previousWidth;
        const auto resizedWidth = std::clamp(
            baseWidth * (old->boxWidth > 0 ? item.boxWidth / old->boxWidth : 1) / std::max(0.01, ratio),
            isRequired(item.id) ? 280.0 : 32.0, isRequired(item.id) ? 640.0 : 1600.0
        );
        if (std::abs(resizedWidth - baseWidth) > 0.001) {
          core.width = resizedWidth;
        }
      }
      const auto [anchorWidth, anchorHeight] = coreSize(item.id);
      const auto anchorIndex = static_cast<unsigned>(coreAnchor(item.id, m_referenceWidth, m_referenceHeight));
      const double xFactor = anchorIndex % 3 == 0 ? -0.5 : anchorIndex % 3 == 2 ? 0.5 : 0;
      const double yFactor = anchorIndex / 3 == 0 ? -0.5 : anchorIndex / 3 == 2 ? 0.5 : 0;
      const double dx = item.cx - old->cx + xFactor * (anchorWidth - previousWidth);
      const double dy = item.cy - old->cy + yFactor * (anchorHeight - previousHeight);
      if (item.cx != old->cx || item.cy != old->cy) {
        const bool narrow = m_referenceWidth < 1000 || m_referenceHeight < 600;
        const float edge = narrow ? std::min(24.0F, std::min(m_referenceWidth, m_referenceHeight) * 0.04F)
                                  : std::clamp(m_referenceWidth * 0.06F, 64.0F, 128.0F);
        const float margin = isRequired(item.id) ? edge : (narrow ? edge : 48);
        if (coreType(item.id) == "keyboard") {
          core.width = anchorWidth;
        }
        // Anchor the untransformed control, matching the greeter runtime.
        // In particular, moving to the center stores center + zero offsets.
        float distance = std::numeric_limits<float>::max();
        for (unsigned index = 0; index < 9; ++index) {
          const auto anchor = static_cast<GreeterAnchor>(index);
          const auto [cx, cy] =
              anchored(anchor, m_referenceWidth, m_referenceHeight, anchorWidth, anchorHeight, margin);
          const float candidate = std::hypot((item.cx - cx) / m_referenceWidth, (item.cy - cy) / m_referenceHeight);
          if (candidate < distance) {
            distance = candidate;
            core.anchor = anchor;
            core.x = std::clamp(static_cast<double>(item.cx - cx), -8192.0, 8192.0);
            core.y = std::clamp(static_cast<double>(item.cy - cy), -8192.0, 8192.0);
          }
        }
      } else {
        if (dx != 0) {
          core.x = std::clamp(core.x.value_or(inherited.x.value_or(0)) + dx, -8192.0, 8192.0);
        }
        if (dy != 0) {
          core.y = std::clamp(core.y.value_or(inherited.y.value_or(0)) + dy, -8192.0, 8192.0);
        }
      }
      if (item.rotationRad != old->rotationRad) {
        core.rotation = radians(item.rotationRad);
      }
      if (item.flipX != old->flipX) {
        core.flipX = item.flipX;
      }
      if (item.flipY != old->flipY) {
        core.flipY = item.flipY;
      }
      if (item.enabled != old->enabled && !isRequired(item.id)) {
        core.visible = item.enabled;
      }
    } else {
      widgetsChanged = true;
    }
  }
  std::vector<std::string> oldOrder, newOrder;
  for (const auto& item : previous) {
    if (!isCore(item.id)) {
      oldOrder.push_back(item.id);
    }
  }
  for (const auto& item : m_items) {
    if (!isCore(item.id)) {
      newOrder.push_back(item.id);
    }
  }
  widgetsChanged = widgetsChanged || oldOrder != newOrder;
  if (widgetsChanged) {
    std::vector<GreeterWidgetConfig> next;
    for (const auto& item : m_items) {
      if (isCore(item.id)) {
        continue;
      }
      GreeterWidgetConfig widget;
      const auto* existing = findWidget(beforeWidgets, item.id);
      if (existing != nullptr) {
        widget = *existing;
      } else {
        widget.id = item.id;
        widget.type = item.type;
        widget.placement = GreeterRelativePlacement{item.cx, item.cy, m_referenceWidth, m_referenceHeight};
        if (!item.outputName.empty()) {
          widget.output = item.outputName;
        }
      }
      const auto old = std::ranges::find(previous, item.id, &DesktopWidgetState::id);
      if (old != previous.end()
          && (item.cx != old->cx
              || item.cy != old->cy
              || item.boxWidth != old->boxWidth
              || item.boxHeight != old->boxHeight)) {
        if (auto* anchor = std::get_if<GreeterAnchorPlacement>(&widget.placement)) {
          const auto index = static_cast<unsigned>(anchor->anchor);
          const double xFactor = index % 3 == 0 ? -0.5 : index % 3 == 2 ? 0.5 : 0;
          const double yFactor = index / 3 == 0 ? -0.5 : index / 3 == 2 ? 0.5 : 0;
          const auto natural =
              m_naturalSizes.contains(item.id) ? m_naturalSizes.at(item.id) : std::pair{320.0F, 160.0F};
          const float oldWidth = old->boxWidth > 0 ? old->boxWidth : natural.first;
          const float oldHeight = old->boxHeight > 0 ? old->boxHeight : natural.second;
          const float newWidth = item.boxWidth > 0 ? item.boxWidth : natural.first;
          const float newHeight = item.boxHeight > 0 ? item.boxHeight : natural.second;
          anchor->x = std::clamp(anchor->x + item.cx - old->cx + xFactor * (newWidth - oldWidth), -8192.0, 8192.0);
          anchor->y = std::clamp(anchor->y + item.cy - old->cy + yFactor * (newHeight - oldHeight), -8192.0, 8192.0);
        } else {
          auto& relative = std::get<GreeterRelativePlacement>(widget.placement);
          relative.cx = std::clamp(item.cx / m_referenceWidth * relative.placementWidth, -65536.0, 65536.0);
          relative.cy = std::clamp(item.cy / m_referenceHeight * relative.placementHeight, -65536.0, 65536.0);
        }
      }
      widget.enabled = item.enabled;
      if (old == previous.end() || item.boxWidth != old->boxWidth) {
        widget.boxWidth = item.boxWidth == 0 ? 0 : std::clamp<double>(item.boxWidth, 16, 4096);
      }
      if (old == previous.end() || item.boxHeight != old->boxHeight) {
        widget.boxHeight = item.boxHeight == 0 ? 0 : std::clamp<double>(item.boxHeight, 16, 4096);
      }
      if (old == previous.end() || item.rotationRad != old->rotationRad) {
        widget.rotation = radians(item.rotationRad);
      }
      widget.flipX = item.flipX;
      widget.flipY = item.flipY;
      widget.settings = item.settings;
      next.push_back(std::move(widget));
    }
    customWidgets() = std::move(next);
  }
  m_previous = m_items;
  m_status.clear();
}
std::unique_ptr<DesktopWidget> GreeterEditor::createPreview(const DesktopWidgetState& item) const {
  if (!isCore(item.id)) {
    return nullptr;
  }
  return std::make_unique<GreeterPreviewWidget>(
      coreType(item.id), coreSize(item.id).first, minimal(coreOutput(item.id)), m_userName, m_displayName,
      m_platform ? m_platform->currentKeyboardLayoutName() : m_wayland->currentKeyboardLayoutName()
  );
}
std::vector<desktop_settings::DesktopWidgetTypeOption> GreeterEditor::typeOptions() const {
  std::vector<desktop_settings::DesktopWidgetTypeOption> result;
  result.reserve(kTypes.size() + kCores.size());
  for (const auto core : kCores) {
    result.push_back({std::string(core), i18n::tr("greeter-editor.components." + std::string(core))});
  }
  for (const auto type : kTypes) {
    result.push_back({std::string(type), desktop_settings::desktopWidgetTypeLabel(type)});
  }
  return result;
}
std::vector<settings::WidgetSettingSpec>
GreeterEditor::settingSpecs(const DesktopWidgetState& item, bool background) const {
  if (isCore(item.id)) {
    return {};
  }
  auto specs = background ? desktop_settings::commonDesktopWidgetSettingSpecs(item.type)
                          : desktop_settings::desktopWidgetSettingSpecs(item.type);
  const auto fields = noctalia::config::schema::greeterWidgetSettingSchema(item.type);
  std::erase_if(specs, [&](const auto& spec) {
    return std::ranges::none_of(fields, [&](const auto& field) { return field.key == spec.schema.key; });
  });
  for (auto& spec : specs) {
    const auto field = std::ranges::find(fields, spec.schema.key, &noctalia::config::schema::WidgetSettingField::key);
    spec.schema = *field;
    if (spec.control == settings::WidgetControlKind::Select && !spec.schema.enumValues.empty()) {
      std::erase_if(spec.options, [&](const auto& option) {
        return !std::ranges::contains(spec.schema.enumValues, option.value);
      });
    }
  }
  if (!background && item.type == "button") {
    settings::WidgetSettingSpec action;
    action.schema = *std::ranges::find(fields, "action", &noctalia::config::schema::WidgetSettingField::key);
    action.control = settings::WidgetControlKind::Select;
    action.labelKey = "greeter-editor.inspector.action";
    for (const auto& value : action.schema.enumValues) {
      action.options.push_back({value, "greeter-editor.actions." + value});
    }
    specs.push_back(std::move(action));
  }
  return specs;
}
std::string GreeterEditor::itemLabel(const DesktopWidgetState& item) const {
  return isCore(item.id) ? i18n::tr("greeter-editor.components." + coreType(item.id))
                         : desktop_settings::desktopWidgetTypeLabel(item.type);
}
void GreeterEditor::resetItem(const std::string& id) {
  flush();
  if (isCore(id)) {
    if (profile().layout) {
      editableLayout(id).components.erase(coreType(id));
    }
  } else {
    auto& widgets = customWidgets();
    const auto it = std::ranges::find(widgets, id, &GreeterWidgetConfig::id);
    if (it != widgets.end()) {
      it->settings.clear();
    }
  }
  rebuildItems();
}
void GreeterEditor::setApplyStatus(std::string message) {
  m_applyStatus = std::move(message);
  if (isOpen()) {
    m_editor->requestLayout();
  }
}

bool GreeterEditor::save() {
  flush();
  if (m_draft->save()) {
    m_conflict = false;
    m_status.clear();
    m_editor->requestLayout();
    return true;
  }
  m_status = m_draft->error();
  refreshBacking();
  m_editor->selectItem(m_editor->selectedItem().empty() ? coreId("login") : m_editor->selectedItem());
  m_editor->requestLayout();
  return false;
}
void GreeterEditor::refreshBacking() {
  const auto previousError = m_draft->error();
  m_draft->refresh();
  if (m_status == previousError || !m_draft->error().empty()) {
    m_status = m_draft->error();
  }
  const bool conflict = m_draft->backingChanged();
  if (conflict != m_conflict) {
    m_conflict = conflict;
    if (conflict) {
      m_editor->selectItem(m_editor->selectedItem().empty() ? coreId("login") : m_editor->selectedItem());
    }
    m_editor->requestLayout();
  }
}

bool GreeterEditor::wantsSecondTicks() const { return isOpen() && m_editor->wantsSecondTicks(); }
void GreeterEditor::requestUpdate() {
  if (isOpen()) {
    m_editor->requestUpdate();
  }
}

void GreeterEditor::measured(const std::string& id, float width, float height) {
  const auto size = std::pair{width, height};
  if (isCore(id) || (m_naturalSizes.contains(id) && m_naturalSizes.at(id) == size)) {
    return;
  }
  m_naturalSizes.insert_or_assign(id, size);
  if (const auto* output = currentOutput()) {
    const auto it = std::ranges::find(m_items, id, &DesktopWidgetState::id);
    if (it != m_items.end()) {
      const auto state = presentation(*it, *output);
      it->cx = state.cx;
      it->cy = state.cy;
    }
  }
  m_previous = m_items;
}

std::string GreeterEditor::showCore(const std::string& type) {
  const auto id = coreId(type);
  flush();
  if (!isRequired(id)) {
    const auto item = std::ranges::find(m_items, id, &DesktopWidgetState::id);
    if (item != m_items.end() && !item->enabled) {
      if (!profile().layout) {
        profile().layout.emplace();
      }
      editableLayout(id).components[coreType(id)].visible = true;
    }
  }
  rebuildItems();
  return id;
}
void GreeterEditor::hideCore(const std::string& id) {
  if (isRequired(id)) {
    return;
  }
  flush();
  if (!profile().layout) {
    profile().layout.emplace();
  }
  editableLayout(id).components[coreType(id)].visible = false;
  rebuildItems();
}
