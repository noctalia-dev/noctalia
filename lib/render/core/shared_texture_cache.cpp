#include "render/core/shared_texture_cache.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/image_decoder.h"
#include "render/core/image_source_log.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {

  // Wallpaper sets shuffle through the same files; decoding to a raw RGBA copy on first use turns
  // every later switch into one file read. Capped LRU by file mtime.
  constexpr std::size_t kMaxPixelCacheFiles = 8;
  constexpr std::uint32_t kPixelCacheMagic = 0x5854524E; // 'NRTX'

  struct PixelCacheHeader {
    std::uint32_t magic = kPixelCacheMagic;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t sourceWidth = 0;
    std::int32_t sourceHeight = 0;
    std::int32_t opaque = 0;
  };

  std::filesystem::path pixelCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::filesystem::path(xdg) / "noctalia" / "wallpaper";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".cache" / "noctalia" / "wallpaper";
    }
    return std::filesystem::path("/tmp") / "noctalia" / "wallpaper";
  }

  std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char ch : text) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
      hash *= 1099511628211ULL;
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

  std::filesystem::path pixelCacheFile(const SharedTextureRequest& request, std::int64_t mtimeNs) {
    const std::string key = request.path
        + '\n'
        + std::to_string(static_cast<int>(request.kind))
        + '\n'
        + std::to_string(mtimeNs)
        + '\n'
        + std::to_string(request.coverage.width)
        + 'x'
        + std::to_string(request.coverage.height);
    return pixelCacheDir() / (hex64(fnv1a64(key)) + ".rgba");
  }

  std::size_t pixelBytes(const PixelCacheHeader& header, bool alphaMask) {
    return static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height) * (alphaMask ? 1U : 4U);
  }

  // A short read means a torn or truncated file; treat it as a miss and re-decode.
  bool loadCachedPixels(
      const std::filesystem::path& file, bool alphaMask, std::vector<std::uint8_t>& pixels, PixelCacheHeader& header
  ) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return false;
    }
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || header.magic != kPixelCacheMagic || header.width <= 0 || header.height <= 0) {
      return false;
    }
    const std::size_t bytes = pixelBytes(header, alphaMask);
    if (static_cast<std::size_t>(in.gcount()) != sizeof(header) || header.width * header.height <= 0) {
      return false;
    }
    pixels.resize(bytes);
    in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(bytes));
    return static_cast<std::size_t>(in.gcount()) == bytes;
  }

  void storeCachedPixels(
      const std::filesystem::path& file, const std::vector<std::uint8_t>& pixels, int width, int height,
      int sourceWidth, int sourceHeight, bool opaque
  ) {
    if (pixels.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
      return;
    }
    PixelCacheHeader header;
    header.width = width;
    header.height = height;
    header.sourceWidth = sourceWidth;
    header.sourceHeight = sourceHeight;
    header.opaque = opaque ? 1 : 0;
    const auto tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        return;
      }
      out.write(reinterpret_cast<const char*>(&header), sizeof(header));
      out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
      if (!out) {
        std::filesystem::remove(tmp, ec);
        return;
      }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
    }
    // LRU by file mtime; prune after each write so the cache never outgrows the cap.
    std::vector<std::filesystem::file_time_type> times;
    for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
      if (!ec && entry.is_regular_file()) {
        times.push_back(entry.last_write_time(ec));
        if (ec) {
          break;
        }
      }
    }
    if (times.size() > kMaxPixelCacheFiles) {
      std::ranges::sort(times);
      const auto cutoff = *(times.end() - static_cast<std::ptrdiff_t>(kMaxPixelCacheFiles));
      for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
        if (!ec && entry.is_regular_file() && entry.last_write_time(ec) < cutoff) {
          std::filesystem::remove(entry.path(), ec);
        }
      }
    }
  }

} // namespace

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

  bool allOpaque(const std::vector<std::uint8_t>& rgba) {
    for (std::size_t index = 3; index < rgba.size(); index += 4) {
      if (rgba[index] != 0xFF) {
        return false;
      }
    }
    return true;
  }

  // Number of 2x halvings that keep the image at least `coverage` on both axes.
  int reductionLevel(int width, int height, TextureCoverage coverage) {
    if (coverage.empty()) {
      return 0;
    }
    int level = 0;
    while ((width >> (level + 1)) >= coverage.width && (height >> (level + 1)) >= coverage.height) {
      ++level;
    }
    return level;
  }

  // One mip step: each output pixel averages a 2x2 block, the box filter glGenerateMipmap uses. An odd last row or
  // column is dropped, matching the floor(size / 2) level dimensions. Runs in place: every write lands at or before
  // the block being read, and only on bytes no later block reads.
  void halveInPlace(std::vector<std::uint8_t>& pixels, int& width, int& height, int channels) {
    const int halfWidth = width / 2;
    const int halfHeight = height / 2;
    const auto channelCount = static_cast<std::size_t>(channels);
    const std::size_t stride = static_cast<std::size_t>(width) * channelCount;
    std::uint8_t* data = pixels.data();
    for (int y = 0; y < halfHeight; ++y) {
      const std::uint8_t* top = data + (static_cast<std::size_t>(y) * 2U * stride);
      const std::uint8_t* bottom = top + stride;
      std::uint8_t* out = data + (static_cast<std::size_t>(y) * static_cast<std::size_t>(halfWidth) * channelCount);
      for (int x = 0; x < halfWidth; ++x) {
        const std::size_t left = static_cast<std::size_t>(x) * 2U * channelCount;
        for (std::size_t c = 0; c < channelCount; ++c) {
          const std::size_t s = left + c;
          const unsigned sum = top[s] + top[s + channelCount] + bottom[s] + bottom[s + channelCount] + 2U;
          out[(static_cast<std::size_t>(x) * channelCount) + c] = static_cast<std::uint8_t>(sum >> 2U);
        }
      }
    }
    width = halfWidth;
    height = halfHeight;
    pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channelCount);
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

