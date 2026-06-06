#include "shell/lockscreen/lock_screen.h"

#include "capture/screencopy_util.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/lockscreen/lock_surface.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <string>
#include <wayland-client.h>

namespace {

  constexpr Logger kLog("lockscreen");

  Color resolveWallpaperFillColor(const WallpaperConfig& config) {
    // The lockscreen is an ext-session-lock surface: any transparency lets the
    // compositor's "client hasn't painted" fallback (e.g. niri's red) bleed
    // through. With no wallpaper image and no configured fill color, paint an
    // opaque black background so the lock surface is always fully opaque.
    if (!config.fillColor) {
      return rgba(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return resolveColorSpec(*config.fillColor);
  }

  const ext_session_lock_v1_listener kSessionLockListener = {
      .locked = &LockScreen::handleLocked,
      .finished = &LockScreen::handleFinished,
  };

} // namespace

LockScreen::LockScreen() = default;

LockScreen::~LockScreen() {
  clearInstances();
  resetLockState();
}

bool LockScreen::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService,
    SharedTextureCache* textureCache
) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_configService = configService;
  m_textureCache = textureCache;
  m_user = PamAuthenticator::currentUsername();
  return true;
}

void LockScreen::setSessionHooks(std::function<void()> onLocked, std::function<void()> onUnlocked) {
  m_onSessionLocked = std::move(onLocked);
  m_onSessionUnlocked = std::move(onUnlocked);
}

bool LockScreen::lock() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return false;
  }
  if (isActive()) {
    return true;
  }
  if (!m_wayland->hasSessionLockManager()) {
    kLog.warn("session lock protocol unavailable");
    return false;
  }

  m_desktopCaptures.clear();
  if (shouldUseBlurredDesktop()) {
    captureDesktopSnapshots();
  }

  m_lock = ext_session_lock_manager_v1_lock(m_wayland->sessionLockManager());
  if (m_lock == nullptr) {
    kLog.warn("failed to create session lock object");
    return false;
  }
  if (ext_session_lock_v1_add_listener(m_lock, &kSessionLockListener, this) != 0) {
    ext_session_lock_v1_destroy(m_lock);
    m_lock = nullptr;
    kLog.warn("failed to register session lock listener");
    return false;
  }

  m_lockPending = true;
  m_locked = false;
  clearSensitiveString(m_password);
  m_status = i18n::tr("lockscreen.waiting");
  m_statusIsError = false;
  syncInstances();
  if (m_instances.empty()) {
    kLog.warn("no outputs available for lock screen");
    resetLockState();
    return false;
  }
  wl_display_flush(m_wayland->display());
  kLog.info("session lock requested");
  return true;
}

void LockScreen::unlock() {
  if (!isActive()) {
    return;
  }

  m_pendingAfterLocked = {};

  const bool wasLockedInteractive = m_locked;

  if (m_lock != nullptr) {
    if (m_locked) {
      ext_session_lock_v1_unlock_and_destroy(m_lock);
      kLog.info("session unlock requested");
    } else {
      ext_session_lock_v1_destroy(m_lock);
      kLog.info("session lock request cancelled");
    }
    m_lock = nullptr;
  }

  m_lockPending = false;
  m_locked = false;
  clearSensitiveString(m_password);
  m_status.clear();
  m_statusIsError = false;
  m_wayland->stopKeyRepeat();
  m_desktopCaptures.clear();

  // Tear down widgets while lock surfaces still exist. Session hooks run only after
  // isActive() is false so LockscreenWidgetsController::applyVisibility() hides first.
  if (wasLockedInteractive && m_onSessionUnlocked) {
    m_onSessionUnlocked();
  }

  clearInstances();
  m_pointerSurface = nullptr;
  wl_display_flush(m_wayland->display());
}

void LockScreen::onFontChanged() { requestLayout(); }

void LockScreen::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst.surface != nullptr) {
      inst.surface->requestLayout();
    }
  }
}

