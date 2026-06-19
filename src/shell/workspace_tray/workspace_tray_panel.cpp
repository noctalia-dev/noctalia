#include "shell/workspace_tray/workspace_tray_panel.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

  constexpr float kRowHeight = 24.0f;

} // namespace

WorkspaceTrayPanel::WorkspaceTrayPanel(CompositorPlatform* platform, ConfigService* config)
    : m_platform(platform), m_config(config) {}

void WorkspaceTrayPanel::create() {
  loadConfig();
  auto container = std::make_unique<Node>();
  m_container = container.get();
  setRoot(std::move(container));
}

void WorkspaceTrayPanel::onOpen(std::string_view context) {
  parseOutput(context);
  loadConfig();
  if (m_platform != nullptr && m_output != nullptr) {
    m_workspaces = m_platform->workspaces(m_output);
  }
  m_needsRebuild = true;
}

void WorkspaceTrayPanel::onClose() {
  if (m_container != nullptr) {
    while (!m_container->children().empty()) {
      m_container->removeChild(m_container->children().back().get());
    }
  }
  m_workspaces.clear();
}

PanelPlacement WorkspaceTrayPanel::panelPlacement() const noexcept {
  if (m_config == nullptr) {
    return PanelPlacement::Attached;
  }
  return m_config->config().shell.panel.workspaceTrayPlacement;
}

float WorkspaceTrayPanel::preferredWidth() const {
  ensureDataLoaded();
  const float charWidth = scaled(Style::fontSizeBody * 0.65f);
  const float padding = scaled(Style::spaceSm) * 2.0f;
  const float minWidth = scaled(64.0f);
  return std::max(static_cast<float>(m_maxLabelChars) * charWidth + padding, minWidth);
}

float WorkspaceTrayPanel::preferredHeight() const {
  ensureDataLoaded();
  const bool showPlus = m_showNewWorkspace && !m_newWorkspaceCommand.empty();
  const std::size_t count = visibleCount() + (showPlus ? 1 : 0);
  const float rows = static_cast<float>(std::max(count, static_cast<std::size_t>(1)));
  const float gap = scaled(1.0f);
  const float pad = scaled(Style::panelPadding) * 2.0f;
  return rows * scaled(kRowHeight) + (rows > 1 ? (rows - 1) * gap : 0.0f) + pad;
}

void WorkspaceTrayPanel::doLayout(Renderer& renderer, float width, float /*height*/) {
  m_actualWidth = width;
  if (m_needsRebuild) {
    rebuild(renderer);
    m_needsRebuild = false;
  }
}

