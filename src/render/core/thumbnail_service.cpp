#include "render/core/thumbnail_service.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/texture_manager.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stb/stb_image_resize2.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#include <webp/encode.h>

namespace {

  constexpr Logger kLog("thumbnail");
  constexpr float kThumbnailWebPQuality = 82.0f;
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;
  constexpr std::string_view kThumbnailCacheVersion = "thumbnail-service-v2";

  std::filesystem::path thumbnailCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::filesystem::path(xdg) / "noctalia" / "thumbnails";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".cache" / "noctalia" / "thumbnails";
    }
    return std::filesystem::path("/tmp") / "noctalia" / "thumbnails";
  }

  std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (char ch : text) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
      hash *= 1099511628211ull;
    }
    return hash;
  }

  std::string hex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
      out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
      value >>= 4;
    }
    return out;
  }

  std::optional<std::filesystem::path> cachePathForSource(const std::string& sourcePath, int targetPx) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(sourcePath, ec);
    if (ec) {
      return std::nullopt;
    }

    const auto mtime = fs::last_write_time(sourcePath, ec);
    if (ec) {
      return std::nullopt;
    }

    const auto ticks = mtime.time_since_epoch().count();
    const std::string key = sourcePath
        + '\n'
        + std::to_string(size)
        + '\n'
        + std::to_string(static_cast<long long>(ticks))
        + '\n'
        + std::to_string(targetPx)
        + '\n'
        + std::string(kThumbnailCacheVersion);
    return thumbnailCacheDir() / (hex64(fnv1a64(key)) + ".webp");
  }

  bool writeFile(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
  }

  std::vector<std::uint8_t> rgbaToRgb(const std::vector<std::uint8_t>& rgba) {
    const std::size_t pixelCount = rgba.size() / 4;
    std::vector<std::uint8_t> rgb(pixelCount * 3);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      rgb[i * 3 + 0] = rgba[i * 4 + 0];
      rgb[i * 3 + 1] = rgba[i * 4 + 1];
      rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return rgb;
  }

  std::vector<std::uint8_t> rgbToRgba(const std::vector<std::uint8_t>& rgb) {
    const std::size_t pixelCount = rgb.size() / 3;
    std::vector<std::uint8_t> rgba(pixelCount * 4);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      rgba[i * 4 + 0] = rgb[i * 3 + 0];
      rgba[i * 4 + 1] = rgb[i * 3 + 1];
      rgba[i * 4 + 2] = rgb[i * 3 + 2];
      rgba[i * 4 + 3] = 255;
    }
    return rgba;
  }

  bool resizeThumbnail(std::vector<std::uint8_t>& pixels, int& width, int& height, int targetPx) {
    const int maxDim = std::max(width, height);
    if (maxDim <= targetPx || width <= 0 || height <= 0) {
      return true;
    }

    const float scale = static_cast<float>(targetPx) / static_cast<float>(maxDim);
    const int resizedW = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
    const int resizedH = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));

    std::vector<std::uint8_t> out(static_cast<std::size_t>(resizedW) * static_cast<std::size_t>(resizedH) * 3);
    unsigned char* result =
        stbir_resize_uint8_linear(pixels.data(), width, height, 0, out.data(), resizedW, resizedH, 0, STBIR_RGB);
    if (result == nullptr) {
      return false;
    }

    pixels = std::move(out);
    width = resizedW;
    height = resizedH;
    return true;
  }

} // namespace

std::size_t ThumbnailService::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  const std::size_t pathHash = std::hash<std::string>{}(key.path);
  const std::size_t sizeHash = std::hash<int>{}(key.targetPx);
  return pathHash ^ (sizeHash + 0x9e3779b97f4a7c15ull + (pathHash << 6U) + (pathHash >> 2U));
}

ThumbnailService::Subscription::Subscription(std::function<void()> disconnect) : m_disconnect(std::move(disconnect)) {}

ThumbnailService::Subscription::~Subscription() { disconnect(); }

ThumbnailService::Subscription::Subscription(Subscription&& other) noexcept
    : m_disconnect(std::move(other.m_disconnect)) {}

ThumbnailService::Subscription& ThumbnailService::Subscription::operator=(Subscription&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  disconnect();
  m_disconnect = std::move(other.m_disconnect);
  return *this;
}

