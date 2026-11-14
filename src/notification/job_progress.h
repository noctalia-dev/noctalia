#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <sdbus-c++/sdbus-c++.h>
#include <vector>

struct JobProgress {
  std::uint32_t id = 0;
  std::string owner;
  std::string desktopEntry;
  std::string title;
  std::string infoMessage;
  std::string destination;
  std::string descriptionLabel1;
  std::string descriptionValue1;
  std::string descriptionLabel2;
  std::string descriptionValue2;
  std::uint32_t capabilities = 0;
  std::optional<std::uint32_t> percent;
  std::optional<std::uint64_t> processedBytes, totalBytes, processedFiles, totalFiles,
      processedDirectories, totalDirectories, processedItems, totalItems, speed, elapsedTime;
  std::vector<std::uint64_t> speedHistory;
  bool suspended = false;
  std::optional<bool> suspensionPending;
  bool terminated = false;
  std::uint32_t errorCode = 0;
  std::string errorMessage;
};

enum class JobProgressEvent { Added, Updated, Finished, Removed };
enum class JobProgressControl { Cancel, Suspend, Resume };

class JobProgressManager {
public:
  using Callback = void (*)(void*, const JobProgress&, JobProgressEvent);
  using ControlCallback = void (*)(void*, std::uint32_t, JobProgressControl);
  void setCallback(Callback callback, void* context) { m_callback = callback; m_context = context; }
  void setControlCallback(ControlCallback callback, void* context) { m_controlCallback = callback; m_controlContext = context; }
  void request(std::uint32_t id, JobProgressControl control);
  [[nodiscard]] std::uint64_t changeSerial() const noexcept { return m_changeSerial; }
  [[nodiscard]] const std::vector<JobProgress>& jobs() const { return m_jobs; }
  [[nodiscard]] JobProgress* find(std::uint32_t id);
  [[nodiscard]] const JobProgress* find(std::uint32_t id) const;
  [[nodiscard]] std::uint32_t add(std::string owner, std::string desktopEntry, std::uint32_t capabilities);
  bool update(std::uint32_t id, const std::map<std::string, sdbus::Variant>& properties);
  bool finish(std::uint32_t id, std::uint32_t errorCode, std::string errorMessage);
  bool remove(std::uint32_t id);
  void removeOwner(const std::string& owner);
private:
  void emit(const JobProgress& job, JobProgressEvent event);
  std::vector<JobProgress> m_jobs;
  std::uint32_t m_nextId = 1;
  Callback m_callback = nullptr;
  void* m_context = nullptr;
  ControlCallback m_controlCallback = nullptr;
  void* m_controlContext = nullptr;
  std::uint64_t m_changeSerial = 0;
};
