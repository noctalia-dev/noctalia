#pragma once

#include "auth/pam_authenticator.h"
#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"
#include "render/core/lockscreen_transition_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ScreencopyImage;

struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct ext_session_lock_v1;
struct wl_surface;
struct wl_output;
class ConfigService;

class CompositorPlatform;
class FingerprintAuthenticator;
class HttpClient;
class LockSurface;
class MprisService;
class RenderContext;
class SessionActionRunner;
class SharedTextureCache;
class SystemBus;
class WaylandConnection;
class WeatherService;
enum class LockscreenTransition : std::uint8_t;

class LockScreen {
public:
  LockScreen();
  ~LockScreen();

  bool initialize(
      WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService,
      SharedTextureCache* textureCache, SystemBus* systemBus, CompositorPlatform* compositorPlatform
  );
  void setSessionHooks(
      std::function<void()> onLocked, std::function<void()> onUnlocked, std::function<void()> onLockAborted,
      std::function<void()> onReadyForSleep
  );
  void setLoginBoxServices(
      SessionActionRunner* sessionActions, MprisService* mpris, const WeatherService* weather, HttpClient* httpClient
  );
  bool lock();
  void primeDesktopCaptures();
  void clearPrimedDesktopCaptures();
  void unlock();
  void onOutputChange();
  void onFontChanged();
  void onThemeChanged();
  void onGpuResourcesInvalidated();
  void prepareForGraphicsReset() noexcept;
  void onWallpaperChanged();
  void onConfigChanged();
  void onLockKeysChanged();
  void onKeyboardLayoutChanged();
  void requestLayout();
  void requestUpdate();
  /// After suspend/resume, discard pending callbacks on active lock surfaces
  /// while preserving queued work, then request an immediate redraw.
  void forceRepaintAfterResume();
  /// After suspend/resume, revoke the passwordless grace period as a safety net;
  /// the boottime expiry check alone would also have expired it by then.
  void onSystemResumed();
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] bool isActive() const noexcept;
  [[nodiscard]] bool isSessionLocked() const noexcept;
  [[nodiscard]] bool isReadyForSleep() const;
  /// Revoke passwordless unlock and wait for an uncovered lock frame before sleep.
  void prepareForSleep();
  /// Finish entry and acknowledge the safe lock buffers before powering off.
  /// Calls `done(false)` if cancelled or no safe frame arrives before the deadline.
  void prepareForOutputPowerOff(std::function<void(bool)> done);
  void cancelOutputPowerOff();
  void onOutputPowerChanged();
  /// Outputs were powered back on; keys right after this are wake keys.
  void markOutputsWoke();

  template <typename Fn> void forEachSurface(Fn&& fn) {
    for (auto& instance : m_instances) {
      if (instance.surface != nullptr) {
        fn(*instance.surface);
      }
    }
  }

  /// Runs `fn` once every lock surface has acknowledged its fully revealed frame.
  /// Cleared if lock fails, unlocks, aborts, or the system resumes before dispatch.
  void runAfterSessionLocked(std::function<void()> fn);

  /// Revokes the grace window: in-flight grace unlock stops working, and arming is
  /// cleared for a lock already pending or engaged (sleep transitions call this).
  void resetGracePeriod();

  static void handleLocked(void* data, ext_session_lock_v1* lock);
  static void handleFinished(void* data, ext_session_lock_v1* lock);

