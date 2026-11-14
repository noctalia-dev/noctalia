#include "notification/job_progress.h"

#include <algorithm>
#include <limits>

namespace {
  constexpr std::size_t kMaxText = 4096;

  template <typename T> bool read(const std::map<std::string, sdbus::Variant>& props, const char* key, T& out) {
    const auto it = props.find(key);
    if (it == props.end()) return true;
    try { out = it->second.get<T>(); } catch (...) { return false; }
    return true;
  }

  bool readText(const std::map<std::string, sdbus::Variant>& props, const char* key, std::string& out) {
    if (!read(props, key, out)) return false;
    return out.size() <= kMaxText;
  }

  bool readCount(const std::map<std::string, sdbus::Variant>& props, const char* key,
                 std::optional<std::uint64_t>& out) {
    std::uint64_t value = 0;
    const auto it = props.find(key);
    if (it == props.end()) return true;
    try { value = it->second.get<std::uint64_t>(); }
    catch (...) {
      try { value = it->second.get<std::uint32_t>(); }
      catch (...) {
        try {
          const auto signedValue = it->second.get<std::int64_t>();
          if (signedValue < 0) return false;
          value = static_cast<std::uint64_t>(signedValue);
        } catch (...) { return false; }
      }
    }
    out = value;
    return true;
  }
}

JobProgress* JobProgressManager::find(std::uint32_t id) {
  const auto it = std::ranges::find(m_jobs, id, &JobProgress::id);
  return it == m_jobs.end() ? nullptr : &*it;
}

const JobProgress* JobProgressManager::find(std::uint32_t id) const {
  const auto it = std::ranges::find(m_jobs, id, &JobProgress::id);
  return it == m_jobs.end() ? nullptr : &*it;
}

std::uint32_t JobProgressManager::add(std::string owner, std::string desktopEntry, std::uint32_t capabilities) {
  JobProgress job;
  job.id = m_nextId++;
  if (job.id == 0) job.id = m_nextId++;
  job.owner = std::move(owner);
  job.desktopEntry = std::move(desktopEntry);
  job.capabilities = capabilities & 0x3U;
  m_jobs.push_back(std::move(job));
  emit(m_jobs.back(), JobProgressEvent::Added);
  return m_jobs.back().id;
}

bool JobProgressManager::update(std::uint32_t id, const std::map<std::string, sdbus::Variant>& p) {
  JobProgress* current = find(id);
  if (current == nullptr || current->terminated || p.size() > 64) return false;
  JobProgress next = *current;
  std::uint32_t percent = next.percent.value_or(0);
  bool suspended = next.suspended;
  if (!readText(p,"title",next.title) || !readText(p,"infoMessage",next.infoMessage)
      || !readText(p,"destUrl",next.destination) || !readText(p,"descriptionLabel1",next.descriptionLabel1)
      || !readText(p,"descriptionValue1",next.descriptionValue1) || !readText(p,"descriptionLabel2",next.descriptionLabel2)
      || !readText(p,"descriptionValue2",next.descriptionValue2) || !read(p,"percent",percent)
      || !read(p,"suspended",suspended) || !read(p,"errorCode",next.errorCode)
      || !readText(p,"errorMessage",next.errorMessage)
      || !readCount(p,"processedBytes",next.processedBytes) || !readCount(p,"totalBytes",next.totalBytes)
      || !readCount(p,"processedFiles",next.processedFiles) || !readCount(p,"totalFiles",next.totalFiles)
      || !readCount(p,"processedDirectories",next.processedDirectories) || !readCount(p,"totalDirectories",next.totalDirectories)
      || !readCount(p,"processedItems",next.processedItems) || !readCount(p,"totalItems",next.totalItems)
      || !readCount(p,"speed",next.speed) || !readCount(p,"elapsedTime",next.elapsedTime)) return false;
  if (percent > 100) return false;
  if (p.contains("percent")) next.percent = percent;
  next.suspended = suspended;
  if (p.contains("suspended")) next.suspensionPending.reset();
  if (p.contains("speed") && next.speed) {
    constexpr std::size_t kMaxSpeedSamples = 60;
    if (next.speedHistory.size() == kMaxSpeedSamples) next.speedHistory.erase(next.speedHistory.begin());
    next.speedHistory.push_back(*next.speed);
  }
  *current = std::move(next);
  emit(*current, JobProgressEvent::Updated);
  return true;
}

bool JobProgressManager::finish(std::uint32_t id, std::uint32_t errorCode, std::string errorMessage) {
  JobProgress* job = find(id);
  if (job == nullptr || job->terminated || errorMessage.size() > kMaxText) return false;
  job->terminated = true;
  job->errorCode = errorCode;
  job->errorMessage = std::move(errorMessage);
  emit(*job, JobProgressEvent::Finished);
  return true;
}

bool JobProgressManager::remove(std::uint32_t id) {
  const auto it = std::ranges::find(m_jobs, id, &JobProgress::id);
  if (it == m_jobs.end()) return false;
  const JobProgress removed = *it;
  m_jobs.erase(it);
  emit(removed, JobProgressEvent::Removed);
  return true;
}

void JobProgressManager::removeOwner(const std::string& owner) {
  for (std::size_t i = m_jobs.size(); i > 0; --i)
    if (m_jobs[i - 1].owner == owner) remove(m_jobs[i - 1].id);
}

void JobProgressManager::emit(const JobProgress& job, JobProgressEvent event) {
  m_changeSerial++;
  if (m_callback != nullptr) m_callback(m_context, job, event);
}

void JobProgressManager::request(std::uint32_t id, JobProgressControl control) {
  JobProgress* job = find(id);
  if (job == nullptr || job->terminated || m_controlCallback == nullptr) return;
  if ((control == JobProgressControl::Cancel && (job->capabilities & 1U) == 0)
      || (control != JobProgressControl::Cancel && (job->capabilities & 2U) == 0)) return;
  if (control == JobProgressControl::Cancel) {
    m_controlCallback(m_controlContext, id, control);
    return;
  }
  const bool desired = control == JobProgressControl::Suspend;
  if (job->suspended == desired || job->suspensionPending) return;
  job->suspensionPending = desired;
  emit(*job, JobProgressEvent::Updated);
  m_controlCallback(m_controlContext, id, control);
}