void LockScreen::onOutputChange() {
  if (!isActive()) {
    return;
  }
  syncInstances();
}

void LockScreen::onSecondTick() {
  if (!isActive()) {
    return;
  }
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      instance.surface->onSecondTick();
    }
  }
}

void LockScreen::onThemeChanged() {
  if (!isActive()) {
    return;
  }
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      if (m_configService != nullptr) {
        instance.surface->setWallpaperFillColor(resolveWallpaperFillColor(m_configService->config().wallpaper));
      }
      instance.surface->onThemeChanged();
    }
  }
}

void LockScreen::onGpuResourcesInvalidated() {
  if (!isActive()) {
    return;
  }
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      instance.surface->onGpuResourcesInvalidated();
    }
  }
}

void LockScreen::onConfigChanged() {
  if (!isActive() || m_configService == nullptr) {
    return;
  }
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      applyLockscreenStyle(*instance.surface);
    }
  }
  applyWallpaperStyleToSurfaces();
}

void LockScreen::onWallpaperChanged() {
  if (!isActive() || m_configService == nullptr) {
    return;
  }
  if (!m_configService->config().lockscreen.wallpaper.empty()) {
    return;
  }
  applyWallpaperStyleToSurfaces();
  requestLayout();
}

void LockScreen::onPointerEvent(const PointerEvent& event) {
  if (!isActive()) {
    return;
  }

  if (event.type == PointerEvent::Type::Enter && event.surface != nullptr) {
    m_pointerSurface = event.surface;
  } else if (event.type == PointerEvent::Type::Leave && event.surface == m_pointerSurface) {
    m_pointerSurface = nullptr;
  } else if (
      (event.type == PointerEvent::Type::Button || event.type == PointerEvent::Type::Axis) && event.surface != nullptr
  ) {
    m_pointerSurface = event.surface;
  }

  wl_surface* target = event.surface != nullptr ? event.surface : m_pointerSurface;
  if (target == nullptr) {
    return;
  }

  for (auto& instance : m_instances) {
    if (instance.surface->wlSurface() == target) {
      instance.surface->onPointerEvent(event);
      return;
    }
  }
}

void LockScreen::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isActive()) {
    return;
  }
  if (!m_locked) {
    return;
  }
  if (!event.pressed) {
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, event.sym, event.modifiers)) {
    tryAuthenticate();
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    clearSensitiveString(m_password);
    m_status = i18n::tr("lockscreen.password-cleared");
    m_statusIsError = false;
    updatePromptOnSurfaces();
    return;
  }

  LockSurface* targetSurface = nullptr;
  if (m_pointerSurface != nullptr) {
    for (auto& instance : m_instances) {
      if (instance.surface != nullptr && instance.surface->wlSurface() == m_pointerSurface) {
        targetSurface = instance.surface.get();
        break;
      }
    }
  }
  if (targetSurface == nullptr) {
    for (auto& instance : m_instances) {
      if (instance.surface != nullptr) {
        targetSurface = instance.surface.get();
        break;
      }
    }
  }
  if (targetSurface != nullptr) {
    targetSurface->onKeyboardEvent(event);
  }
}

bool LockScreen::isActive() const noexcept { return m_lockPending || m_locked; }

bool LockScreen::isSessionLocked() const noexcept { return m_locked; }

void LockScreen::runAfterSessionLocked(std::function<void()> fn) {
  if (fn == nullptr) {
    return;
  }
  if (m_locked) {
    DeferredCall::callLater(std::move(fn));
    return;
  }
  m_pendingAfterLocked = std::move(fn);
  if (isActive()) {
    return;
  }
  if (!lock()) {
    m_pendingAfterLocked = {};
  }
}