void SharedTextureCache::Lease::setCoverage(TextureCoverage coverage) {
  if (SharedTextureCache* cache = liveCache(); cache != nullptr) {
    cache->setLeaseCoverage(m_id, coverage);
  }
}

bool SharedTextureCache::Lease::setPreparedImage(
    PreparedTextureRequest request, std::shared_ptr<const LoadedImageFile> image
) {
  if (SharedTextureCache* cache = liveCache(); cache != nullptr) {
    return cache->setLeasePreparedImage(m_id, std::move(request), std::move(image));
  }
  return false;
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
  m_worker = std::thread([this] { workerLoop(); });
}

SharedTextureCache::~SharedTextureCache() {
  m_lifetimeToken.reset();
  m_leases.clear();
  for (auto& [key, entry] : m_entries) {
    if (key.prepared && key.backend != nullptr) {
      unloadEntry(key, entry);
    }
  }

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }
  if (m_uploadContext != EGL_NO_CONTEXT && m_sharedGl != nullptr) {
    eglDestroyContext(m_sharedGl->display(), m_uploadContext);
    m_uploadContext = EGL_NO_CONTEXT;
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
  request.coverage = effectiveCoverage(request.coverage);
  EntryKey key{.request = std::move(request), .backend = m_textureManager != nullptr ? nullptr : backend};
  if (m_textureManager == nullptr && key.backend == nullptr) {
    return {};
  }

  retainEntry(key);
  const std::uint64_t id = ++m_nextLeaseId;
  m_leases.emplace(id, LeaseRecord{.key = std::move(key), .onChange = std::move(onChange)});
  return Lease{this, m_lifetimeToken, id};
}

bool SharedTextureCache::validPreparedImage(
    const PreparedTextureRequest& request, const std::shared_ptr<const LoadedImageFile>& image
) {
  if (request.source.empty() || request.revision.empty() || !image || image->width <= 0 || image->height <= 0) {
    return false;
  }
  const auto width = static_cast<std::size_t>(image->width), height = static_cast<std::size_t>(image->height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U / height || image->rgba.size() != width * height * 4U) {
    return false;
  }
  return (image->sourceWidth == 0 && image->sourceHeight == 0) || (image->sourceWidth > 0 && image->sourceHeight > 0);
}

SharedTextureCache::Lease SharedTextureCache::acquirePrepared(
    PreparedTextureRequest request, std::shared_ptr<const LoadedImageFile> image, RenderBackend* backend,
    std::function<void()> onChange
) {
  if (m_eventFd < 0 || !validPreparedImage(request, image)) {
    return {};
  }
  EntryKey key{
      .backend = m_textureManager != nullptr ? nullptr : backend,
      .prepared = std::move(request),
      .preparedWidth = image->width,
      .preparedHeight = image->height
  };
  if (m_textureManager == nullptr && key.backend == nullptr) {
    return {};
  }
  if (const auto it = m_entries.find(key); it != m_entries.end() && it->second.invalidated) {
    return {};
  }
  retainEntry(key, std::move(image));
  const auto id = ++m_nextLeaseId;
  m_leases.emplace(id, LeaseRecord{.key = std::move(key), .onChange = std::move(onChange)});
  return Lease{this, m_lifetimeToken, id};
}

