#include "shell/backdrop/backdrop.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "core/log.h"
#include "render/core/shared_texture_cache.h"
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
  m_config->addReloadCallback([this]() { reload(); });

  if (!m_config->config().backdrop.enabled) {
    kLog.info("disabled in config");
    return true;
  }

  if (shouldHaveInstances()) {
    syncInstances();
  }
  return true;
}

void Backdrop::reload() {
  kLog.info("reloading config");

  // Always tear down existing instances. This is necessary because a
  // wallpaper enable/disable cycle resets the wallpaper share context, and any
  // backdrop instances created against the old context cannot access the new
  // textures. Full teardown + recreate is safe since backdrop surfaces are
  // hidden by the compositor outside of overview mode (no visible flash).
  destroyInstances();

  if (!m_config->config().backdrop.enabled) {
    return;
  }

  if (shouldHaveInstances()) {
    syncInstances();
  }
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
    bool found =
        std::any_of(outputs.begin(), outputs.end(), [&inst](const auto& out) { return out.name == inst->outputName; });
    if (!found) {
      kLog.info("removing instance for output {}", inst->outputName);
      releaseInstanceTexture(*inst);
    }
    return !found;
  });

  // Create instances for new outputs
  for (const auto& output : outputs) {
    if (!output.done || output.connectorName.empty()) {
      continue;
    }

    bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&output](const auto& inst) {
      return inst->outputName == output.name;
    });
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
    m_textureCache->release(inst.currentTexture, inst.currentPath);
    inst.currentTexture = {};
  }
  if (clearPath) {
    inst.currentPath.clear();
  }
}
