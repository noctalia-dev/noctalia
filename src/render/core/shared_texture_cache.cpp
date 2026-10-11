#include "render/core/shared_texture_cache.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"

#include <algorithm>
#include <cerrno>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("texcache");

  // Keeps the first channel of each RGBA pixel, packed tightly.
  void packFirstChannel(std::vector<std::uint8_t>& rgba, int width, int height) {
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
      rgba[pixel] = rgba[pixel * 4U];
    }
    rgba.resize(pixelCount);
  }

  [[nodiscard]] bool allOpaque(const std::vector<std::uint8_t>& rgba) {
    for (std::size_t index = 3; index < rgba.size(); index += 4) {
      if (rgba[index] != 0xFF) {
        return false;
      }
    }
    return true;
  }
} // namespace

// ── Lease ────────────────────────────────────────────────────────────────────

SharedTextureCache::Lease::Lease(SharedTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id)
    : m_cache(cache), m_lifetimeToken(std::move(lifetimeToken)), m_id(id) {}

SharedTextureCache::Lease::~Lease() { reset(); }

SharedTextureCache::Lease::Lease(Lease&& other) noexcept
    : m_cache(std::exchange(other.m_cache, nullptr)), m_lifetimeToken(std::move(other.m_lifetimeToken)),
      m_id(std::exchange(other.m_id, 0)) {}

SharedTextureCache::Lease& SharedTextureCache::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    m_cache = std::exchange(other.m_cache, nullptr);
    m_lifetimeToken = std::move(other.m_lifetimeToken);
    m_id = std::exchange(other.m_id, 0);
  }
  return *this;
}

SharedTextureCache* SharedTextureCache::Lease::liveCache() const {
  return m_cache != nullptr && !m_lifetimeToken.expired() ? m_cache : nullptr;
}

TextureHandle SharedTextureCache::Lease::texture() const {
  const SharedTextureCache* cache = liveCache();
  const Entry* entry = cache != nullptr ? cache->findLeaseEntry(m_id) : nullptr;
  return entry != nullptr && entry->state == EntryState::Ready ? entry->handle : TextureHandle{};
}

bool SharedTextureCache::Lease::failed() const {
  const SharedTextureCache* cache = liveCache();
  const Entry* entry = cache != nullptr ? cache->findLeaseEntry(m_id) : nullptr;
  return entry != nullptr && entry->state == EntryState::Failed;
}

void SharedTextureCache::Lease::reset() {
  if (SharedTextureCache* cache = liveCache(); cache != nullptr) {
    cache->releaseLease(m_id);
  }
  m_cache = nullptr;
  m_lifetimeToken.reset();
  m_id = 0;
}

// ── Cache ────────────────────────────────────────────────────────────────────

SharedTextureCache::SharedTextureCache() {
  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; wallpaper textures will never finish loading");
  }
  // One worker: wallpapers decode to hundreds of MB each, and they are requested one at a time anyway.
  m_worker = std::thread([this]() { workerLoop(); });
}

SharedTextureCache::~SharedTextureCache() {
  m_lifetimeToken.reset();
  m_leases.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }

  // Textures uploaded into consumer backends die with those backends' contexts. Best-effort for the shared ones: if
  // the context can't be bound (lost on resume), the GL textures are already gone.
  if (m_textureManager != nullptr && makeCurrentFor(EntryKey{})) {
    m_textureManager->cleanup();
  }

  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

void SharedTextureCache::initialize(GlSharedContext* sharedGl) {
  m_sharedGl = sharedGl;
  if (m_sharedGl != nullptr) {
    m_textureManager = createDefaultTextureManager();
  }
}

SharedTextureCache::Lease
SharedTextureCache::acquire(SharedTextureRequest request, RenderBackend* backend, std::function<void()> onChange) {
  if (request.path.empty()) {
    return {};
  }
  EntryKey key{.request = std::move(request), .backend = m_textureManager != nullptr ? nullptr : backend};
  if (m_textureManager == nullptr && key.backend == nullptr) {
    return {};
  }

  retainEntry(key);
  const std::uint64_t id = ++m_nextLeaseId;
  m_leases.emplace(id, LeaseRecord{.key = std::move(key), .onChange = std::move(onChange)});
  return Lease{this, m_lifetimeToken, id};
}

