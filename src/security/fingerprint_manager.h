#pragma once

#include "core/timer_manager.h"
#include "security/fingerprint_status.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;

namespace sdbus {
  class Error;
  class IProxy;
  class ObjectPath;
} // namespace sdbus

struct FingerprintDevice {
  std::string path;
  std::string name;
  std::string scanType = "press"; // "press" or "swipe"
  int enrollStages = -1;          // -1 when the driver does not report it
};

// The current user's fingerprints through fprintd's D-Bus API: lists the readers, then enrolls,
// verifies and deletes prints on the selected one (fprintd's CLI tools only use the default
// reader). All state lives here so the settings page, rebuilt on every change, stays stateless.
class FingerprintManager {
public:
  enum class Mode { Idle, Enrolling, Verifying, Deleting };

  using ChangedCallback = std::function<void()>;

  explicit FingerprintManager(SystemBus& bus);
  ~FingerprintManager();

  FingerprintManager(const FingerprintManager&) = delete;
  FingerprintManager& operator=(const FingerprintManager&) = delete;

  void setChangedCallback(ChangedCallback callback);

  // Re-reads the readers and the selected reader's enrolled fingers, asynchronously. Without
  // fprintd, or without a reader it supports, the list ends up empty.
  void refresh();
  [[nodiscard]] bool loaded() const noexcept { return m_loaded; }

  [[nodiscard]] const std::vector<FingerprintDevice>& devices() const noexcept { return m_devices; }
  [[nodiscard]] const std::string& selectedDevice() const noexcept { return m_selectedPath; }
  void selectDevice(const std::string& path);
  [[nodiscard]] const std::vector<std::string>& enrolledFingers() const noexcept { return m_enrolled; }

  void deleteFinger(const std::string& finger);
  void startEnroll(const std::string& finger);
  void startVerify();
  // Stops a running enroll/verify and releases the reader.
  void cancel();
  // Clears the outcome of the last operation (status line, matched finger).
  void clearStatus();
  // Forgets a finished enrollment so the enroll sheet starts at the finger picker again.
  void resetEnrollment();

  [[nodiscard]] Mode mode() const noexcept { return m_mode; }
  [[nodiscard]] bool busy() const noexcept { return m_mode != Mode::Idle || m_claiming; }
  [[nodiscard]] const std::string& enrollFinger() const noexcept { return m_enrollFinger; }
  [[nodiscard]] int enrollStagesDone() const noexcept { return m_stagesDone; }
  // -1 when the driver does not report it.
  [[nodiscard]] int enrollStagesTotal() const noexcept;
  [[nodiscard]] bool enrollCompleted() const noexcept { return m_enrollCompleted; }
  // Finger recognized by the last test; "any" when fprintd does not say which one.
  [[nodiscard]] const std::string& matchedFinger() const noexcept { return m_matchedFinger; }
  [[nodiscard]] const std::string& status() const noexcept { return m_status; }
  [[nodiscard]] bool statusIsError() const noexcept { return m_statusError; }

  [[nodiscard]] static std::string fingerLabel(std::string_view finger);

private:
  [[nodiscard]] const FingerprintDevice* selected() const;
  [[nodiscard]] std::string touchPrompt() const;
  void readDevices(std::vector<sdbus::ObjectPath> paths);
  bool ensureDeviceProxy();
  void loadEnrolled();
  void claimThen(std::function<void()> next);
  void release();
  void finishOperation();
  void handleScanStatus(Mode mode, const std::string& result, bool done);
  void failOperation(std::string_view what, const sdbus::Error& error);
  void setStatus(std::string message, bool error);
  void notifyChanged();
  void dropDeviceProxyLater();
  void later(std::function<void()> call);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::unique_ptr<sdbus::IProxy> m_device;
  std::shared_ptr<void> m_alive = std::make_shared<int>(0);
  ChangedCallback m_onChanged;
  Timer m_verifyTimeout;

  bool m_loaded = false;
  bool m_loading = false;
  std::vector<FingerprintDevice> m_devices;
  std::string m_selectedPath;
  std::vector<std::string> m_enrolled;

  Mode m_mode = Mode::Idle;
  bool m_claimed = false;
  bool m_claiming = false;
  std::string m_enrollFinger;
  int m_stagesDone = 0;
  bool m_enrollCompleted = false;
  std::string m_matchedFinger;
  std::string m_status;
  bool m_statusError = false;
};
