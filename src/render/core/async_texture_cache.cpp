#include "render/core/async_texture_cache.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/gl_shared_context.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("async-tex");
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;
  constexpr std::size_t kMaxUnusedResidentEntries = 128;

} // namespace

AsyncTextureCache::ReadySubscription::ReadySubscription(
    AsyncTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id
)
    : m_cache(cache), m_lifetimeToken(std::move(lifetimeToken)), m_id(id) {}

AsyncTextureCache::ReadySubscription::~ReadySubscription() { disconnect(); }

AsyncTextureCache::ReadySubscription::ReadySubscription(ReadySubscription&& other) noexcept
    : m_cache(other.m_cache), m_lifetimeToken(std::move(other.m_lifetimeToken)), m_id(other.m_id) {
  other.m_cache = nullptr;
  other.m_id = 0;
}

AsyncTextureCache::ReadySubscription&
AsyncTextureCache::ReadySubscription::operator=(ReadySubscription&& other) noexcept {
  if (this != &other) {
    disconnect();
    m_cache = other.m_cache;
    m_lifetimeToken = std::move(other.m_lifetimeToken);
    m_id = other.m_id;
    other.m_cache = nullptr;
    other.m_id = 0;
  }
  return *this;
}

void AsyncTextureCache::ReadySubscription::disconnect() {
  if (m_id == 0) {
    return;
  }
  if (m_cache != nullptr && !m_lifetimeToken.expired()) {
    m_cache->removeReadyListener(m_id);
  }
  m_cache = nullptr;
  m_lifetimeToken.reset();
  m_id = 0;
}

AsyncTextureCache::AsyncTextureCache() {
  m_textureManager = createDefaultTextureManager();

  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; async texture cache notifications will be disabled");
  }

  const unsigned hc = std::thread::hardware_concurrency();
  const std::size_t suggested = hc == 0U ? kMinWorkers : std::max<std::size_t>(kMinWorkers, hc / 2U);
  const std::size_t workerCount = std::clamp<std::size_t>(suggested, kMinWorkers, kMaxWorkers);
  m_workers.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    m_workers.emplace_back([this]() { workerLoop(); });
  }
}

AsyncTextureCache::~AsyncTextureCache() {
  m_lifetimeToken.reset();
  m_readyListeners.clear();

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  if (!m_entries.empty()) {
    makeCurrent();
    for (auto& [key, entry] : m_entries) {
      (void)key;
      if (entry.handle.id != 0) {
        m_textureManager->unload(entry.handle);
      }
    }
    m_entries.clear();
  }
  m_textureManager->cleanup();

  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

void AsyncTextureCache::initialize(GlSharedContext* sharedGl) { m_sharedGl = sharedGl; }

AsyncTextureCache::ReadySubscription
AsyncTextureCache::subscribeReady(const std::string& path, int targetSize, bool mipmap, TextureReadyCallback callback) {
  auto key = makeKey(path, targetSize, mipmap);
  if (key.path.empty() || !callback) {
    return {};
  }

  if (const auto handle = peek(key.path, key.targetSize, key.mipmap); handle.id != 0) {
    callback(handle);
    return {};
  }

  const std::uint64_t id = ++m_nextReadyListenerId;
  m_readyListeners.emplace(id, ReadyListener{.key = std::move(key), .callback = std::move(callback)});
  return ReadySubscription{this, m_lifetimeToken, id};
}

TextureHandle AsyncTextureCache::acquire(const std::string& path, int targetSize, bool mipmap) {
  const auto key = makeKey(path, targetSize, mipmap);
  if (key.path.empty()) {
    return {};
  }

  auto [it, inserted] = m_entries.try_emplace(key);
  Entry& entry = it->second;
  if (inserted) {
    entry.refCount = 1;
  } else {
    ++entry.refCount;
  }
  touchEntry(entry);

  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_canceled.erase(key);
    if (!m_inFlight.contains(key)) {
      m_inFlight.insert(key);
      m_jobQueue.push_back(key);
    }
  }
  m_queueCv.notify_one();
  return {};
}