void SharedTextureCache::abandonGpuResources() noexcept {
  if (m_textureManager != nullptr) {
    m_textureManager->abandonGpuResources();
  }
  // A reset loses every context, including the per-backend ones.
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.state == EntryState::Ready) {
      entry.handle = {};
      entry.state = EntryState::Loading;
    }
  }
}

void SharedTextureCache::reloadResidentTextures() {
  for (const auto& [key, entry] : m_entries) {
    if (entry.state == EntryState::Loading) {
      requestDecode(key.request);
    }
  }
}

void SharedTextureCache::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void SharedTextureCache::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size() || (fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  std::deque<DecodedImage> results;
  {
    std::scoped_lock lock(m_resultMutex);
    results = std::move(m_results);
    m_results.clear();
  }

  std::vector<EntryKey> changed;
  for (const DecodedImage& image : results) {
    m_pendingDecodes.erase(image.request);
    for (auto& [key, entry] : m_entries) {
      if (key.request != image.request || entry.state != EntryState::Loading) {
        continue;
      }
      if (image.failed) {
        entry.state = EntryState::Failed;
      } else {
        upload(key, entry, image);
      }
      if (entry.state != EntryState::Loading) {
        changed.push_back(key);
      }
    }
  }
  if (changed.empty()) {
    return;
  }

  std::vector<std::uint64_t> notify;
  for (const auto& [id, record] : m_leases) {
    if (record.onChange && std::ranges::find(changed, record.key) != changed.end()) {
      notify.push_back(id);
    }
  }
  // Callbacks may acquire or release leases, so look each one up again and call a copy.
  for (const std::uint64_t id : notify) {
    const auto it = m_leases.find(id);
    if (it == m_leases.end()) {
      continue;
    }
    const auto onChange = it->second.onChange;
    onChange();
  }
}

void SharedTextureCache::retainEntry(const EntryKey& key) {
  auto [it, inserted] = m_entries.try_emplace(key);
  ++it->second.refCount;
  if (inserted) {
    requestDecode(key.request);
  }
}

void SharedTextureCache::releaseEntry(const EntryKey& key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }
  if (--it->second.refCount > 0) {
    return;
  }
  unloadEntry(it->first, it->second);
  m_entries.erase(it);
  kLog.info("evicted {}", key.request.path);
  cancelDecodeIfUnused(key.request);
}

void SharedTextureCache::releaseLease(std::uint64_t id) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end()) {
    return;
  }
  const EntryKey key = std::move(it->second.key);
  m_leases.erase(it);
  releaseEntry(key);
}

const SharedTextureCache::Entry* SharedTextureCache::findLeaseEntry(std::uint64_t id) const {
  const auto lease = m_leases.find(id);
  if (lease == m_leases.end()) {
    return nullptr;
  }
  const auto entry = m_entries.find(lease->second.key);
  return entry != m_entries.end() ? &entry->second : nullptr;
}

void SharedTextureCache::requestDecode(const SharedTextureRequest& request) {
  if (!m_pendingDecodes.insert(request).second) {
    return;
  }
  {
    std::scoped_lock lock(m_queueMutex);
    m_jobQueue.push_back(request);
  }
  m_queueCv.notify_one();
}

void SharedTextureCache::cancelDecodeIfUnused(const SharedTextureRequest& request) {
  if (!m_pendingDecodes.contains(request) || hasLoadingEntry(request)) {
    return;
  }
  // A decode already running finishes and its result is dropped in dispatch(); only queued work can be skipped.
  bool removed = false;
  {
    std::scoped_lock lock(m_queueMutex);
    if (const auto it = std::ranges::find(m_jobQueue, request); it != m_jobQueue.end()) {
      m_jobQueue.erase(it);
      removed = true;
    }
  }
  if (removed) {
    m_pendingDecodes.erase(request);
  }
}

bool SharedTextureCache::hasLoadingEntry(const SharedTextureRequest& request) const {
  return std::ranges::any_of(m_entries, [&request](const auto& item) {
    return item.first.request == request && item.second.state == EntryState::Loading;
  });
}

