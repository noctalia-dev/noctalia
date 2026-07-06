#include "shell/dock/dock_instance.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_items.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"

#include <cmath>

namespace shell::dock {

  void prepareFrame(
      DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks, bool needsUpdate,
      bool needsLayout
  ) {
    if (instance.surface == nullptr) {
      return;
    }

    const auto width = instance.surface->width();
    const auto height = instance.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    deps.renderContext.makeCurrent(instance.surface->renderTarget());

    bool needsModelRebuild = false;
    if (needsUpdate || instance.sceneRoot == nullptr) {
      UiPhaseScope updatePhase(UiPhase::Update);
      if (callbacks.syncModel) {
        needsModelRebuild = callbacks.syncModel(instance);
      }
    }

    const bool needsSceneBuild = instance.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height;
    if (needsSceneBuild || needsLayout || needsModelRebuild) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      if (needsModelRebuild && instance.sceneRoot != nullptr && callbacks.rebuildItems) {
        callbacks.rebuildItems(instance);
      }
      buildScene(instance, deps, callbacks);
    } else if (needsUpdate && callbacks.updateVisuals) {
      callbacks.updateVisuals(instance);
    }
  }

  void syncDockSlideLayerTransform(DockInstance& instance, const DockConfig& cfg) {
    if (instance.slideRoot == nullptr) {
      return;
    }
    if (cfg.autoHide) {
      const float t = 1.0f - instance.hideOpacity;
      instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
    } else {
      instance.slideRoot->setPosition(0.0f, 0.0f);
    }
  }

  void applyDockCompositorBlur(DockInstance& instance, const DockConfig& cfg) {
    if (instance.surface == nullptr) {
      return;
    }
    // Compositor blur is independent of scene opacity — clear it while auto-hide
    // has faded the dock out so a transparent buffer does not leave a blur halo.
    constexpr float kBlurVisibleOpacity = 0.02f;
    if (cfg.autoHide && instance.hideOpacity < kBlurVisibleOpacity) {
      instance.surface->clearBlurRegion();
      return;
    }
    if (instance.panel == nullptr) {
      return;
    }
    const auto concave = dockConcaveShape(cfg);
    float absX = 0.0f;
    float absY = 0.0f;
    Node::absolutePosition(instance.panel, absX, absY);

    const float insetL = concave.logicalInset.left;
    const float insetT = concave.logicalInset.top;
    const float insetR = concave.logicalInset.right;
    const float insetB = concave.logicalInset.bottom;
    const int px = static_cast<int>(std::lround(absX + insetL));
    const int py = static_cast<int>(std::lround(absY + insetT));
    const int pw = static_cast<int>(std::lround(instance.panel->width() - insetL - insetR));
    const int ph = static_cast<int>(std::lround(instance.panel->height() - insetT - insetB));
    auto blurStrips = Surface::tessellateShape(px, py, pw, ph, concave.corners, concave.logicalInset, concave.radii);
    instance.surface->setBlurRegion(blurStrips);
  }

  void buildScene(DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks) {
    uiAssertNotRendering("shell::dock::buildScene");
    if (instance.surface == nullptr) {
      return;
    }

    const auto& cfg = deps.config.config().dock;
    const bool vert = shell::dock::isVerticalEdge(cfg.position);

    const auto w = static_cast<float>(instance.surface->width());
    const auto h = static_cast<float>(instance.surface->height());

    const auto& shadowConfig = deps.config.config().shell.shadow;
    const auto panelGeometry = shell::dock::computePanelGeometry(cfg, shadowConfig, w, h);
    const auto concave = shell::dock::dockConcaveShape(cfg);

    if (instance.sceneRoot == nullptr) {
      instance.sceneRoot = std::make_unique<Node>();
      instance.sceneRoot->setAnimationManager(&instance.animations);
      instance.sceneRoot->setSize(w, h);
      instance.sceneRoot->setOpacity(1.0f);

      auto slide = std::make_unique<Node>();
      slide->setParticipatesInLayout(false);
      instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

      // Shadow
      if (shell::surface_shadow::enabled(cfg.shadow, shadowConfig)) {
        instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      }

      // Panel background (icons render as a sibling so magnification can extend past the capsule).
      instance.panel = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [concave](Box& box) {
                box.setCornerShapes(concave.corners);
                box.setLogicalInset(concave.logicalInset);
                box.setRadii(concave.radii);
                box.setClipChildren(false);
              },
          })
      ));
      instance.panel->setZIndex(0);

      // Item row sits above the panel background without being clipped by it.
      instance.row = static_cast<Flex*>(instance.slideRoot->addChild(makeDockItemRow(cfg, vert)));
      instance.row->setZIndex(1);

      // Wire up InputDispatcher.
      instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
      instance.inputDispatcher.setCursorShapeCallback([&platform =
                                                           deps.platform](std::uint32_t serial, std::uint32_t shape) {
        platform.setCursorShape(serial, shape);
      });
      instance.inputDispatcher.setHoverChangeCallback([inst = &instance](InputArea* /*old*/, InputArea* next) {
        TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
      });

      // Populate items and wire up palette reactivity.
      if (callbacks.rebuildItems) {
        callbacks.rebuildItems(instance);
      }

      if (cfg.autoHide) {
        // Start off-screen (slide); opacity stays at 1 so the compositor blur matches the panel.
        instance.slideRoot->setOpacity(1.0f);
        instance.hideOpacity = 0.0f;
      } else {
        instance.slideRoot->setOpacity(0.0f);
        instance.hideOpacity = 1.0f;
        instance.animations.animate(
            0.0f, 1.0f, Style::animSlow, Easing::EaseOutCubic,
            [slide = instance.slideRoot](float v) { slide->setOpacity(v); }, {}, instance.slideRoot
        );
      }

      instance.surface->setSceneRoot(instance.sceneRoot.get());
    } else {
      // Update corner shapes/radii/inset on reconfigure.
      instance.panel->setCornerShapes(concave.corners);
      instance.panel->setLogicalInset(concave.logicalInset);
      instance.panel->setRadii(concave.radii);
    }

    // Update root size on reconfigure.
    instance.sceneRoot->setSize(w, h);
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setSize(w, h);
    }

    // Shadow follows the same shape as the background
    if (instance.shadow != nullptr) {
      const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
      const auto shadowOffsetX = static_cast<float>(shadowOff.x);
      const auto shadowOffsetY = static_cast<float>(shadowOff.y);
      const RoundedRectStyle shadowStyle = shell::surface_shadow::style(
          shadowConfig, cfg.backgroundOpacity,
          shell::surface_shadow::Shape{
              .corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii
          }
      );
      instance.shadow->setStyle(shadowStyle);
      instance.shadow->setZIndex(-1);
      instance.shadow->setPosition(
          panelGeometry.panelX - concave.logicalInset.left + shadowOffsetX,
          panelGeometry.panelY - concave.logicalInset.top + shadowOffsetY
      );
      instance.shadow->setSize(
          panelGeometry.panelW + concave.logicalInset.left + concave.logicalInset.right,
          panelGeometry.panelH + concave.logicalInset.top + concave.logicalInset.bottom
      );
    }

    // Panel
    applyPanelPalette(instance, cfg);
    instance.panel->setPosition(
        panelGeometry.panelX - concave.logicalInset.left, panelGeometry.panelY - concave.logicalInset.top
    );
    instance.panel->setSize(
        panelGeometry.panelW + concave.logicalInset.left + concave.logicalInset.right,
        panelGeometry.panelH + concave.logicalInset.top + concave.logicalInset.bottom
    );

    // Row matches the pill; hover spread is clamped to stay inside the background.
    instance.row->setPosition(panelGeometry.panelX, panelGeometry.panelY);
    instance.row->setSize(panelGeometry.panelW, panelGeometry.panelH);
    instance.row->layout(deps.renderContext);
    shell::dock::syncDockItemRestPositions(instance, cfg);

    if (cfg.autoHide) {
      const auto hiddenDelta = shell::dock::computeHiddenSlideDelta(cfg, shadowConfig, w, h, panelGeometry);
      instance.slideHiddenDx = hiddenDelta.first;
      instance.slideHiddenDy = hiddenDelta.second;
    } else {
      instance.slideHiddenDx = 0.0f;
      instance.slideHiddenDy = 0.0f;
    }
    syncDockSlideLayerTransform(instance, cfg);

    // Input region: trigger strip when hidden (autoHide), full panel otherwise.
    const bool hiddenInputRegion = cfg.autoHide && instance.hideOpacity < 0.5f;
    auto inputPanelGeometry = panelGeometry;
    if (!hiddenInputRegion && instance.slideRoot != nullptr) {
      inputPanelGeometry.panelX += instance.slideRoot->x();
      inputPanelGeometry.panelY += instance.slideRoot->y();
    }
    instance.surface->setInputRegion(
        shell::dock::computeInputRegion(
            cfg, inputPanelGeometry, static_cast<int>(w), static_cast<int>(h), hiddenInputRegion
        )
    );

    applyDockCompositorBlur(instance, cfg);

    // Palette reactivity.
    instance.paletteConn = paletteChanged().connect([inst = &instance, &config = deps.config] {
      applyPanelPalette(*inst, config.config().dock);
      if (inst->surface)
        inst->surface->requestRedraw();
    });

    if (callbacks.updateVisuals) {
      callbacks.updateVisuals(instance);
    }
  }

  void applyPanelPalette(DockInstance& instance, const DockConfig& cfg) {
    if (instance.panel == nullptr)
      return;
    const float opacity = cfg.backgroundOpacity;
    instance.panel->setFill(colorSpecFromRole(ColorRole::Surface, opacity));
    instance.panel->setBorder(colorSpecFromRole(ColorRole::Outline), 0.0f);
  }

  void resizeSurface(DockInstance& instance, const DockConfig& cfg, const ShellConfig::ShadowConfig& shadowConfig) {
    if (instance.surface == nullptr) {
      return;
    }

    const auto surfaceGeometry = shell::dock::computeSurfaceGeometry(
        cfg, shadowConfig, instance.items.size() + shell::dock::dockLauncherButtonCount(cfg)
    );

    if (instance.surface->width() != surfaceGeometry.surfaceW
        || instance.surface->height() != surfaceGeometry.surfaceH) {
      instance.surface->requestSize(surfaceGeometry.surfaceW, surfaceGeometry.surfaceH);
    }
  }

  void startHideFadeOut(DockInstance& inst, ConfigService& config) {
    if (inst.hideAnimId != 0) {
      inst.animations.cancel(inst.hideAnimId);
      inst.hideAnimId = 0;
    }
    const float current = inst.hideOpacity;
    inst.hideAnimId = inst.animations.animate(
        current, 0.0f, Style::animSlow, Easing::EaseInQuad,
        [&inst, &config](float v) {
          inst.hideOpacity = v;
          const auto& cfg = config.config().dock;
          syncDockSlideLayerTransform(inst, cfg);
          applyDockCompositorBlur(inst, cfg);
        },
        [&inst, &config]() {
          inst.hideAnimId = 0;
          if (inst.surface == nullptr)
            return;
          const auto& cfg = config.config().dock;
          int surfW = static_cast<int>(inst.surface->width());
          int surfH = static_cast<int>(inst.surface->height());
          if (surfW <= 0 || surfH <= 0) {
            const auto surfaceGeometry = shell::dock::computeSurfaceGeometry(
                cfg, config.config().shell.shadow, inst.items.size() + shell::dock::dockLauncherButtonCount(cfg)
            );
            surfW = static_cast<int>(surfaceGeometry.surfaceW);
            surfH = static_cast<int>(surfaceGeometry.surfaceH);
          }
          inst.surface->setInputRegion(
              shell::dock::computeInputRegion(cfg, shell::dock::DockPanelGeometry{}, surfW, surfH, true)
          );
        }
    );
    if (inst.surface)
      inst.surface->requestRedraw();
  }

} // namespace shell::dock