void LockScreen::handleLocked(void* data, ext_session_lock_v1* /*lock*/) {
  auto* self = static_cast<LockScreen*>(data);
  self->m_lockPending = false;
  self->m_locked = true;
  self->m_status = i18n::tr("lockscreen.ready");
  self->m_statusIsError = false;
  for (auto& instance : self->m_instances) {
    instance.surface->setLockedState(true);
    instance.surface->setOnLogin([self]() { self->tryAuthenticate(); });
  }
  self->updatePromptOnSurfaces();
  kLog.info("session is locked");
  if (self->m_onSessionLocked) {
    self->m_onSessionLocked();
  }
  if (self->m_pendingAfterLocked) {
    auto pending = std::move(self->m_pendingAfterLocked);
    self->m_pendingAfterLocked = {};
    DeferredCall::callLater(std::move(pending));
  }
}

void LockScreen::handleFinished(void* data, ext_session_lock_v1* /*lock*/) {
  auto* self = static_cast<LockScreen*>(data);
  kLog.info("session lock finished by compositor");
  self->m_pendingAfterLocked = {};

  if (self->m_lock != nullptr) {
    if (self->m_locked) {
      ext_session_lock_v1_unlock_and_destroy(self->m_lock);
    } else {
      ext_session_lock_v1_destroy(self->m_lock);
    }
    self->m_lock = nullptr;
  }
  self->m_lockPending = false;
  self->m_locked = false;
  clearSensitiveString(self->m_password);
  self->m_status.clear();
  self->m_statusIsError = false;
  self->m_desktopCaptures.clear();
  if (self->m_onSessionUnlocked) {
    self->m_onSessionUnlocked();
  }
  self->clearInstances();
  self->m_pointerSurface = nullptr;
}

void LockScreen::syncInstances() {
  if (m_wayland == nullptr) {
    return;
  }

  const auto& outputs = m_wayland->outputs();

  std::erase_if(m_instances, [&](Instance& instance) {
    const bool exists = std::any_of(outputs.begin(), outputs.end(), [&](const WaylandOutput& output) {
      return output.name == instance.outputName;
    });
    if (!exists && instance.surface != nullptr && instance.surface->wlSurface() == m_pointerSurface) {
      m_pointerSurface = nullptr;
    }
    return !exists;
  });

  for (const auto& output : outputs) {
    const bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&](const Instance& instance) {
      return instance.outputName == output.name;
    });
    if (!exists) {
      createInstance(output);
    }
  }
}

bool LockScreen::shouldUseBlurredDesktop() const {
  return m_configService != nullptr
      && m_configService->config().lockscreen.blurredDesktop
      && m_wayland != nullptr
      && m_wayland->hasScreencopy();
}

void LockScreen::captureDesktopSnapshots() {
  if (m_wayland == nullptr) {
    return;
  }

  ScreencopyCapture capture(*m_wayland);
  if (!capture.available()) {
    kLog.warn("blurred lockscreen requested but screencopy is unavailable");
    return;
  }

  for (const auto& output : m_wayland->outputs()) {
    if (output.output == nullptr) {
      continue;
    }

    ScreencopyImage image;
    std::string error;
    if (!screencopy::captureOutputBlocking(capture, *m_wayland, output.output, image, error)) {
      kLog.warn("lockscreen desktop capture failed for {}: {}", output.connectorName, error);
      continue;
    }
    if (!screencopy::orientCaptureNative(image, *m_wayland, output.output)) {
      kLog.warn("lockscreen desktop capture orientation failed for {}", output.connectorName);
      continue;
    }
    m_desktopCaptures[output.output] = std::move(image);
  }
}

void LockScreen::applyLockscreenStyle(LockSurface& surface) const {
  if (m_configService == nullptr) {
    return;
  }
  const auto& lockscreen = m_configService->config().lockscreen;
  surface.setBackgroundStyle(lockscreen.blurIntensity, lockscreen.tintIntensity);
}

std::string LockScreen::wallpaperPathForOutput(const std::string& connectorName) const {
  if (m_configService == nullptr) {
    return {};
  }
  const std::string& customWallpaper = m_configService->config().lockscreen.wallpaper;
  if (!customWallpaper.empty()) {
    return customWallpaper;
  }
  return m_configService->getWallpaperPath(connectorName);
}

