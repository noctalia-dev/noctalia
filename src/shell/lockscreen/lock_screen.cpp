#include "shell/lockscreen/lock_screen.h"

#include "auth/fingerprint_authenticator.h"
#include "capture/screencopy_util.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/lockscreen/lock_surface.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>

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
  invalidatePendingAuthentication();
  clearInstances();
  resetLockState();
}

bool LockScreen::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService,
    SharedTextureCache* textureCache, SystemBus* systemBus, CompositorPlatform* compositorPlatform
) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_configService = configService;
  m_textureCache = textureCache;
  m_systemBus = systemBus;
  m_compositorPlatform = compositorPlatform;
  m_user = PamAuthenticator::currentUsername();

  if (m_systemBus != nullptr) {
    m_fingerprint = std::make_unique<FingerprintAuthenticator>(*m_systemBus);
    m_fingerprint->setAuthenticatedCallback([this]() {
      m_status = i18n::tr("lockscreen.unlocked");
      m_statusIsError = false;
      updatePromptOnSurfaces();
      unlock();
    });
    m_fingerprint->setStatusCallback([this](const std::string& message, bool isError) {
      handleFingerprintStatus(message, isError);
    });
  }
  return true;
}

void LockScreen::setSessionHooks(std::function<void()> onLocked, std::function<void()> onUnlocked) {
  m_onSessionLocked = std::move(onLocked);
  m_onSessionUnlocked = std::move(onUnlocked);
}

void LockScreen::setLockEngagedCallback(std::function<void()> callback) { m_onLockEngaged = std::move(callback); }

bool LockScreen::lock() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return false;
  }
  if (m_configService != nullptr && !m_configService->isLockScreenEnabled()) {
    kLog.debug("lock screen disabled");
    return false;
  }
  if (isActive()) {
    return true;
  }
  if (!m_wayland->hasSessionLockManager()) {
    kLog.warn("session lock protocol unavailable");
    return false;
  }
  if (m_wayland->outputs().empty()) {
    m_lockDeferred = true;
    kLog.warn("no outputs available for lock screen; lock deferred until an output is connected");
    // No output can ever show a lock surface, so run any pending post-lock action (e.g. suspend)
    // immediately instead of holding it until a monitor reconnects.
    dispatchPendingAfterLocked();
    return true;
  }

  if (m_desktopCapturesPrimed) {
    m_desktopCapturesPrimed = false;
  } else {
    m_desktopCaptures.clear();
    if (shouldUseBlurredDesktop()) {
      captureDesktopSnapshots();
    }
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
  if (m_onLockEngaged) {
    m_onLockEngaged();
  }
  return true;
}

void LockScreen::unlock() {
  m_lockDeferred = false;
  if (!isActive()) {
    return;
  }

  m_pendingAfterLocked = {};
  m_suspendTimeoutTimer.stop();
  invalidatePendingAuthentication();
  stopFingerprint();

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
  m_desktopCapturesPrimed = false;

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
  if (m_lockDeferred) {
    if (m_wayland != nullptr && !m_wayland->outputs().empty()) {
      m_lockDeferred = false;
      (void)lock();
    }
    return;
  }
  if (!isActive()) {
    return;
  }
  syncInstances();
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

void LockScreen::prepareForGraphicsReset() noexcept {
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      instance.surface->prepareForGraphicsReset();
    }
  }
}

