#include "shell/backdrop/backdrop.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/shared_texture_cache.h"
#include "shell/backdrop/backdrop_instance.h"
#include "shell/backdrop/backdrop_surface.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("backdrop");

} // namespace

Backdrop::Backdrop() = default;
Backdrop::~Backdrop() = default;

bool Backdrop::isSupportedForCurrentCompositor() const { return m_wayland != nullptr && compositors::isNiri(); }

bool Backdrop::shouldHaveInstances() const {
  if (m_config == nullptr || !m_config->config().backdrop.enabled) {
    return false;
  }
  return isSupportedForCurrentCompositor();
}

void Backdrop::destroyInstances() {
  for (auto& inst : m_instances) {
    releaseInstanceTexture(*inst);
  }
  m_instances.clear();
}

bool Backdrop::initialize(
    WaylandConnection& wayland, ConfigService* config, SharedTextureCache* textureCache, GlSharedContext* sharedGl
) {
  m_wayland = &wayland;
  m_config = config;
  m_textureCache = textureCache;
  m_sharedGl = sharedGl;

  // Register reload callback unconditionally so toggling enabled in config works.
  m_config->addReloadCallback([this]() { reload(); }, "backdrop");

  if (!m_config->config().backdrop.enabled) {
    kLog.info("disabled in config");
    return true;
  }

  cacheReloadBaseline();
  return true;
}

void Backdrop::cacheReloadBaseline() {
  if (m_config == nullptr) {
    return;
  }
  const auto& cfg = m_config->config();
  m_lastBackdropConfig = cfg.backdrop;
  m_lastShouldHaveInstances = shouldHaveInstances();
  m_lastWallpaperEnabled = cfg.wallpaper.enabled;
  m_lastWallpaperFillMode = cfg.wallpaper.fillMode;
}

void Backdrop::reload() {
  if (m_config == nullptr) {
    return;
  }

  const auto& cfg = m_config->config();
  const bool shouldInstances = shouldHaveInstances();
  const bool recreateNeeded = cfg.backdrop != m_lastBackdropConfig
      || shouldInstances != m_lastShouldHaveInstances
      || cfg.wallpaper.enabled != m_lastWallpaperEnabled;

  if (!recreateNeeded
      && cfg.backdrop.enabled
      && shouldInstances
      && !m_instances.empty()
      && cfg.wallpaper.fillMode == m_lastWallpaperFillMode) {
    return;
  }

  cacheReloadBaseline();

  if (!cfg.backdrop.enabled || !shouldInstances) {
    if (!m_instances.empty()) {
      kLog.info("reloading config");
      destroyInstances();
    }
    return;
  }

  if (!recreateNeeded) {
    for (auto& inst : m_instances) {
      updateRendererState(*inst);
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
      }
    }
    return;
  }

  kLog.info("reloading config");

  // Full teardown is required when backdrop or wallpaper enablement changes because
  // a wallpaper enable/disable cycle resets the shared texture context.
  destroyInstances();
  syncInstances();
}

void Backdrop::onOutputChange() {
  if (m_config == nullptr || !m_config->config().backdrop.enabled || !shouldHaveInstances()) {
    return;
  }
  syncInstances();
}

void Backdrop::onStateChange() {
  kLog.info("state changed, checking wallpaper updates");

  for (auto& inst : m_instances) {
    auto newPath = m_config->getWallpaperPath(inst->connectorName);
    if (newPath.empty() || newPath == inst->currentPath) {
      continue;
    }

    kLog.info("updating {} → {}", inst->connectorName, newPath);
    releaseInstanceTexture(*inst);
    loadWallpaper(*inst, newPath);
  }
}

void Backdrop::onThemeChanged() {
  if (!shouldHaveInstances()) {
    return;
  }
  for (auto& inst : m_instances) {
    updateRendererState(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Backdrop::onGpuResourcesInvalidated() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->onGpuResourcesInvalidated();
    }
    if (!inst->currentPath.empty()) {
      if (m_textureCache != nullptr && m_textureCache->shared()) {
        inst->currentTexture = m_textureCache->peek(inst->currentPath);
      } else if (inst->surface != nullptr) {
        auto* renderer = inst->surface->wallpaperRenderer();
        if (renderer != nullptr && renderer->backend() != nullptr) {
          renderer->backend()->makeCurrentNoSurface();
          if (inst->currentTexture.id != 0) {
            renderer->backend()->textureManager().unload(inst->currentTexture);
          }
          inst->currentTexture = renderer->backend()->textureManager().loadFromFile(inst->currentPath, 0, true);
        }
      }
    }
    updateRendererState(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Backdrop::prepareForGraphicsReset() noexcept {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->prepareForGraphicsReset();
    }
    inst->currentTexture = {};
  }
}

void Backdrop::restoreAfterGraphicsReset() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->restoreAfterGraphicsReset();
    }
  }
}