void ThumbnailService::Subscription::disconnect() {
  if (!m_disconnect) {
    return;
  }
  auto disconnect = std::move(m_disconnect);
  m_disconnect = nullptr;
  disconnect();
}

ThumbnailService::ThumbnailService() {
  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; thumbnail wakeups will be disabled");
  }

  const unsigned hc = std::thread::hardware_concurrency();
  const std::size_t suggested = (hc == 0) ? kMinWorkers : std::max<std::size_t>(kMinWorkers, hc / 2);
  const std::size_t n = std::clamp<std::size_t>(suggested, kMinWorkers, kMaxWorkers);
  m_workers.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    m_workers.emplace_back([this]() { workerLoop(); });
  }
  kLog.info("spawned {} decode worker(s)", n);
}

ThumbnailService::~ThumbnailService() {
  if (m_lifetimeToken != nullptr) {
    *m_lifetimeToken = false;
  }
  m_pendingListeners.clear();
  m_readyListeners.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  for (auto& t : m_workers) {
    if (t.joinable()) {
      t.join();
    }
  }

  deleteAllTextures();

  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

ThumbnailService::Subscription ThumbnailService::subscribePendingUpload(PendingUploadCallback callback) {
  if (!callback) {
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_pendingListeners.emplace(id, PendingListener{.callback = std::move(callback)});
  bool hasPendingResults = false;
  {
    std::scoped_lock lock(m_resultMutex);
    hasPendingResults = !m_results.empty();
  }
  if (hasPendingResults && m_pendingListeners.contains(id) && m_pendingListeners[id].callback) {
    m_pendingListeners[id].callback();
  }

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id]() {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_pendingListeners.erase(id);
  });
}

ThumbnailService::Subscription
ThumbnailService::subscribeReady(const std::string& path, ReadyCallback callback, int targetPx) {
  if (path.empty() || !callback) {
    return {};
  }

  const TextureHandle current = peek(path, targetPx);
  if (current.id != 0) {
    callback(path, current);
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_readyListeners.emplace(
      id, ReadyListener{.key = RequestKey{.path = path, .targetPx = targetPx}, .callback = std::move(callback)}
  );

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id]() {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_readyListeners.erase(id);
  });
}

TextureHandle ThumbnailService::acquire(const std::string& path, int targetPx) {
  if (path.empty()) {
    return {};
  }

  const RequestKey key{.path = path, .targetPx = targetPx};
  CacheEntry& entry = m_entries[key];
  ++entry.refCount;
  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  enqueueDecodeIfNeeded(key);
  return {};
}

TextureHandle ThumbnailService::peek(const std::string& path, int targetPx) const {
  const auto it = m_entries.find(RequestKey{.path = path, .targetPx = targetPx});
  if (it == m_entries.end()) {
    return {};
  }
  return it->second.handle;
}

void ThumbnailService::release(const std::string& path, int targetPx) {
  const RequestKey key{.path = path, .targetPx = targetPx};
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }

  CacheEntry& entry = it->second;
  if (entry.refCount > 0) {
    --entry.refCount;
  }

  if (entry.refCount > 0) {
    return;
  }

  if (entry.handle.id != 0 && m_textureManager != nullptr) {
    m_textureManager->unload(entry.handle);
  }
  m_entries.erase(it);

  std::scoped_lock lock(m_queueMutex);
  if (m_inFlight.contains(key)) {
    m_canceled.insert(key);
  }
}

void ThumbnailService::enqueueDecodeIfNeeded(const RequestKey& key) {
  std::scoped_lock lock(m_queueMutex);
  m_canceled.erase(key);
  if (m_inFlight.contains(key)) {
    return;
  }
  m_inFlight.insert(key);
  m_jobQueue.push_back(key);
  m_queueCv.notify_one();
}