void LockScreen::onConfigChanged() {
  if (m_configService == nullptr) {
    return;
  }
  if (!m_configService->isLockScreenEnabled()) {
    if (isActive()) {
      unlock();
    }
    return;
  }
  if (!isActive()) {
    return;
  }
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      applyLockscreenStyle(*instance.surface);
    }
  }
  applyOutputRestriction();
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

  // The password field always owns plain printable keys; Space is a Validate
  // chord but must type a space, not submit (passwords may contain spaces).
  if (!isPlainPrintableKey(event.utf32, event.modifiers, event.preedit)
      && KeybindMatcher::matches(KeybindAction::Validate, event.sym, event.modifiers)) {
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
      if (instance.surface != nullptr
          && !instance.surface->isBlackout()
          && instance.surface->wlSurface() == m_pointerSurface) {
        targetSurface = instance.surface.get();
        break;
      }
    }
  }
  if (targetSurface == nullptr) {
    for (auto& instance : m_instances) {
      if (instance.surface != nullptr && !instance.surface->isBlackout()) {
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

bool LockScreen::tryFlushPendingAfterLocked() {
  if (m_locked && m_pendingAfterLocked && allSurfacesReady()) {
    auto pending = std::move(m_pendingAfterLocked);
    m_pendingAfterLocked = {};
    m_suspendTimeoutTimer.stop();
    DeferredCall::callLater(std::move(pending));
    return true;
  }
  return false;
}

void LockScreen::dispatchPendingAfterLocked() {
  if (m_pendingAfterLocked) {
    auto pending = std::move(m_pendingAfterLocked);
    m_pendingAfterLocked = {};
    m_suspendTimeoutTimer.stop();
    DeferredCall::callLater(std::move(pending));
  }
}

void LockScreen::runAfterSessionLocked(std::function<void()> fn) {
  if (fn == nullptr) {
    return;
  }
  m_pendingAfterLocked = std::move(fn);
  if (tryFlushPendingAfterLocked()) {
    return;
  }
  if (isActive()) {
    return;
  }
  if (!lock()) {
    m_pendingAfterLocked = {};
  }
}

void LockScreen::handleLocked(void* data, ext_session_lock_v1* /*lock*/) {
  auto* self = static_cast<LockScreen*>(data);
  // Ignore locked events after unlock()/handleFinished() tore down the lock object.
  // A late event would re-engage the locked state with no matching unlock hook.
  if (self->m_lock == nullptr || !self->m_lockPending) {
    return;
  }
  self->m_lockPending = false;
  self->m_locked = true;
  // Idle status is empty; the surface renders the (togglable) password hint itself.
  self->m_status.clear();
  self->m_statusIsError = false;
  for (auto& instance : self->m_instances) {
    instance.surface->setLockedState(true);
    instance.surface->setOnLogin([self]() { self->tryAuthenticate(); });
  }

  // Start the fallback timer (3 seconds) to trigger suspend anyway if surfaces take too long to render
  self->m_suspendTimeoutTimer.start(std::chrono::seconds(3), [self]() {
    if (self->m_pendingAfterLocked) {
      kLog.warn("Lock screen surfaces took too long to render; suspending fallback triggered");
      auto pending = std::move(self->m_pendingAfterLocked);
      self->m_pendingAfterLocked = {};
      DeferredCall::callLater(std::move(pending));
    }
  });

  self->updatePromptOnSurfaces();
  self->updateIndicatorsOnSurfaces();
  self->startFingerprint();
  kLog.info("session is locked");
  if (self->m_onSessionLocked) {
    self->m_onSessionLocked();
  }
  self->tryFlushPendingAfterLocked();
}

void LockScreen::handleFinished(void* data, ext_session_lock_v1* /*lock*/) {
  auto* self = static_cast<LockScreen*>(data);
  kLog.info("session lock finished by compositor");
  self->m_pendingAfterLocked = {};
  self->invalidatePendingAuthentication();
  self->stopFingerprint();

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
  self->m_desktopCapturesPrimed = false;
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
    const auto it = std::ranges::find(outputs, instance.outputName, &WaylandOutput::name);
    const bool exists = it != outputs.end() && it->done && it->output != nullptr && it->hasUsableGeometry();
    if (!exists && instance.surface != nullptr && instance.surface->wlSurface() == m_pointerSurface) {
      m_pointerSurface = nullptr;
    }
    return !exists;
  });

  for (const auto& output : outputs) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    const bool exists = std::ranges::contains(m_instances, output.name, &Instance::outputName);
    if (!exists) {
      createInstance(output);
    }
  }

  applyOutputRestriction();
}

bool LockScreen::shouldUseBlurredDesktop() const {
  return m_configService != nullptr
      && m_configService->config().lockscreen.blurredDesktop
      && m_wayland != nullptr
      && m_wayland->hasScreencopy();
}

bool LockScreen::allSurfacesReady() const {
  if (m_instances.empty()) {
    return false;
  }
  for (const auto& instance : m_instances) {
    if (instance.surface != nullptr && !instance.surface->firstFrameRendered()) {
      return false;
    }
  }
  return true;
}