void LockScreen::applyWallpaperStyleToSurfaces() {
  if (!isActive() || m_configService == nullptr) {
    return;
  }
  const auto& wallpaperConfig = m_configService->config().wallpaper;
  const WallpaperFillMode fillMode = wallpaperConfig.fillMode;
  const Color fillColor = resolveWallpaperFillColor(wallpaperConfig);
  for (auto& instance : m_instances) {
    if (instance.surface == nullptr || instance.surface->hasDesktopCapture()) {
      continue;
    }
    instance.surface->setWallpaperPath(wallpaperPathForOutput(instance.connectorName));
    instance.surface->setWallpaperFillMode(fillMode);
    instance.surface->setWallpaperFillColor(fillColor);
  }
}

void LockScreen::createInstance(const WaylandOutput& output) {
  auto surface = std::make_unique<LockSurface>(*m_wayland, m_configService);
  surface->setRenderContext(m_renderContext);
  surface->setTextureCache(m_textureCache);
  surface->setLockedState(m_locked);
  applyLockscreenStyle(*surface);
  surface->setOutputKey(desktop_widgets::outputKey(output));
  if (m_configService != nullptr) {
    surface->setWallpaperPath(wallpaperPathForOutput(output.connectorName));
    surface->setWallpaperFillMode(m_configService->config().wallpaper.fillMode);
    surface->setWallpaperFillColor(resolveWallpaperFillColor(m_configService->config().wallpaper));
  }
  if (auto captureIt = m_desktopCaptures.find(output.output); captureIt != m_desktopCaptures.end()) {
    surface->setDesktopCapture(std::move(captureIt->second));
    m_desktopCaptures.erase(captureIt);
  }
  surface->setOnLogin([this]() { tryAuthenticate(); });
  surface->setOnPasswordChanged([this](const std::string& value) { handlePasswordEdited(value); });
  surface->setPromptState(m_user, m_password, m_status, m_statusIsError);

  if (!surface->initialize(m_lock, output.output, output.scale)) {
    kLog.warn("failed to create lock surface for output {}", output.name);
    return;
  }

  m_instances.push_back(
      Instance{
          .outputName = output.name,
          .output = output.output,
          .connectorName = output.connectorName,
          .surface = std::move(surface),
      }
  );
}

void LockScreen::resetLockState() {
  m_pendingAfterLocked = {};
  if (m_lock == nullptr) {
    m_lockPending = false;
    m_locked = false;
    return;
  }
  if (m_locked) {
    ext_session_lock_v1_unlock_and_destroy(m_lock);
  } else {
    ext_session_lock_v1_destroy(m_lock);
  }
  m_lock = nullptr;
  m_lockPending = false;
  m_locked = false;
}

void LockScreen::clearInstances() { m_instances.clear(); }

void LockScreen::updatePromptOnSurfaces() {
  for (auto& instance : m_instances) {
    instance.surface->setPromptState(m_user, m_password, m_status, m_statusIsError);
  }
}

void LockScreen::handlePasswordEdited(const std::string& value) {
  if (m_password == value && m_status.empty() && !m_statusIsError) {
    return;
  }
  m_password = value;
  m_status.clear();
  m_statusIsError = false;
  updatePromptOnSurfaces();
}

void LockScreen::tryAuthenticate() {
  m_status = i18n::tr("lockscreen.authenticating");
  m_statusIsError = false;
  updatePromptOnSurfaces();

  const auto result = m_authenticator.authenticateCurrentUser(m_password);
  clearSensitiveString(m_password);

  if (result.success) {
    m_status = i18n::tr("lockscreen.unlocked");
    m_statusIsError = false;
    updatePromptOnSurfaces();
    unlock();
    return;
  }

  m_status = result.message.empty() ? i18n::tr("lockscreen.authentication-failed") : result.message;
  m_statusIsError = true;
  updatePromptOnSurfaces();
}

void LockScreen::clearSensitiveString(std::string& value) {
  volatile char* ptr = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) {
    ptr[i] = '\0';
  }
  value.clear();
}
