#pragma once

#include "auth/pam_authenticator.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct ext_session_lock_v1;
struct wl_surface;
struct wl_output;
class ConfigService;
class IpcService;

class LockSurface;
class RenderContext;
class SharedTextureCache;
class WaylandConnection;

class LockScreen {
public:
  LockScreen();
  ~LockScreen();

  bool initialize(
      WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService,
      SharedTextureCache* textureCache
  );
  void setSessionHooks(std::function<void()> onLocked, std::function<void()> onUnlocked);
  bool lock();
  void unlock();
  void onOutputChange();
  void onSecondTick();
  void onFontChanged();
  void onThemeChanged();
  void onWallpaperChanged();
  void requestLayout();
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] bool isActive() const noexcept;
  [[nodiscard]] bool isSessionLocked() const noexcept;

  /// Runs `fn` after the session reaches interactive lock (`m_locked`), or immediately if already locked.
  /// Used so suspend runs after lock surfaces exist. Cleared if lock fails or the lock request is aborted.
  void runAfterSessionLocked(std::function<void()> fn);

  void registerIpc(IpcService& ipc);

  static void handleLocked(void* data, ext_session_lock_v1* lock);
  static void handleFinished(void* data, ext_session_lock_v1* lock);

  static void setInstance(LockScreen* instance);
  static LockScreen* instance();

private:
  struct Instance {
    std::uint32_t outputName = 0;
    wl_output* output = nullptr;
    std::unique_ptr<LockSurface> surface;
  };

  void syncInstances();
  void createInstance(const WaylandOutput& output);
  void resetLockState();
  void clearInstances();
  void updatePromptOnSurfaces();
  void handlePasswordEdited(const std::string& value);
  void tryAuthenticate();
  static void clearSensitiveString(std::string& value);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  ConfigService* m_configService = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  ext_session_lock_v1* m_lock = nullptr;
  std::vector<Instance> m_instances;
  PamAuthenticator m_authenticator;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  wl_surface* m_pointerSurface = nullptr;
  bool m_statusIsError = false;
  bool m_lockPending = false;
  bool m_locked = false;
  std::function<void()> m_pendingAfterLocked;
  std::function<void()> m_onSessionLocked;
  std::function<void()> m_onSessionUnlocked;
};