void Backdrop::finishGraphicsResetRecovery() noexcept {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->finishGraphicsResetRecovery();
    }
  }
}

void Backdrop::onFontChanged() { requestLayout(); }

void Backdrop::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Backdrop::syncInstances() {
  const auto& outputs = m_wayland->outputs();

  // Remove instances for outputs that no longer exist
  std::erase_if(m_instances, [&](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    if (!found) {
      kLog.info("removing instance for output {}", inst->outputName);
      releaseInstanceTexture(*inst);
    }
    return !found;
  });

  // Create instances for new outputs
  for (const auto& output : outputs) {
    if (!output.done || output.connectorName.empty() || !output.hasUsableGeometry()) {
      continue;
    }

    bool exists =
        std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->outputName == output.name; });
    if (!exists) {
      createInstance(output);
    }
  }
}

void Backdrop::createInstance(const WaylandOutput& output) {
  auto wallpaperPath = m_config->getWallpaperPath(output.connectorName);
  kLog.info("creating on {} ({}), path={}", output.connectorName, output.description, wallpaperPath);

  auto inst = std::make_unique<BackdropInstance>();
  inst->outputName = output.name;
  inst->output = output.output;
  inst->scale = output.scale;
  inst->connectorName = output.connectorName;

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-backdrop",
      .layer = LayerShellLayer::Background,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = 0,
      .exclusiveZone = -1,
  };

  inst->surface = std::make_unique<BackdropSurface>(*m_wayland, std::move(surfaceConfig));
  inst->surface->setSharedGl(m_sharedGl);
  inst->surface->setClickThrough(true);

  updateRendererState(*inst);

  auto* instPtr = inst.get();
  inst->surface->setConfigureCallback([this, instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    if (instPtr->currentTexture.id != 0 || !shouldHaveInstances() || m_config == nullptr) {
      return;
    }
    std::string path = instPtr->currentPath;
    if (path.empty()) {
      path = m_config->getWallpaperPath(instPtr->connectorName);
    }
    if (!path.empty()) {
      loadWallpaper(*instPtr, path);
    }
  });

  if (!inst->surface->initialize(output.output)) {
    kLog.warn("failed to initialize backdrop surface for output {}", output.name);
    return;
  }

  if (shouldHaveInstances() && !wallpaperPath.empty()) {
    loadWallpaper(*inst, wallpaperPath);
  }

  m_instances.push_back(std::move(inst));
}

void Backdrop::loadWallpaper(BackdropInstance& inst, const std::string& path) {
  auto tex = m_textureCache->acquire(path);
  if (tex.id == 0 && !m_textureCache->shared() && inst.surface != nullptr) {
    auto* renderer = inst.surface->wallpaperRenderer();
    if (renderer != nullptr && renderer->backend() != nullptr) {
      renderer->backend()->makeCurrentNoSurface();
      tex = renderer->backend()->textureManager().loadFromFile(path, 0, true);
    }
  }
  if (tex.id == 0) {
    kLog.warn("failed to load {}", path);
    return;
  }

  inst.currentTexture = tex;
  inst.currentPath = path;
  updateRendererState(inst);
  if (inst.surface != nullptr) {
    inst.surface->requestRedraw();
  }
}

void Backdrop::updateRendererState(BackdropInstance& inst) {
  if (inst.surface == nullptr) {
    return;
  }

  const auto& ov = m_config->config().backdrop;
  inst.surface->setBlurIntensity(ov.blurIntensity);
  inst.surface->setTintIntensity(ov.tintIntensity);

  // Tint color from the current surface role.
  const Color surface = colorForRole(ColorRole::Surface);
  inst.surface->setTintColor(surface.r, surface.g, surface.b);

  if (inst.currentTexture.id != 0) {
    inst.surface->setWallpaperState(
        inst.currentTexture.id, static_cast<float>(inst.currentTexture.width),
        static_cast<float>(inst.currentTexture.height), m_config->config().wallpaper.fillMode
    );
  } else {
    inst.surface->setWallpaperState({}, 0.0f, 0.0f, m_config->config().wallpaper.fillMode);
  }
}

void Backdrop::releaseInstanceTexture(BackdropInstance& inst, bool clearPath) {
  if (inst.currentTexture.id != 0) {
    if (m_textureCache->shared()) {
      m_textureCache->release(inst.currentTexture, inst.currentPath);
    } else if (inst.surface != nullptr) {
      auto* renderer = inst.surface->wallpaperRenderer();
      if (renderer != nullptr && renderer->backend() != nullptr) {
        renderer->backend()->makeCurrentNoSurface();
        renderer->backend()->textureManager().unload(inst.currentTexture);
      }
    }
    inst.currentTexture = {};
  }
  if (clearPath) {
    inst.currentPath.clear();
  }
}
