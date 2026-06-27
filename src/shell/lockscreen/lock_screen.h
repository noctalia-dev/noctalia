#pragma once

#include "auth/pam_authenticator.h"
#include "capture/screencopy_capture.h"

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

class FingerprintAuthenticator;
class LockSurface;
class RenderContext;
class SharedTextureCache;
class SystemBus;
class WaylandConnection;

class LockScreen {
public:
  LockScreen();
  ~LockScreen();

  bool initialize(
      WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService,
      SharedTextureCache* textureCache, SystemBus* systemBus
  );
  void setSessionHooks(std::function<void()> onLocked, std::function<void()> onUnlocked);
  void setLockEngagedCallback(std::function<void()> callback);
  bool lock();
  void primeDesktopCaptures();
  void clearPrimedDesktopCaptures();
  void unlock();
  void onOutputChange();
  void onFontChanged();
  void onThemeChanged();
  void onGpuResourcesInvalidated();
  void onWallpaperChanged();
  void onConfigChanged();
  void requestLayout();
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] bool isActive() const noexcept;
  [[nodiscard]] bool isSessionLocked() const noexcept;

  template <typename Fn> void forEachSurface(Fn&& fn) {
    for (auto& instance : m_instances) {
      if (instance.surface != nullptr) {
        fn(*instance.surface);
      }
    }
  }

  /// Runs `fn` after the session reaches interactive lock (`m_locked`), or immediately if already locked.
  /// Used so suspend runs after lock surfaces exist. Cleared if lock fails or the lock request is aborted.
  void runAfterSessionLocked(std::function<void()> fn);

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
  void captureDesktopSnapshots();
  [[nodiscard]] bool shouldUseBlurredDesktop() const;
  void applyLockscreenStyle(LockSurface& surface) const;
  void applyOutputRestriction();
  void applyWallpaperStyleToSurfaces();
  [[nodiscard]] bool isInteractiveOutput(const WaylandOutput& output) const;
  [[nodiscard]] std::string wallpaperPathForOutput(const std::string& connectorName) const;
  void createInstance(const WaylandOutput& output);
  void resetLockState();
  void clearInstances();
  void updatePromptOnSurfaces();
  void handlePasswordEdited(const std::string& value);
  void tryAuthenticate();
  void handleAuthResult(std::uint64_t generation, PamAuthenticator::Result result);
  void invalidatePendingAuthentication();
  void startFingerprint();
  void stopFingerprint();
  void handleFingerprintStatus(const std::string& message, bool isError);
  static void clearSensitiveString(std::string& value);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  ConfigService* m_configService = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  SystemBus* m_systemBus = nullptr;
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
  bool m_desktopCapturesPrimed = false;
  bool m_lockDeferred = false;
  std::function<void()> m_pendingAfterLocked;
  std::function<void()> m_onSessionLocked;
  std::function<void()> m_onSessionUnlocked;
  std::function<void()> m_onLockEngaged;
};
