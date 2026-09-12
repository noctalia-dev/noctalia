#include "shell/settings/display/monitor_identifier_overlay.h"

#include "core/log.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace settings::display {

  namespace {

    constexpr Logger kLog("display_overlay");

  } // namespace

  MonitorIdentifierOverlay::MonitorIdentifierOverlay() = default;

  MonitorIdentifierOverlay::~MonitorIdentifierOverlay() { destroySurfaces(); }

  void MonitorIdentifierOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void MonitorIdentifierOverlay::show() {
    if (m_visible) {
      return;
    }
    m_visible = true;
    ensureSurfaces();
  }

  void MonitorIdentifierOverlay::hide() {
    m_visible = false;
    destroySurfaces();
  }

  void MonitorIdentifierOverlay::onOutputChange() {
    if (!m_visible || m_wayland == nullptr) {
      return;
    }
    std::vector<std::string> sortedNames;
    for (const auto& output : m_wayland->outputs()) {
      if (output.done && output.output != nullptr && output.hasUsableGeometry()) {
        sortedNames.push_back(output.connectorName);
      }
    }
    std::sort(sortedNames.begin(), sortedNames.end());
    std::string signature;
    for (const auto& name : sortedNames) {
      signature += name;
      signature.push_back(',');
    }
    if (signature == m_signature) {
      return;
    }
    destroySurfaces();
    ensureSurfaces();
  }

  void MonitorIdentifierOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr) {
      return;
    }

    std::vector<std::string> sortedNames;
    for (const auto& output : m_wayland->outputs()) {
      if (output.done && output.output != nullptr && output.hasUsableGeometry()) {
        sortedNames.push_back(output.connectorName);
      }
    }
    std::sort(sortedNames.begin(), sortedNames.end());
    m_signature.clear();
    for (const auto& name : sortedNames) {
      m_signature += name;
      m_signature.push_back(',');
    }

    for (const auto& output : m_wayland->outputs()) {
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      const auto nameIt = std::ranges::find(sortedNames, output.connectorName);
      if (nameIt == sortedNames.end()) {
        continue;
      }
      const int index = static_cast<int>(nameIt - sortedNames.begin()) + 1;

      auto instance = std::make_unique<Instance>();
      instance->output = output.output;
      instance->scale = output.scale;

      auto surfaceConfig = LayerSurfaceConfig{
          .nameSpace = "noctalia-display-identifiers",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Left,
          .width = 64,
          .height = 40,
          .exclusiveZone = 0,
          .marginTop = 12,
          .marginLeft = 12,
          .keyboard = LayerShellKeyboard::None,
          .defaultWidth = 64,
          .defaultHeight = 40,
      };
      instance->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
      instance->surface->setRenderContext(m_renderContext);
      auto* instancePtr = instance.get();
      instance->surface->setConfigureCallback([instancePtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
        instancePtr->surface->requestLayout();
      });
      instance->surface->setPrepareFrameCallback([this, instancePtr](bool needsUpdate, bool needsLayout) {
        prepareFrame(*instancePtr, needsUpdate, needsLayout);
      });
      if (!instance->surface->initialize(output.output)) {
        kLog.warn("monitor identifier overlay: failed to initialize surface on {}", output.connectorName);
        continue;
      }
      instance->surface->setInputRegion({});

      instance->sceneRoot = ui::column({
          .align = FlexAlign::Start,
          .justify = FlexJustify::Start,
      });
      auto badge = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .padding = 10.0F * static_cast<float>(output.scale),
          .configure = [](Flex& box) {
            box.setRadius(Style::radiusMd);
            box.setFill(colorSpecFromRole(ColorRole::Surface, 0.9F));
            box.setBorder(colorSpecFromRole(ColorRole::Primary), 2.0F);
          },
      });
      badge->addChild(
          ui::label({
              .text = std::to_string(index) + " " + output.connectorName,
              .fontSize = Style::fontSizeHeader * static_cast<float>(output.scale),
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          })
      );
      instance->sceneRoot->addChild(std::move(badge));
      m_instances.push_back(std::move(instance));
    }
  }

  void MonitorIdentifierOverlay::destroySurfaces() { m_instances.clear(); }

  void MonitorIdentifierOverlay::buildScene(Instance& instance) {
    if (m_renderContext == nullptr || instance.surface == nullptr) {
      return;
    }
    Renderer& renderer = instance.surface->renderTarget().renderer();
    const float width = static_cast<float>(instance.surface->width());
    const float height = static_cast<float>(instance.surface->height());
    if (width <= 0.0F || height <= 0.0F) {
      return;
    }
    LayoutConstraints constraints;
    constraints.setMaxWidth(width);
    constraints.setMaxHeight(height);
    const auto size = instance.sceneRoot->measure(renderer, constraints);
    const std::uint32_t neededW = static_cast<std::uint32_t>(std::ceil(size.width));
    const std::uint32_t neededH = static_cast<std::uint32_t>(std::ceil(size.height));
    if (neededW != instance.surface->width() || neededH != instance.surface->height()) {
      instance.surface->requestSize(neededW, neededH);
      return;
    }
    instance.sceneRoot->arrange(renderer, {.x = 0.0F, .y = 0.0F, .width = size.width, .height = size.height});
  }

  void MonitorIdentifierOverlay::prepareFrame(Instance& instance, bool /*needsUpdate*/, bool /*needsLayout*/) {
    if (m_renderContext == nullptr || instance.surface == nullptr || instance.sceneRoot == nullptr) {
      return;
    }
    m_renderContext->makeCurrent(instance.surface->renderTarget());
    buildScene(instance);
  }

} // namespace settings::display