bool ThumbnailService::uploadPending(TextureManager& textures) {
  m_textureManager = &textures;

  std::deque<DecodedJob> jobs;
  {
    std::scoped_lock lock(m_resultMutex);
    jobs = std::move(m_results);
    m_results.clear();
  }
  if (jobs.empty()) {
    return false;
  }

  bool changed = false;
  for (auto& job : jobs) {
    bool dropped = false;
    {
      std::scoped_lock lock(m_queueMutex);
      m_inFlight.erase(job.key);
      if (auto c = m_canceled.find(job.key); c != m_canceled.end()) {
        m_canceled.erase(c);
        dropped = true;
      }
    }
    if (dropped) {
      continue;
    }

    auto entryIt = m_entries.find(job.key);
    if (entryIt == m_entries.end() || entryIt->second.refCount == 0) {
      continue;
    }
    if (job.failed || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    TextureHandle handle = textures.loadFromRgba(job.rgba.data(), job.width, job.height);
    if (handle.id == 0) {
      kLog.warn("failed to upload thumbnail texture for {}", job.key.path);
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    if (entryIt->second.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entryIt->second.handle);
    }
    entryIt->second.handle = handle;
    entryIt->second.failed = false;
    changed = true;
    notifyReady(job.key, handle);
  }
  return changed;
}

void ThumbnailService::invalidateGpuResources(TextureManager& textures) {
  m_textureManager = &textures;

  std::vector<RequestKey> liveKeys;
  liveKeys.reserve(m_entries.size());
  for (auto& [key, entry] : m_entries) {
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = {};
    entry.failed = false;
    if (entry.refCount > 0) {
      liveKeys.push_back(key);
    }
  }

  for (const RequestKey& key : liveKeys) {
    enqueueDecodeIfNeeded(key);
  }
}

void ThumbnailService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void ThumbnailService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  notifyPendingUpload();
}

void ThumbnailService::signalMain() {
  if (m_eventFd < 0) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal thumbnail eventfd: errno={}", errno);
  }
}

void ThumbnailService::pushResult(DecodedJob job) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(job));
  }
  signalMain();
}

void ThumbnailService::deleteAllTextures() {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entry.handle);
    }
  }
  m_entries.clear();
}

void ThumbnailService::notifyPendingUpload() {
  std::vector<PendingUploadCallback> callbacks;
  callbacks.reserve(m_pendingListeners.size());
  for (const auto& [id, listener] : m_pendingListeners) {
    (void)id;
    if (listener.callback) {
      callbacks.push_back(listener.callback);
    }
  }

  for (auto& callback : callbacks) {
    callback();
  }
}

void ThumbnailService::notifyReady(const RequestKey& key, TextureHandle handle) {
  std::vector<std::pair<std::uint64_t, ReadyCallback>> callbacks;
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (const auto& [id, callback] : callbacks) {
    const auto it = m_readyListeners.find(id);
    if (it == m_readyListeners.end()) {
      continue;
    }
    m_readyListeners.erase(it);
    callback(key.path, handle);
  }
}

void ThumbnailService::workerLoop() {
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

    const std::string& path = key.path;
    DecodedJob result;
    result.key = key;

    if (const auto cachePath = cachePathForSource(path, key.targetPx); cachePath.has_value()) {
      auto cachedBytes = FileUtils::readBinaryFile(cachePath->string());
      if (!cachedBytes.empty()) {
        if (auto cached = decodeRasterImage(cachedBytes.data(), cachedBytes.size())) {
          result.rgba = std::move(cached->pixels);
          result.width = cached->width;
          result.height = cached->height;
          pushResult(std::move(result));
          continue;
        }

        std::error_code ec;
        std::filesystem::remove(*cachePath, ec);
      }
    }

    auto bytes = FileUtils::readBinaryFile(path);
    if (bytes.empty()) {
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    auto decoded = decodeRasterImage(bytes.data(), bytes.size());
    if (!decoded) {
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    int w = decoded->width;
    int h = decoded->height;
    auto pixels = rgbaToRgb(decoded->pixels);

    if (!resizeThumbnail(pixels, w, h, key.targetPx)) {
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    if (const auto cachePath = cachePathForSource(path, key.targetPx); cachePath.has_value()) {
      std::uint8_t* encoded = nullptr;
      const std::size_t encodedSize = WebPEncodeRGB(pixels.data(), w, h, w * 3, kThumbnailWebPQuality, &encoded);
      if (encoded != nullptr && encodedSize > 0) {
        (void)writeFile(*cachePath, encoded, encodedSize);
        WebPFree(encoded);
      }
    }

    result.rgba = rgbToRgba(pixels);
    result.width = w;
    result.height = h;
    pushResult(std::move(result));
  }
}
