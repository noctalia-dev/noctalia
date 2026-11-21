#include "config/config_service.h"
#include "config/schema/greeter_schema.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/greeter/greeter_editor.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <string_view>
#include <utility>

namespace {
  constexpr std::array<std::string_view, 9> kAnchors = {"top-left",    "top-center",    "top-right",
                                                        "center-left", "center",        "center-right",
                                                        "bottom-left", "bottom-center", "bottom-right"};
  std::string tr(std::string_view key) { return i18n::tr("greeter-editor." + std::string(key)); }
  std::unique_ptr<Node> sectionLabel(std::string_view key) {
    return ui::label(
        {.text = tr(key),
         .fontSize = Style::fontSizeCaption,
         .fontWeight = FontWeight::Medium,
         .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
    );
  }
} // namespace

bool GreeterEditor::applyProperty(const std::string& id, const std::string& key, const WidgetSettingValue& value) {
  if (!key.starts_with("item.")) {
    return false;
  }
  flush();
  const auto before = profile();
  const auto name = std::string_view(key).substr(5);
  const auto* number = std::get_if<double>(&value);
  const auto* string = std::get_if<std::string>(&value);
  if (isCore(id)) {
    if (!profile().layout) {
      profile().layout.emplace();
    }
    auto& core = profile().layout->components[id];
    if (name == "login_mode" && string) {
      if (*string == "inherit") {
        profile().layout->loginMode.reset();
      } else if (*string == "auto") {
        profile().layout->loginMode = GreeterLoginMode::Auto;
      } else if (*string == "full") {
        profile().layout->loginMode = GreeterLoginMode::Full;
      } else if (*string == "minimal") {
        profile().layout->loginMode = GreeterLoginMode::Minimal;
      }
    } else if (name == "anchor" && string) {
      if (string->empty()) {
        core.anchor.reset();
      } else {
        const auto* it = std::ranges::find(kAnchors, *string);
        if (it != kAnchors.end()) {
          core.anchor = static_cast<GreeterAnchor>(std::distance(kAnchors.begin(), it));
        }
      }
    } else if (name == "output" && string && !isRequired(id)) {
      core.output = string->empty() ? std::nullopt : std::make_optional(*string);
    } else if (number) {
      if (name == "x") {
        core.x = *number;
      } else if (name == "y") {
        core.y = *number;
      } else if (name == "width") {
        core.width = *number;
      } else if (name == "scale") {
        core.scale = *number;
      } else if (name == "rotation") {
        double radians = std::remainder(*number, 2 * std::numbers::pi);
        if (radians >= std::numbers::pi) {
          radians -= 2 * std::numbers::pi;
        }
        core.rotation = radians;
      }
    }
  } else {
    auto& widgets = customWidgets();
    const auto it = std::ranges::find(widgets, id, &GreeterWidgetConfig::id);
    if (it == widgets.end()) {
      return true;
    }
    auto& widget = *it;
    const auto state = std::ranges::find(m_items, id, &DesktopWidgetState::id);
    if (name == "placement" && string && state != m_items.end()) {
      if (*string == "relative") {
        widget.placement = GreeterRelativePlacement{state->cx, state->cy, m_referenceWidth, m_referenceHeight};
      } else if (*string == "anchor") {
        widget.placement = GreeterAnchorPlacement{};
        if (const auto* output = currentOutput()) {
          const auto centered = presentation(*state, *output);
          auto& anchor = std::get<GreeterAnchorPlacement>(widget.placement);
          anchor.x = state->cx - centered.cx;
          anchor.y = state->cy - centered.cy;
        }
      }
    } else if (name == "anchor" && string) {
      if (auto* anchor = std::get_if<GreeterAnchorPlacement>(&widget.placement)) {
        const auto* a = std::ranges::find(kAnchors, *string);
        if (a != kAnchors.end()) {
          anchor->anchor = static_cast<GreeterAnchor>(std::distance(kAnchors.begin(), a));
        }
      }
    } else if (name == "output" && string) {
      widget.output = string->empty() ? std::nullopt : std::make_optional(*string);
    } else if (name == "reset_geometry") {
      widget.placement =
          GreeterRelativePlacement{m_referenceWidth / 2, m_referenceHeight / 2, m_referenceWidth, m_referenceHeight};
      widget.output.reset();
      widget.boxWidth = 0;
      widget.boxHeight = 0;
      widget.rotation = 0;
      widget.flipX = false;
      widget.flipY = false;
    } else if (number) {
      if (name == "width") {
        widget.boxWidth = *number;
      } else if (name == "height") {
        widget.boxHeight = *number;
      } else if (name == "rotation") {
        double angle = std::remainder(*number, 2 * std::numbers::pi);
        if (angle >= std::numbers::pi) {
          angle -= 2 * std::numbers::pi;
        }
        widget.rotation = angle;
      } else if (auto* anchor = std::get_if<GreeterAnchorPlacement>(&widget.placement)) {
        if (name == "x") {
          anchor->x = *number;
        } else if (name == "y") {
          anchor->y = *number;
        }
      } else {
        auto& relative = std::get<GreeterRelativePlacement>(widget.placement);
        if (name == "x") {
          relative.cx = *number / m_referenceWidth * relative.placementWidth;
        } else if (name == "y") {
          relative.cy = *number / m_referenceHeight * relative.placementHeight;
        }
      }
    }
  }
  noctalia::config::schema::Diagnostics diagnostics;
  const auto serialized = noctalia::config::schema::writeGreeterConfig(profile());
  if (!noctalia::config::schema::readGreeterConfig(serialized, "shell.greeter", diagnostics)) {
    profile() = before;
    m_status = diagnostics.entries.front().describe();
  } else {
    m_status.clear();
  }
  rebuildItems();
  return true;
}

void GreeterEditor::buildInspector(Flex& content, DesktopWidgetsEditor& editor, const DesktopWidgetState& item) {
  if (isRequired(item.id) || item.type == "button") {
    content.addChild(
        ui::label(
            {.text = tr("preview"),
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
  }
  if (!m_status.empty()) {
    content.addChild(
        ui::label(
            {.text = m_status,
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxWidth = 450.0F,
             .maxLines = 0}
        )
    );
  }
  if (m_conflict) {
    content.addChild(
        ui::label(
            {.text = tr("conflict"),
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::Error),
             .maxWidth = 450.0F,
             .maxLines = 0}
        )
    );
    content.addChild(
        ui::row(
            {.gap = Style::spaceSm},
            ui::button(
                {.text = tr("keep-draft"),
                 .controlHeight = Style::controlHeightSm,
                 .onClick =
                     [this, &editor] {
                       editor.mutateDocument([this, &editor] {
                         m_draft->keepDraft();
                         m_conflict = false;
                         m_status.clear();
                         editor.requestLayout();
                       });
                     }}
            ),
            ui::button({.text = tr("reload"), .controlHeight = Style::controlHeightSm, .onClick = [this, &editor] {
                          editor.mutateDocument([this, &editor] {
                            m_draft->discard();
                            m_conflict = false;
                            m_status.clear();
                            rebuildItems();
                            editor.requestLayout();
                          });
                        }})
        )
    );
  }
  content.addChild(sectionLabel("inspector.placement"));
  const auto row = [&](std::string_view key, std::unique_ptr<Node> control) {
    content.addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = Style::spaceXs, .fillWidth = true},
            ui::label(
                {.text = tr("inspector." + std::string(key)), .fontSize = Style::fontSizeCaption, .flexGrow = 1.0F}
            ),
            std::move(control)
        )
    );
  };
  const auto numeric = [&](std::string_view key, double value, bool degrees = false) {
    return ui::input(
        {.value = std::format("{:.2f}", degrees ? value * 180 / std::numbers::pi : value),
         .fontSize = Style::fontSizeCaption,
         .controlHeight = 32.0F,
         .minLayoutWidth = 110.0F,
         .width = 120.0F,
         .onSubmit =
             [this, &editor, key = std::string(key), degrees](const std::string& input) {
               double parsed = 0;
               const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
               if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || !std::isfinite(parsed)) {
                 m_status = tr("invalid-number");
                 editor.mutateDocument([&editor] { editor.requestLayout(); });
                 return;
               }
               editor.applySettingChange("item." + key, degrees ? parsed * std::numbers::pi / 180 : parsed);
             },
         .submitOnFocusLoss = true}
    );
  };
  const auto select = [&](std::string_view key, std::vector<std::string> labels, std::vector<std::string> values,
                          std::size_t selected) {
    return ui::select(
        {.options = std::move(labels),
         .selectedIndex = selected,
         .controlHeight = 32.0F,
         .onSelectionChanged =
             [&editor, key = std::string(key), values = std::move(values)](std::size_t index, std::string_view) {
               if (index < values.size()) {
                 editor.applySettingChange("item." + key, values[index]);
               }
             },
         .configure = [](Select& node) { node.setMinWidth(180); }}
    );
  };
  std::optional<GreeterAnchor> anchor;
  double x = 0, y = 0, width = item.boxWidth, height = item.boxHeight, rotation = item.rotationRad, scale = 1;
  if (isCore(item.id)) {
    GreeterCoreOverride core;
    if (profile().layout) {
      const auto it = profile().layout->components.find(item.id);
      if (it != profile().layout->components.end()) {
        core = it->second;
      }
    }
    anchor = core.anchor;
    x = core.x.value_or(0);
    y = core.y.value_or(0);
    width = coreSize(item.id).first;
    scale = core.scale.value_or(1);
    rotation = core.rotation.value_or(0);
    if (isRequired(item.id)) {
      row("sample",
          ui::select(
              {.options = std::vector<std::string>{tr("sample-full"), tr("sample-minimal")},
               .selectedIndex = m_compactSample ? 1U : 0U,
               .controlHeight = Style::controlHeightSm,
               .onSelectionChanged = [this, &editor](std::size_t index, std::string_view) {
                 editor.mutateDocument([this, &editor, index] {
                   flush();
                   m_compactSample = index == 1;
                   m_config->setStateBool("greeter_editor", "compact_sample", m_compactSample);
                   rebuildItems();
                   editor.requestLayout();
                 });
               }}
          ));
      const auto mode = profile().layout ? profile().layout->loginMode : std::nullopt;
      row("login-mode",
          select(
              "login_mode", {tr("inherit"), tr("auto"), tr("full"), tr("minimal")},
              {"inherit", "auto", "full", "minimal"}, mode ? static_cast<std::size_t>(*mode) + 1 : 0
          ));
    }
  } else {
    const auto widgets = effectiveWidgets();
    const auto it = std::ranges::find(widgets, item.id, &GreeterWidgetConfig::id);
    if (it == widgets.end()) {
      return;
    }
    const bool anchoredMode = std::holds_alternative<GreeterAnchorPlacement>(it->placement);
    row("placement",
        select("placement", {tr("anchored"), tr("relative")}, {"anchor", "relative"}, anchoredMode ? 0 : 1));
    if (const auto* placement = std::get_if<GreeterAnchorPlacement>(&it->placement)) {
      anchor = placement->anchor;
      x = placement->x;
      y = placement->y;
    } else {
      x = item.cx;
      y = item.cy;
    }
  }
  const bool anchorVisible = isCore(item.id) || anchor.has_value();
  if (anchorVisible) {
    std::vector<std::string> labels, values;
    std::size_t selected = 0;
    if (isCore(item.id)) {
      labels.push_back(tr("inherit"));
      values.emplace_back();
    }
    for (const auto value : kAnchors) {
      labels.push_back(tr("anchors." + std::string(value)));
      values.emplace_back(value);
    }
    if (anchor) {
      selected = static_cast<std::size_t>(*anchor) + (isCore(item.id) ? 1 : 0);
    }
    row("anchor", select("anchor", labels, values, selected));
  }
  row("x", numeric("x", x));
  row("y", numeric("y", y));
  row("width", numeric("width", width));
  if (!isCore(item.id)) {
    row("height", numeric("height", height));
  } else {
    row("scale", numeric("scale", scale));
  }
  row("rotation", numeric("rotation", rotation, true));
  content.addChild(
      ui::row(
          {.gap = Style::spaceSm, .paddingV = Style::spaceSm},
          ui::button(
              {.text = tr("inspector.reset-placement"),
               .glyph = "restore",
               .controlHeight = 32.0F,
               .variant = ButtonVariant::Default,
               .onClick =
                   [this, &editor, id = item.id] {
                     editor.mutateDocument([this, &editor, id] {
                       if (isCore(id)) {
                         resetItem(id);
                       } else {
                         (void)applyProperty(id, "item.reset_geometry", true);
                       }
                       editor.requestLayout();
                     });
                   }}
          ),
          ui::button(
              {.glyph = "flip-vertical",
               .controlHeight = 32.0F,
               .selected = item.flipX,
               .tooltip = tr("inspector.flip-x"),
               .onClick = [&editor] { editor.invoke(DesktopWidgetsEditor::Action::FlipX); }}
          ),
          ui::button(
              {.glyph = "flip-horizontal",
               .controlHeight = 32.0F,
               .selected = item.flipY,
               .tooltip = tr("inspector.flip-y"),
               .onClick = [&editor] { editor.invoke(DesktopWidgetsEditor::Action::FlipY); }}
          )
      )
  );
  if (item.type == "clock") {
    content.addChild(
        ui::label(
            {.text = tr("clock-preview-note"),
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxWidth = 450.0F,
             .maxLines = 0}
        )
    );
  }
}

void GreeterEditor::buildOverlay(
    Node& root, Renderer& renderer, DesktopWidgetsEditor& editor, const std::string& outputName, float width,
    float height
) {
  if (!m_config->config().wallpaper.enabled && width >= 1000 && height >= 600) {
    for (const auto& orbit :
         {std::array<float, 4>{-0.28F, 0.13F, 0.83F, 0.045F}, std::array<float, 4>{-0.27F, 0.16F, 0.78F, 0.035F}}) {
      auto circle = ui::box(
          {.fill = clearColorSpec(),
           .border = colorSpecFromRole(ColorRole::Secondary, orbit[3]),
           .borderWidth = 1.0F,
           .styleRadius = width}
      );
      circle->setHitTestVisible(false);
      circle->setPosition(width * orbit[0], height * orbit[1]);
      circle->setSize(width * orbit[2], width * orbit[2]);
      circle->setZIndex(0);
      root.addChild(std::move(circle));
    }
  }
  (void)renderer;
  (void)editor;
  (void)outputName;
}