void LockScreen::primeDesktopCaptures() {
  if (m_configService != nullptr && !m_configService->isLockScreenEnabled()) {
    return;
  }
  if (isActive()) {
    return;
  }
  m_desktopCaptures.clear();
  m_desktopCapturesPrimed = false;
  if (!shouldUseBlurredDesktop()) {
    return;
  }
  captureDesktopSnapshots();
  m_desktopCapturesPrimed = true;
}

void LockScreen::clearPrimedDesktopCaptures() {
  if (!m_desktopCapturesPrimed) {
    return;
  }
  m_desktopCapturesPrimed = false;
  m_desktopCaptures.clear();
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
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry() || !isInteractiveOutput(output)) {
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

bool LockScreen::isInteractiveOutput(const WaylandOutput& output) const {
  if (m_configService == nullptr) {
    return true;
  }

  const auto& selectedMonitors = m_configService->config().lockscreen.monitors;
  if (selectedMonitors.empty()) {
    return true;
  }

  const bool anyConfiguredPresent =
      m_wayland != nullptr && std::ranges::any_of(m_wayland->outputs(), [&](const WaylandOutput& candidate) {
        return candidate.output != nullptr && std::ranges::any_of(selectedMonitors, [&](const std::string& match) {
                 return outputMatchesSelector(match, candidate);
               });
      });
  if (!anyConfiguredPresent) {
    return true;
  }

  return std::ranges::any_of(selectedMonitors, [&](const std::string& match) {
    return outputMatchesSelector(match, output);
  });
}

void LockScreen::applyOutputRestriction() {
  for (auto& instance : m_instances) {
    if (instance.surface == nullptr) {
      continue;
    }

    const WaylandOutput* output = nullptr;
    if (m_wayland != nullptr) {
      for (const auto& candidate : m_wayland->outputs()) {
        if (candidate.output == instance.output || candidate.connectorName == instance.connectorName) {
          output = &candidate;
          break;
        }
      }
    }

    const bool interactive = output != nullptr ? isInteractiveOutput(*output) : true;
    instance.surface->setBlackout(!interactive);
  }
  requestLayout();
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
    if (instance.surface == nullptr || instance.surface->isBlackout() || instance.surface->hasDesktopCapture()) {
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
  surface->setRenderCallback([this]() { tryFlushPendingAfterLocked(); });
  surface->setOnLogin([this]() { tryAuthenticate(); });
  surface->setOnCycleLayout([this]() { cycleKeyboardLayout(); });
  surface->setOnPasswordChanged([this](const std::string& value) { handlePasswordEdited(value); });
  surface->setPromptState(m_user, m_password, m_status, m_statusIsError, m_authenticating);
  applyIndicatorsToSurface(*surface);

  surface->setBlackout(!isInteractiveOutput(output));

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
  m_suspendTimeoutTimer.stop();
  m_lockDeferred = false;
  if (m_lock == nullptr) {
    m_lockPending = false;
    m_locked = false;
    return;
  }
  // unlock_and_destroy is required once the compositor may have entered the locked
  // state (including while m_lockPending is still true locally).
  ext_session_lock_v1_unlock_and_destroy(m_lock);
  m_lock = nullptr;
  m_lockPending = false;
  m_locked = false;
}

void LockScreen::clearInstances() { m_instances.clear(); }

void LockScreen::updatePromptOnSurfaces() {
  for (auto& instance : m_instances) {
    instance.surface->setPromptState(m_user, m_password, m_status, m_statusIsError, m_authenticating);
  }
}

void LockScreen::applyIndicatorsToSurface(LockSurface& surface) const {
  const bool capsLock = m_wayland != nullptr && m_wayland->keyboardLockKeysState().capsLock;
  bool hasMultipleLayouts = false;
  bool switchable = false;
  std::string layoutLabel;
  if (m_compositorPlatform != nullptr) {
    hasMultipleLayouts = m_compositorPlatform->keyboardLayoutNames().size() > 1;
    switchable = m_compositorPlatform->hasKeyboardLayoutBackend();
    std::string display = "short";
    std::unordered_map<std::string, std::string> customLabels;
    if (m_configService != nullptr) {
      if (const auto widgetIt = m_configService->config().widgets.find("keyboard_layout");
          widgetIt != m_configService->config().widgets.end()) {
        display = widgetIt->second.getString("display", display);
        customLabels = widgetIt->second.getStringMap("custom_labels");
      }
    }
    layoutLabel = KeyboardLayoutWidget::resolveLayoutLabel(
        m_compositorPlatform->currentKeyboardLayoutName(), KeyboardLayoutWidget::parseDisplayMode(display), customLabels
    );
  }
  surface.setKeyboardIndicators(capsLock, hasMultipleLayouts, switchable, std::move(layoutLabel));
}

void LockScreen::updateIndicatorsOnSurfaces() {
  for (auto& instance : m_instances) {
    if (instance.surface != nullptr) {
      applyIndicatorsToSurface(*instance.surface);
    }
  }
}

void LockScreen::cycleKeyboardLayout() {
  if (m_compositorPlatform == nullptr || !m_compositorPlatform->cycleKeyboardLayout()) {
    return;
  }
  updateIndicatorsOnSurfaces();
}

void LockScreen::onLockKeysChanged() {
  if (!isActive()) {
    return;
  }
  updateIndicatorsOnSurfaces();
}

void LockScreen::onKeyboardLayoutChanged() {
  if (!isActive()) {
    return;
  }
  updateIndicatorsOnSurfaces();
}

void LockScreen::invalidatePendingAuthentication() {
  ++m_authGeneration;
  m_authenticating = false;
}

void LockScreen::handlePasswordEdited(const std::string& value) {
  if (m_authenticating) {
    m_password = value;
    updatePromptOnSurfaces();
    return;
  }
  if (m_password == value && m_status.empty() && !m_statusIsError) {
    return;
  }
  m_password = value;
  m_status.clear();
  m_statusIsError = false;
  updatePromptOnSurfaces();
}

void LockScreen::tryAuthenticate() {
  if (m_authenticating || !m_locked) {
    return;
  }
  if (m_password.empty()) {
    const bool allowEmptyPassword =
        m_configService != nullptr && m_configService->config().lockscreen.allowEmptyPassword;
    if (!allowEmptyPassword) {
      return;
    }
  }

  stopFingerprint();
  if (m_wayland != nullptr) {
    m_wayland->stopKeyRepeat();
  }

  std::string password = m_password;
  clearSensitiveString(m_password);

  const std::uint64_t generation = ++m_authGeneration;
  m_authenticating = true;
  m_status = i18n::tr("lockscreen.authenticating");
  m_statusIsError = false;
  updatePromptOnSurfaces();

  const PamAuthenticator authenticator = m_authenticator;
  // Authenticate against the "login" stack. If fingerprint is enabled, strip
  // pam_fprintd from it: noctalia drives the reader itself over D-Bus and the
  // two can't share the sensor. See docs/fingerprint.md.
  const std::string pamService = "login";
  std::thread([this, generation, password = std::move(password), authenticator, pamService]() mutable {
    const auto result = authenticator.authenticateCurrentUser(password, pamService);
    clearSensitiveString(password);
    DeferredCall::callLater([this, generation, result]() { handleAuthResult(generation, result); });
  }).detach();
}

void LockScreen::handleAuthResult(std::uint64_t generation, PamAuthenticator::Result result) {
  if (generation != m_authGeneration || !m_locked) {
    return;
  }

  m_authenticating = false;

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
  startFingerprint();
}

void LockScreen::startFingerprint() {
  if (m_fingerprint == nullptr) {
    return;
  }
  if (m_configService != nullptr && !m_configService->config().lockscreen.fingerprint) {
    return;
  }
  m_fingerprint->start();
}

void LockScreen::stopFingerprint() {
  if (m_fingerprint != nullptr) {
    m_fingerprint->stop();
  }
}

void LockScreen::handleFingerprintStatus(const std::string& message, bool isError) {
  if (!isActive()) {
    return;
  }
  // Don't clobber a password the user is typing.
  if (!m_password.empty()) {
    return;
  }
  // Empty message means verification disarmed; fall back to the idle prompt (rendered by the surface).
  m_status = message.empty() ? std::string{} : message;
  m_statusIsError = isError;
  updatePromptOnSurfaces();
}

void LockScreen::clearSensitiveString(std::string& value) {
  volatile char* ptr = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) {
    ptr[i] = '\0';
  }
  value.clear();
}