TextureHandle AsyncTextureCache::peek(const std::string& path, int targetSize, bool mipmap) const {
  const auto key = makeKey(path, targetSize, mipmap);
  if (key.path.empty()) {
    return {};
  }

  auto it = m_entries.find(key);
  return it != m_entries.end() ? it->second.handle : TextureHandle{};
}

void AsyncTextureCache::release(const std::string& path, int targetSize, bool mipmap) {
  const auto key = makeKey(path, targetSize, mipmap);
  if (key.path.empty()) {
    return;
  }

  auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }

  if (it->second.refCount > 0) {
    --it->second.refCount;
  }
  if (it->second.refCount > 0) {
    return;
  }

  if (it->second.handle.id != 0 || it->second.failed) {
    // Keep a bounded set of recently used zero-ref textures resident so
    // virtualized views can immediately rebind them while users scrub back and
    // forth. Owners that truly tear down the view can explicitly trim unused
    // entries once those consumers are gone.
    touchEntry(it->second);
    pruneUnusedEntries(kMaxUnusedResidentEntries);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_inFlight.contains(key)) {
      m_canceled.insert(key);
    }
  }

  m_entries.erase(it);
}

void AsyncTextureCache::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void AsyncTextureCache::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  std::deque<DecodedJob> jobs;
  {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    jobs = std::move(m_results);
    m_results.clear();
  }
  if (jobs.empty()) {
    return;
  }

  bool madeCurrent = false;

  for (auto& job : jobs) {
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_inFlight.erase(job.key);
      if (auto canceled = m_canceled.find(job.key); canceled != m_canceled.end()) {
        m_canceled.erase(canceled);
        dropped = true;
      }
    }
    if (dropped) {
      continue;
    }

    auto entryIt = m_entries.find(job.key);
    if (entryIt == m_entries.end()) {
      continue;
    }

    if (job.failed || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
      entryIt->second.failed = true;
      continue;
    }

    if (!madeCurrent) {
      makeCurrent();
      madeCurrent = true;
    }

    entryIt->second.handle = m_textureManager->loadFromRgba(job.rgba.data(), job.width, job.height, job.key.mipmap);
    if (entryIt->second.handle.id == 0) {
      entryIt->second.failed = true;
      continue;
    }
    touchEntry(entryIt->second);
    notifyReady(job.key, entryIt->second.handle);
  }
}

void AsyncTextureCache::trimUnused(std::size_t maxUnusedEntries) { pruneUnusedEntries(maxUnusedEntries); }

void AsyncTextureCache::reloadResidentTextures() {
  if (m_textureManager == nullptr || m_entries.empty()) {
    return;
  }

  makeCurrent();

  std::vector<RequestKey> resident;
  resident.reserve(m_entries.size());
  for (const auto& [key, entry] : m_entries) {
    if (entry.handle.id != 0 || entry.refCount > 0) {
      resident.push_back(key);
    }
  }

  for (const RequestKey& key : resident) {
    auto entryIt = m_entries.find(key);
    if (entryIt == m_entries.end()) {
      continue;
    }

    Entry& entry = entryIt->second;
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = {};
    entry.failed = false;

    std::string errorMessage;
    auto loaded = loadImageFile(key.path, key.targetSize, &errorMessage);
    if (!loaded.has_value()) {
      entry.failed = true;
      if (!errorMessage.empty()) {
        kLog.warn("failed to reload image after GPU reset: {} ({})", ImageSourceLog::describe(key.path), errorMessage);
      }
      continue;
    }

    entry.handle = m_textureManager->loadFromRgba(loaded->rgba.data(), loaded->width, loaded->height, key.mipmap);
    if (entry.handle.id == 0) {
      entry.failed = true;
      continue;
    }
    touchEntry(entry);
    notifyReady(key, entry.handle);
  }
}