private:
  struct Instance {
    std::uint32_t outputName = 0;
    wl_output* output = nullptr;
    std::string connectorName;
    std::unique_ptr<LockSurface> surface;
  };

  void syncInstances();
  [[nodiscard]] bool captureDesktopSnapshots();
  void invalidateDesktopCaptures();
  [[nodiscard]] bool shouldCaptureDesktop() const;
  [[nodiscard]] bool allSurfacesReady() const;
  bool tryFlushPendingAfterLocked();
  void dispatchPendingAfterLocked(bool timedOut = false);
  void cancelPendingAfterLocked();
  void armSleepReadyTimeout();
  void handleSleepReady();
  void suppressEntryTransition();
  void flushPendingOutputPowerOff();
  void armOutputPowerOffTimeout();
  void applyLockscreenStyle(LockSurface& surface) const;
  void applyOutputRestriction();
  void applyWallpaperStyleToSurfaces();
  [[nodiscard]] bool isInteractiveOutput(const WaylandOutput& output) const;
  [[nodiscard]] std::string wallpaperPathForOutput(const std::string& connectorName) const;
  void createInstance(const WaylandOutput& output);
  void chooseTransition();
  void handleTransitionStateChanged();
  void tryFinishAnimatedUnlock();
  void finishUnlock();
  void notifyLockAborted();
  void resetLockState();
  void clearInstances();
  void updatePromptOnSurfaces();
  void updateIndicatorsOnSurfaces();
  void applyIndicatorsToSurface(LockSurface& surface) const;
  void cycleKeyboardLayout();
  void handlePasswordEdited(const std::string& value);
  void tryAuthenticate();
  void handleAuthResult(std::uint64_t generation, PamAuthenticator::Result result);
  void invalidatePendingAuthentication();
  void startFingerprint();
  void stopFingerprint();
  void handleFingerprintStatus(const std::string& message, bool isError);
  static void clearSensitiveString(std::string& value);
  /// Passwordless grace window: any keypress or pointer movement beyond 5px unlocks
  /// without a password while the window is live (matches hyprlock).
  [[nodiscard]] bool isInGracePeriod() const noexcept;
  void tryGraceUnlock();
  /// Runs the lock-screen action bound to this chord. True when the chord is bound
  /// (even if suppressed as a wake key), so the key is consumed.
  bool tryActionShortcut(std::uint32_t sym, std::uint32_t modifiers);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  ConfigService* m_configService = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  SystemBus* m_systemBus = nullptr;
  CompositorPlatform* m_compositorPlatform = nullptr;
  ext_session_lock_v1* m_lock = nullptr;
  std::vector<Instance> m_instances;
  std::unordered_map<wl_output*, ScreencopyImage> m_desktopCaptures;
  PamAuthenticator m_authenticator;
  std::unique_ptr<FingerprintAuthenticator> m_fingerprint;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  wl_surface* m_pointerSurface = nullptr;
  bool m_statusIsError = false;
  bool m_authenticating = false;
  std::uint64_t m_authGeneration = 0;
  bool m_lockPending = false;
  bool m_locked = false;
  bool m_lockStarting = false;
  bool m_desktopCapturesPrimed = false;
  std::uint64_t m_desktopCaptureGeneration = 0;
  bool m_lockDeferred = false;
  bool m_transitionSuppressed = false;
  bool m_unlocking = false;
  bool m_unlockFinishQueued = false;
  std::optional<LockscreenTransition> m_activeTransition;
  LockscreenTransitionParams m_transitionParams;
  float m_transitionDurationMs = 1500.0F;
  std::function<void()> m_pendingAfterLocked;
  std::uint64_t m_afterLockedGeneration = 0;
  std::function<void(bool)> m_pendingOutputPowerOff;
  std::uint64_t m_outputPowerOffGeneration = 0;
  bool m_outputPowerOffQueued = false;
  std::function<void()> m_onSessionLocked;
  std::function<void()> m_onSessionUnlocked;
  std::function<void()> m_onLockAborted;
  std::function<void()> m_onReadyForSleep;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  SessionActionRunner* m_sessionActions = nullptr;
  MprisService* m_mpris = nullptr;
  const WeatherService* m_weather = nullptr;
  HttpClient* m_httpClient = nullptr;
  Timer m_suspendTimeoutTimer;
  Timer m_outputPowerOffTimer;
  // CLOCK_BOOTTIME millis when the lock flow started; grace expires gracePeriodSeconds after it.
  std::int64_t m_lockedAtMillis = 0;
  bool m_graceAllowed = false;
  // CLOCK_BOOTTIME millis of the last resume or output power-on; guards wake keys.
  std::int64_t m_wokeAtMillis = 0;
  double m_pointerEnterX = 0.0;
  double m_pointerEnterY = 0.0;
  Timer m_unlockTransitionTimer;
};