void SharedTextureCache::upload(const EntryKey& key, Entry& entry, const DecodedImage& image) {
  if (!makeCurrentFor(key)) {
    return;
  }
  TextureManager* manager = textureManagerFor(key);
  TextureHandle handle = key.request.kind == SharedTextureKind::AlphaMask
      ? manager->loadFromPixels(
            image.pixels.data(), image.width, image.height, TextureDataFormat::Alpha, TextureFilter::Linear, true
        )
      : manager->loadFromRgba(image.pixels.data(), image.width, image.height, true);
  if (!handle.valid()) {
    kLog.warn("failed to upload {} ({}x{})", key.request.path, image.width, image.height);
    entry.state = EntryState::Failed;
    return;
  }
  handle.sourceWidth = image.sourceWidth;
  handle.sourceHeight = image.sourceHeight;
  handle.opaque = image.opaque;
  entry.handle = handle;
  entry.state = EntryState::Ready;
  kLog.info("uploaded {} ({}x{})", key.request.path, image.width, image.height);
}

void SharedTextureCache::unloadEntry(const EntryKey& key, Entry& entry) {
  if (entry.state != EntryState::Ready || !entry.handle.valid()) {
    return;
  }
  // If the context can't be bound (lost on resume), the GPU object is already gone.
  if (makeCurrentFor(key)) {
    textureManagerFor(key)->unload(entry.handle);
  }
  entry.handle = {};
}

TextureManager* SharedTextureCache::textureManagerFor(const EntryKey& key) const {
  return key.backend != nullptr ? &key.backend->textureManager() : m_textureManager.get();
}

bool SharedTextureCache::makeCurrentFor(const EntryKey& key) {
  if (key.backend != nullptr) {
    return key.backend->makeCurrentNoSurface();
  }
  if (m_sharedGl == nullptr) {
    return false;
  }
  // Backend contexts share the root context's share-list, so uploads/deletes work on whichever context is bound. If
  // a backend already owns the thread's context (mid-frame), switching away would drop its draw surface and break its
  // trailing eglSwapBuffers.
  if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
    return true;
  }
  return m_sharedGl->makeCurrentSurfaceless();
}

void SharedTextureCache::workerLoop() {
  while (true) {
    SharedTextureRequest request;
    {
      std::unique_lock lock(m_queueMutex);
      m_queueCv.wait(lock, [this]() { return m_shutdown.load() || !m_jobQueue.empty(); });
      if (m_shutdown.load()) {
        return;
      }
      request = std::move(m_jobQueue.front());
      m_jobQueue.pop_front();
    }

    DecodedImage image{.request = request};
    auto loaded = loadImageFile(request.path);
    if (!loaded) {
      kLog.warn("failed to decode {} ({})", ImageSourceLog::describe(request.path), loaded.error());
      image.failed = true;
      pushResult(std::move(image));
      continue;
    }

    image.width = loaded->width;
    image.height = loaded->height;
    image.sourceWidth = loaded->width;
    image.sourceHeight = loaded->height;
    if (request.kind == SharedTextureKind::AlphaMask) {
      packFirstChannel(loaded->rgba, loaded->width, loaded->height);
    } else {
      image.opaque = allOpaque(loaded->rgba);
    }
    image.pixels = std::move(loaded->rgba);
    pushResult(std::move(image));
  }
}

void SharedTextureCache::pushResult(DecodedImage image) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(image));
  }
  signalMain();
}

void SharedTextureCache::signalMain() {
  if (m_eventFd < 0) {
    return;
  }
  const std::uint64_t one = 1;
  if (::write(m_eventFd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal texture cache eventfd: errno={}", errno);
  }
}

std::size_t SharedTextureCache::RequestHash::operator()(const SharedTextureRequest& request) const noexcept {
  return std::hash<std::string>{}(request.path) ^ (static_cast<std::size_t>(request.kind) << 1U);
}

std::size_t SharedTextureCache::EntryKeyHash::operator()(const EntryKey& key) const noexcept {
  return RequestHash{}(key.request) ^ (std::hash<const void*>{}(key.backend) << 2U);
}