std::size_t AsyncTextureCache::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  std::size_t seed = std::hash<std::string>{}(key.path);
  seed ^= std::hash<int>{}(key.targetSize) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  seed ^= std::hash<bool>{}(key.mipmap) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  return seed;
}

void AsyncTextureCache::workerLoop() {
  while (true) {
    RequestKey key;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCv.wait(lock, [this]() { return m_shutdown.load() || !m_jobQueue.empty(); });
      if (m_shutdown.load()) {
        return;
      }
      key = std::move(m_jobQueue.front());
      m_jobQueue.pop_front();
    }

    DecodedJob result;
    result.key = key;

    std::string errorMessage;
    if (auto loaded = loadImageFile(key.path, key.targetSize, &errorMessage)) {
      result.rgba = std::move(loaded->rgba);
      result.width = loaded->width;
      result.height = loaded->height;
    } else {
      result.failed = true;
      kLog.warn("failed to decode image: {} ({})", ImageSourceLog::describe(key.path), errorMessage);
    }

    pushResult(std::move(result));
  }
}

void AsyncTextureCache::signalMain() {
  if (m_eventFd < 0) {
    return;
  }

  const std::uint64_t one = 1;
  const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal async texture eventfd: errno={}", errno);
  }
}

void AsyncTextureCache::pushResult(DecodedJob job) {
  {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_results.push_back(std::move(job));
  }
  signalMain();
}

void AsyncTextureCache::makeCurrent() {
  if (m_sharedGl != nullptr) {
    // Uploads are valid on any context in the share group, so if a backend already owns the thread's context
    // (mid-frame), don't switch away — that would drop its draw surface and break its trailing eglSwapBuffers.
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
      return;
    }
    m_sharedGl->makeCurrentSurfaceless();
  } else if (m_makeCurrentCallback) {
    // Non-shared mode: no share group, so uploads must bind RenderContext's own context even if another isolated
    // context is currently bound on the thread.
    m_makeCurrentCallback();
  }
}

void AsyncTextureCache::touchEntry(Entry& entry) { entry.lastTouch = ++m_touchSerial; }

void AsyncTextureCache::removeReadyListener(std::uint64_t id) { m_readyListeners.erase(id); }

void AsyncTextureCache::notifyReady(const RequestKey& key, TextureHandle handle) {
  std::vector<std::pair<std::uint64_t, TextureReadyCallback>> callbacks;
  callbacks.reserve(m_readyListeners.size());
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (auto& [id, callback] : callbacks) {
    if (m_readyListeners.contains(id)) {
      callback(handle);
    }
  }
}

void AsyncTextureCache::pruneUnusedEntries(std::size_t maxUnusedEntries) {
  using It = std::unordered_map<RequestKey, Entry, RequestKeyHash>::iterator;
  std::vector<It> unused;

  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (it->second.refCount == 0 && (it->second.handle.id != 0 || it->second.failed)) {
      unused.push_back(it);
    }
  }

  if (unused.size() <= maxUnusedEntries) {
    return;
  }

  std::sort(unused.begin(), unused.end(), [](const It& a, const It& b) {
    return a->second.lastTouch < b->second.lastTouch;
  });

  const std::size_t toEvict = unused.size() - maxUnusedEntries;
  bool madeCurrent = false;
  for (std::size_t i = 0; i < toEvict; ++i) {
    if (unused[i]->second.handle.id != 0) {
      if (!madeCurrent) {
        makeCurrent();
        madeCurrent = true;
      }
      m_textureManager->unload(unused[i]->second.handle);
    }
    m_entries.erase(unused[i]);
  }
}

AsyncTextureCache::RequestKey AsyncTextureCache::makeKey(const std::string& path, int targetSize, bool mipmap) {
  return RequestKey{.path = path, .targetSize = std::max(0, targetSize), .mipmap = mipmap};
}