void SharedTextureCache::queuePrepared(const EntryKey& key, Entry& entry) {
  if (!entry.queued) {
    entry.queued = true;
    m_preparedWork.push_back({key, entry.serial});
    signalMain();
  }
}

void SharedTextureCache::invalidatePrepared(const std::string& source) {
  for (auto& [key, entry] : m_entries) {
    if (key.prepared && key.prepared->source == source && !entry.invalidated) {
      entry.invalidated = true;
      queuePrepared(key, entry);
    }
  }
}

std::size_t SharedTextureCache::preparedBytes() const {
  std::size_t bytes = 0;
  std::unordered_set<const LoadedImageFile*> seen;
  for (const auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.preparedPixels && seen.insert(entry.preparedPixels.get()).second) {
      bytes += entry.preparedPixels->rgba.size();
    }
  }
  return bytes;
}

std::size_t SharedTextureCache::residentBytes() const {
  std::size_t bytes = 0;
  for (const auto& [key, entry] : m_entries) {
    (void)key;
    bytes += entry.residentBytes;
  }
  return bytes;
}

void SharedTextureCache::abandonGpuResources() noexcept {
  // Invalidate uploads in flight: their GL objects die with the old share group.
  m_resetSerial.fetch_add(1);
  if (m_textureManager != nullptr) {
    m_textureManager->abandonGpuResources();
  }
  // A reset loses every context, including the per-backend ones.
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.state == EntryState::Ready) {
      entry.handle = {};
      entry.residentBytes = 0;
      entry.state = EntryState::Loading;
    }
  }
}