void WorkspaceTrayPanel::doUpdate(Renderer& /*renderer*/) {
  if (m_platform == nullptr || m_output == nullptr) {
    return;
  }

  auto current = m_platform->workspaces(m_output);
  bool changed = current.size() != m_workspaces.size();
  if (!changed) {
    for (std::size_t i = 0; i < current.size(); ++i) {
      if (current[i].id != m_workspaces[i].id
          || current[i].name != m_workspaces[i].name
          || current[i].active != m_workspaces[i].active
          || current[i].occupied != m_workspaces[i].occupied
          || current[i].index != m_workspaces[i].index) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    m_workspaces = std::move(current);
    m_needsRebuild = true;
  }
}

void WorkspaceTrayPanel::rebuild(Renderer& renderer) {
  if (m_container == nullptr) {
    return;
  }

  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }

  if (m_platform == nullptr) {
    return;
  }

  if (m_workspaces.empty()) {
    if (m_output != nullptr) {
      m_workspaces = m_platform->workspaces(m_output);
    } else {
      m_workspaces = m_platform->workspaces();
    }
  }

  const float rowHeight = scaled(kRowHeight);
  const float fontSize = scaled(Style::fontSizeBody);
  const float gap = scaled(1.0f);
  const float totalWidth = m_actualWidth > 0.0f ? m_actualWidth : preferredWidth();
  const float contentWidth = totalWidth;

  float y = 0.0f;

  for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
    const auto& ws = m_workspaces[i];
    if (m_hideWhenEmpty && !ws.occupied && !ws.active && !ws.urgent) {
      continue;
    }
    const std::string label = m_displayModeName ? (!ws.name.empty() ? ws.name : ws.id)
                                                : (ws.index > 0 ? std::to_string(ws.index) : std::to_string(i + 1));

    const ColorSpec color = ws.active ? m_focusedColor : m_occupiedColor;

    auto area = std::make_unique<InputArea>();
    area->setFrameSize(contentWidth, rowHeight);
    area->setPosition(0.0f, y);
    area->setOnClick([this, ws](const InputArea::PointerData& data) {
      if (data.pressed) {
        return;
      }
      if (m_platform != nullptr && m_output != nullptr) {
        m_platform->activateWorkspace(m_output, ws);
      }
      PanelManager::instance().close();
    });

    auto* labelNode = static_cast<Label*>(area->addChild(
        ui::label({
            .text = label,
            .fontSize = fontSize,
            .fontWeight = ws.active ? FontWeight::Medium : FontWeight::Normal,
            .color = color,
            .baselineMode = LabelBaselineMode::TextFixedHeight,
        })
    ));
    labelNode->measure(renderer);

    const float lx = (contentWidth - labelNode->width()) * 0.5f;
    const float ly = (rowHeight - labelNode->height()) * 0.5f;
    labelNode->setPosition(lx, ly);

    m_container->addChild(std::move(area));

    y += rowHeight + gap;
  }

  if (m_showNewWorkspace && !m_newWorkspaceCommand.empty()) {
    auto area = std::make_unique<InputArea>();
    area->setFrameSize(contentWidth, rowHeight);
    area->setPosition(0.0f, y);
    area->setOnClick([this](const InputArea::PointerData& data) {
      if (data.pressed) {
        return;
      }
      (void)process::runAsync(m_newWorkspaceCommand);
      PanelManager::instance().close();
    });

    auto* glyphNode = static_cast<Glyph*>(area->addChild(
        ui::glyph({
            .glyph = m_newWorkspaceGlyph,
            .glyphSize = fontSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    ));
    glyphNode->measure(renderer);

    const float gx = (contentWidth - glyphNode->width()) * 0.5f;
    const float gy = (rowHeight - glyphNode->height()) * 0.5f;
    glyphNode->setPosition(gx, gy);

    m_container->addChild(std::move(area));

    y += rowHeight + gap;
  }

  m_container->setFrameSize(totalWidth, y - gap);

  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
}

void WorkspaceTrayPanel::loadConfig() const {
  if (m_config == nullptr) {
    return;
  }
  for (const auto& [name, widget] : m_config->config().widgets) {
    const std::string effectiveType = widget.type.empty() ? name : widget.type;
    if (effectiveType == "workspace_tray") {
      m_maxLabelChars = static_cast<std::size_t>(widget.getInt("max_label_chars", 3));
      m_displayModeName = widget.getString("display", "id") == "name";
      m_focusedColor = widget.getColorSpec(
          "focused_color", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".focused_color"
      );
      m_occupiedColor = widget.getColorSpec(
          "occupied_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".occupied_color"
      );
      m_showNewWorkspace = widget.getBool("show_new_workspace", false);
      m_newWorkspaceCommand = widget.getString("new_workspace_command", "");
      m_newWorkspaceGlyph = widget.getString("new_workspace_glyph", "plus");
      m_hideWhenEmpty = widget.getBool("hide_when_empty", false);
      return;
    }
  }
}

void WorkspaceTrayPanel::parseOutput(std::string_view context) const {
  if (context.empty()) {
    return;
  }
  try {
    const std::string s(context);
    char* end = nullptr;
    const unsigned long addr = std::strtoul(s.c_str(), &end, 16);
    if (end != s.c_str() && addr != 0) {
      m_output = reinterpret_cast<wl_output*>(addr);
    }
  } catch (...) {
  }
}

void WorkspaceTrayPanel::ensureDataLoaded() const {
  loadConfig();
  parseOutput(pendingOpenContext());
  if (m_platform != nullptr && m_output != nullptr) {
    m_workspaces = m_platform->workspaces(m_output);
  }
}

std::size_t WorkspaceTrayPanel::visibleCount() const {
  std::size_t count = 0;
  for (const auto& ws : m_workspaces) {
    if (!m_hideWhenEmpty || ws.occupied || ws.active || ws.urgent) {
      ++count;
    }
  }
  return count;
}