void SharedTextureCache::reloadResidentTextures() {
  for (auto& [key, entry] : m_entries) {
    if (entry.state == EntryState::Loading) {
      if (key.prepared) {
        queuePrepared(key, entry);
      } else {
        requestDecode(key.request);
      }
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
  for (const auto& work : std::exchange(m_preparedWork, {})) {
    const auto it = m_entries.find(work.key);
    if (it == m_entries.end() || it->second.serial != work.serial) {
      continue;
    }
    auto& entry = it->second;
    entry.queued = false;
    if (entry.invalidated) {
      unloadEntry(it->first, entry);
      entry.preparedPixels.reset();
      entry.state = EntryState::Failed;
    } else if (entry.state == EntryState::Loading && entry.preparedPixels) {
      const auto& image = *entry.preparedPixels;
      uploadPixels(
          it->first, entry, image.rgba.data(), image.width, image.height,
          image.sourceWidth > 0 ? image.sourceWidth : image.width,
          image.sourceHeight > 0 ? image.sourceHeight : image.height, false, it->first.prepared->mipmap
      );
    }
    if (entry.state != EntryState::Loading) {
      changed.push_back(it->first);
    }
  }
  for (const DecodedImage& image : results) {
    m_pendingDecodes.erase(image.request);
    for (auto& [key, entry] : m_entries) {
      if (key.prepared || key.request != image.request || entry.state != EntryState::Loading) {
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

  std::vector<std::uint64_t> notify;
  for (auto& [id, record] : m_leases) {
    const bool pendingChanged =
        record.pending.has_value() && std::ranges::find(changed, *record.pending) != changed.end();
    const bool switched = pendingChanged && resolvePending(record);
    if (record.onChange && (switched || std::ranges::find(changed, record.key) != changed.end())) {
      notify.push_back(id);
    }
  }
  // Coverage changes whose target was already resident switch here, so onChange stays the only way texture() changes.
  for (const std::uint64_t id : std::exchange(m_deferredSwitches, {})) {
    const auto it = m_leases.find(id);
    if (it != m_leases.end() && it->second.pending.has_value() && resolvePending(it->second) && it->second.onChange) {
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

void SharedTextureCache::retainEntry(const EntryKey& key, std::shared_ptr<const LoadedImageFile> image) {
  auto [it, inserted] = m_entries.try_emplace(key);
  ++it->second.refCount;
  if (inserted) {
    if (key.prepared) {
      it->second.preparedPixels = std::move(image);
      it->second.serial = ++m_nextPreparedSerial;
      queuePrepared(it->first, it->second);
    } else {
      requestDecode(key.request);
    }
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
  if (key.prepared) {
    std::erase_if(m_preparedWork, [&key](const auto& work) { return work.key == key; });
  } else {
    kLog.info("evicted {}", key.request.path);
    cancelDecodeIfUnused(key.request);
  }
}

void SharedTextureCache::releaseLease(std::uint64_t id) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end()) {
    return;
  }
  const EntryKey key = std::move(it->second.key);
  const std::optional<EntryKey> pending = std::move(it->second.pending);
  m_leases.erase(it);
  releaseEntry(key);
  if (pending.has_value()) {
    releaseEntry(*pending);
  }
}

const SharedTextureCache::Entry* SharedTextureCache::findLeaseEntry(std::uint64_t id) const {
  const auto lease = m_leases.find(id);
  if (lease == m_leases.end()) {
    return nullptr;
  }
  const auto entry = m_entries.find(lease->second.key);
  return entry != m_entries.end() ? &entry->second : nullptr;
}

void SharedTextureCache::setLeaseCoverage(std::uint64_t id, TextureCoverage coverage) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end() || it->second.key.prepared) {
    return;
  }
  EntryKey target = it->second.key;
  target.request.coverage = effectiveCoverage(coverage);
  retargetLease(id, it->second, std::move(target));
}

bool SharedTextureCache::setLeasePreparedImage(
    std::uint64_t id, PreparedTextureRequest request, std::shared_ptr<const LoadedImageFile> image
) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end() || !it->second.key.prepared || !validPreparedImage(request, image)) {
    return false;
  }
  EntryKey target{
      .backend = it->second.key.backend,
      .prepared = std::move(request),
      .preparedWidth = image->width,
      .preparedHeight = image->height
  };
  if (const auto found = m_entries.find(target); found != m_entries.end() && found->second.invalidated) {
    return false;
  }
  retargetLease(id, it->second, std::move(target), std::move(image));
  return true;
}

void SharedTextureCache::retargetLease(
    std::uint64_t id, LeaseRecord& record, EntryKey target, std::shared_ptr<const LoadedImageFile> image
) {
  if (target == record.key) {
    if (record.pending.has_value()) {
      const EntryKey pending = *std::exchange(record.pending, std::nullopt);
      releaseEntry(pending);
    }
    return;
  }
  if (record.pending == target) {
    return;
  }

  retainEntry(target, std::move(image));
  if (record.pending.has_value()) {
    const EntryKey pending = *std::exchange(record.pending, std::nullopt);
    releaseEntry(pending);
  }

  const EntryState targetState = m_entries.find(target)->second.state;
  const auto active = m_entries.find(record.key);
  const bool activeReady = active != m_entries.end() && active->second.state == EntryState::Ready;
  if (targetState == EntryState::Failed && activeReady) {
    releaseEntry(target);
    return;
  }
  if (targetState == EntryState::Loading && !activeReady) {
    // Nothing is shown either way, so retarget the lease now.
    const EntryKey previous = std::exchange(record.key, std::move(target));
    releaseEntry(previous);
    return;
  }
  record.pending = std::move(target);
  if (targetState != EntryState::Loading) {
    m_deferredSwitches.push_back(id);
    signalMain();
  }
}

bool SharedTextureCache::resolvePending(LeaseRecord& record) {
  const auto it = m_entries.find(*record.pending);
  if (it == m_entries.end()) {
    record.pending.reset();
    return false;
  }
  if (it->second.state == EntryState::Loading) {
    return false;
  }
  const EntryKey pending = *std::exchange(record.pending, std::nullopt);
  if (it->second.state == EntryState::Failed) {
    releaseEntry(pending);
    return false;
  }
  const EntryKey previous = std::exchange(record.key, pending);
  releaseEntry(previous);
  return true;
}

TextureCoverage SharedTextureCache::effectiveCoverage(TextureCoverage coverage) {
  return coverage.empty() || !TextureManager::globalMipmapsEnabled() ? TextureCoverage{} : coverage;
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
  if (image.uploaded.valid() && image.resetSerial == m_resetSerial.load()) {
    entry.handle = image.uploaded;
    entry.state = EntryState::Ready;
    kLog.info(
        "uploaded {} ({}x{} from {}x{})", key.request.path, image.width, image.height, image.sourceWidth,
        image.sourceHeight
    );
    return;
  }
  uploadPixels(
      key, entry, image.pixels.data(), image.width, image.height, image.sourceWidth, image.sourceHeight, image.opaque,
      true
  );
}

void SharedTextureCache::uploadPixels(
    const EntryKey& key, Entry& entry, const std::uint8_t* pixels, int width, int height, int sourceWidth,
    int sourceHeight, bool opaque, bool mipmap
) {
  if (!makeCurrentFor(key)) {
    return;
  }
  TextureManager* manager = textureManagerFor(key);
  const bool mask = !key.prepared && key.request.kind == SharedTextureKind::AlphaMask;
  TextureHandle handle = mask
      ? manager->loadFromPixels(pixels, width, height, TextureDataFormat::Alpha, TextureFilter::Linear, mipmap)
      : manager->loadFromRgba(pixels, width, height, mipmap);
  if (!handle.valid()) {
    if (key.prepared) {
      kLog.warn("failed to upload prepared image ({}x{})", width, height);
    } else {
      kLog.warn("failed to upload {} ({}x{})", key.request.path, width, height);
    }
    entry.state = EntryState::Failed;
    return;
  }
  handle.sourceWidth = sourceWidth;
  handle.sourceHeight = sourceHeight;
  handle.opaque = opaque;
  entry.handle = handle;
  entry.state = EntryState::Ready;
  entry.residentBytes = 0;
  int w = width, h = height;
  do {
    entry.residentBytes += static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * (mask ? 1U : 4U);
    if (!mipmap || !TextureManager::globalMipmapsEnabled() || (w == 1 && h == 1)) {
      break;
    }
    w = std::max(1, w / 2);
    h = std::max(1, h / 2);
  } while (true);
  if (!key.prepared) {
    kLog.info("uploaded {} ({}x{} from {}x{})", key.request.path, width, height, sourceWidth, sourceHeight);
  }
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
  entry.residentBytes = 0;
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

// Worker-thread only: uploads get their own context in the root's share group. The root context is
// current on the main thread and EGL forbids one context current on two threads (BAD_ACCESS: failed
// uploads, repeated GPU work). Recreated once per GPU reset; on failure dispatch() uploads instead.
bool SharedTextureCache::ensureUploadContext() {
  if (m_sharedGl == nullptr) {
    return false;
  }
  const EGLDisplay display = m_sharedGl->display();
  if (m_uploadContext == EGL_NO_CONTEXT) {
    m_uploadContext = m_sharedGl->createContext(m_sharedGl->rootContext(), "texture upload");
  }
  if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_uploadContext) == EGL_TRUE) {
    return true;
  }
  eglDestroyContext(display, m_uploadContext);
  m_uploadContext = m_sharedGl->createContext(m_sharedGl->rootContext(), "texture upload");
  return m_uploadContext != EGL_NO_CONTEXT
      && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_uploadContext) == EGL_TRUE;
}

void SharedTextureCache::workerLoop() {
  while (true) {
    // Every queued request for the same file shares one decode; each coverage gets its own reduction.
    std::vector<SharedTextureRequest> batch;
    {
      std::unique_lock lock(m_queueMutex);
      m_queueCv.wait(lock, [this] { return m_shutdown.load() || !m_jobQueue.empty(); });
      if (m_shutdown.load()) {
        // The upload context is thread-local; unbind it so the destructor can destroy it.
        if (m_sharedGl != nullptr && m_uploadContext != nullptr) {
          eglMakeCurrent(m_sharedGl->display(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        return;
      }
      batch.push_back(std::move(m_jobQueue.front()));
      m_jobQueue.pop_front();
      for (auto it = m_jobQueue.begin(); it != m_jobQueue.end();) {
        if (it->path == batch.front().path && it->kind == batch.front().kind) {
          batch.push_back(std::move(*it));
          it = m_jobQueue.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Stamped onto every result; uploads made before a GPU reset land in the destroyed share group and must be
    // re-uploaded.
    const std::uint64_t serial = m_resetSerial.load();
    const std::string& path = batch.front().path;

    std::int64_t mtimeNs = 0;
    {
      std::error_code ec;
      const auto mtime = std::filesystem::last_write_time(path, ec);
      if (!ec) {
        mtimeNs = static_cast<std::int64_t>(mtime.time_since_epoch().count());
      }
    }
    const bool alphaMask = batch.front().kind == SharedTextureKind::AlphaMask;

    std::vector<DecodedImage> results;
    bool cacheHit = mtimeNs != 0;
    if (cacheHit) {
      for (const SharedTextureRequest& request : batch) {
        PixelCacheHeader header{};
        std::vector<std::uint8_t> pixels;
        if (!loadCachedPixels(pixelCacheFile(request, mtimeNs), alphaMask, pixels, header)) {
          cacheHit = false;
          results.clear();
          break;
        }
        results.push_back(
            DecodedImage{
                .request = request,
                .pixels = std::move(pixels),
                .width = header.width,
                .height = header.height,
                .sourceWidth = header.sourceWidth,
                .sourceHeight = header.sourceHeight,
                .opaque = header.opaque != 0,
                .resetSerial = serial,
            }
        );
      }
    }

    if (!cacheHit) {
      auto loaded = loadImageFile(path);
      if (!loaded) {
        kLog.warn("failed to decode {} ({})", ImageSourceLog::describe(path), loaded.error());
        for (SharedTextureRequest& request : batch) {
          pushResult(DecodedImage{.request = std::move(request), .failed = true, .resetSerial = serial});
        }
        continue;
      }

      const int sourceWidth = loaded->width;
      const int sourceHeight = loaded->height;
      if (alphaMask) {
        packFirstChannel(loaded->rgba, sourceWidth, sourceHeight);
      }
      // Averaging fully opaque pixels stays fully opaque, so the source answer holds for every reduction.
      const bool opaque = !alphaMask && allOpaque(loaded->rgba);

      // Smallest reduction first, so each result is copied out before the buffer is halved further.
      std::ranges::sort(batch, {}, [sourceWidth, sourceHeight](const SharedTextureRequest& request) {
        return reductionLevel(sourceWidth, sourceHeight, request.coverage);
      });
      std::vector<std::uint8_t> pixels = std::move(loaded->rgba);
      int width = sourceWidth;
      int height = sourceHeight;
      int level = 0;
      const auto reduce = [&](SharedTextureRequest& request) {
        const int target = reductionLevel(sourceWidth, sourceHeight, request.coverage);
        for (; level < target; ++level) {
          halveInPlace(pixels, width, height, alphaMask ? 1 : 4);
        }
        return DecodedImage{
            .request = std::move(request),
            .width = width,
            .height = height,
            .sourceWidth = sourceWidth,
            .sourceHeight = sourceHeight,
            .opaque = opaque,
            .resetSerial = serial,
        };
      };
      for (std::size_t i = 0; i + 1 < batch.size(); ++i) {
        DecodedImage image = reduce(batch[i]);
        image.pixels = pixels;
        results.push_back(std::move(image));
      }
      DecodedImage last = reduce(batch.back());
      last.pixels = std::move(pixels);
      results.push_back(std::move(last));

      if (mtimeNs != 0) {
        for (const DecodedImage& image : results) {
          storeCachedPixels(
              pixelCacheFile(image.request, mtimeNs), image.pixels, image.width, image.height, image.sourceWidth,
              image.sourceHeight, image.opaque
          );
        }
      }
    }

    // Upload on this thread when a shared context exists, so the main thread never blocks on glTexImage2D. If the
    // context can't be bound (lost on resume, or creation failed), pixels are kept and dispatch() uploads instead.
    if (m_textureManager != nullptr && ensureUploadContext()) {
      for (DecodedImage& image : results) {
        TextureHandle handle = batch.front().kind == SharedTextureKind::AlphaMask
            ? m_textureManager->loadFromPixels(
                  image.pixels.data(), image.width, image.height, TextureDataFormat::Alpha, TextureFilter::Linear, true
              )
            : m_textureManager->loadFromRgba(image.pixels.data(), image.width, image.height, true);
        if (!handle.valid()) {
          continue; // keep the pixels so dispatch() can retry on the main thread
        }
        handle.sourceWidth = image.sourceWidth;
        handle.sourceHeight = image.sourceHeight;
        handle.opaque = image.opaque;
        image.uploaded = handle;
        std::vector<std::uint8_t>().swap(image.pixels); // RAM no longer needed on the main thread
      }
      // Cross-context visibility: the main thread samples these textures from its own context; the uploads
      // must be complete before the results are dispatched to it.
      glFinish();
    }
    for (DecodedImage& image : results) {
      pushResult(std::move(image));
    }
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
  const auto backend = std::hash<const void*>{}(key.backend) << 2U;
  if (key.prepared) {
    return backend
        ^ std::hash<std::string>{}(key.prepared->source)
        ^ (std::hash<std::string>{}(key.prepared->revision) << 1U)
        ^ (std::hash<int>{}(key.preparedWidth) << 3U)
        ^ (std::hash<int>{}(key.preparedHeight) << 4U)
        ^ (static_cast<std::size_t>(key.prepared->mipmap) << 5U);
  }
  return RequestHash{}(key.request) ^ backend;
}
