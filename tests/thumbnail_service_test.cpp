#include "core/files/directory_scanner.h"
#include "render/core/image_decoder.h"
#include "render/core/image_encoder.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

void setThumbnailCacheWriteHookForTesting(std::function<bool(const std::string&, const std::string&)> hook);
void setThumbnailDecodeObserverHookForTesting(std::function<void(const std::string&, std::size_t)> hook);

class ThumbnailServiceTestAccess {
public:
  static void setMemoryBudget(ThumbnailService& service, std::size_t bytes) {
    std::scoped_lock lock(service.m_memoryMutex);
    service.m_memoryBudgetBytes = bytes;
  }
  static bool isFailed(const ThumbnailService& service, const std::string& path, int targetPx) {
    const auto it = service.m_entries.find(ThumbnailService::RequestKey{.path = path, .targetPx = targetPx});
    return it != service.m_entries.end() && it->second.failed;
  }
  static bool hasInFlight(const ThumbnailService& service) {
    std::scoped_lock lock(service.m_queueMutex);
    return !service.m_inFlight.empty();
  }
};

namespace {

  // Capture the exact RGBA buffer handed to the established image renderer, without a GL context.
  class RecordingTextures : public TextureManager {
  public:
    std::vector<std::uint8_t> pixels;
    TextureHandle loadFromRgba(const std::uint8_t* data, int width, int height, bool) override {
      pixels.assign(data, data + width * height * 4);
      return {.id = TextureId{1}, .width = width, .height = height};
    }
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    assert(!bytes.empty());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(file.good());
  }

  std::vector<std::uint8_t> load(const std::filesystem::path& path, int target) {
    RecordingTextures textures;
    ThumbnailService service;
    (void)service.acquire(path.string(), target);
    std::vector<pollfd> fds;
    const auto index = service.addPollFds(fds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (textures.pixels.empty() && std::chrono::steady_clock::now() < deadline) {
      (void)poll(fds.data(), fds.size(), 100);
      service.dispatch(fds, index);
      (void)service.uploadPending(textures);
    }
    assert(!textures.pixels.empty());
    return textures.pixels;
  }

  void checkAlpha(const std::vector<std::uint8_t>& pixels, std::uint8_t alpha) {
    for (std::size_t i = 3; i < pixels.size(); i += 4)
      assert(pixels[i] == alpha);
  }

  // Seed a valid opaque entry under the old version's actual cache key.
  void seedOpaqueV4Cache(const std::filesystem::path& root, const std::filesystem::path& source) {
    const std::string key = source.string()
        + '\n'
        + std::to_string(std::filesystem::file_size(source))
        + '\n'
        + std::to_string(std::filesystem::last_write_time(source).time_since_epoch().count())
        + "\n8\nthumbnail-service-v4";
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : key) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }
    const auto directory = root / "nythera" / "thumbnails";
    std::filesystem::create_directories(directory);
    const std::array<std::uint8_t, 4> opaque{0, 0, 0, 255};
    write(directory / std::format("{:016x}.webp", hash), encodeWebp(opaque.data(), 1, 1, 100));
  }

  void checkFixture(
      const std::filesystem::path& path, const std::vector<std::uint8_t>& encoded, int target, std::uint8_t alpha
  ) {
    write(path, encoded);
    checkAlpha(load(path, target), alpha);
    // Retain the cache key, but make source decoding fail. A second service must read the on-disk WebP.
    const auto mtime = std::filesystem::last_write_time(path);
    write(path, std::vector<std::uint8_t>(encoded.size(), 0));
    std::filesystem::last_write_time(path, mtime);
    checkAlpha(load(path, target), alpha);
  }
} // namespace

int main() {
  std::array<char, 64> temp{};
  std::string pattern = "/tmp/nythera-thumbnail-test-XXXXXX";
  std::copy(pattern.begin(), pattern.end(), temp.begin());
  const char* directory = mkdtemp(temp.data());
  assert(directory);
  const std::filesystem::path root(directory);
  const char* cacheEnv = std::getenv("XDG_CACHE_HOME");
  const std::string previousCache = cacheEnv ? cacheEnv : "";
  const bool hadCache = cacheEnv != nullptr;
  assert(setenv("XDG_CACHE_HOME", directory, 1) == 0);

  for (const std::uint8_t alpha : {0, 128, 255}) {
    std::vector<std::uint8_t> rgba(8 * 8 * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = 255;
      rgba[i + 3] = alpha;
    }
    for (int target : {4, 8}) {
      const auto suffix = std::to_string(alpha) + "-" + std::to_string(target);
      checkFixture(root / (suffix + ".png"), encodePng(rgba.data(), 8, 8), target, alpha);
      checkFixture(root / (suffix + ".webp"), encodeWebp(rgba.data(), 8, 8, 100), target, alpha);
      if (alpha == 255)
        checkFixture(root / (suffix + ".jpg"), encodeJpeg(rgba.data(), 8, 8, 90), target, 255);
    }
  }

  const auto oldCacheSource = root / "old-cache.png";
  const std::array<std::uint8_t, 4> transparent{255, 0, 0, 0};
  write(oldCacheSource, encodePng(transparent.data(), 1, 1));
  seedOpaqueV4Cache(root, oldCacheSource);
  checkAlpha(load(oldCacheSource, 8), 0);

  // A valid transparent 1x1 GIF, including a transparency control extension.
  const std::vector<std::uint8_t> gif{'G', 'I', 'F', '8', '9',  'a',  1, 0, 1, 0,    0x80, 0, 0,    0, 0,
                                      0,   255, 255, 255, 0x21, 0xF9, 4, 1, 0, 0,    0,    0, 0x2C, 0, 0,
                                      0,   0,   1,   0,   1,    0,    0, 2, 2, 0x44, 1,    0, 0x3B};
  checkFixture(root / "transparent.gif", gif, 8, 0);

  // Hidden blue must not bleed into red at a downsampled transparent boundary.
  const std::vector<std::uint8_t> edge{0, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 128, 0, 0, 255, 0};
  const auto edgePath = root / "edge.png";
  write(edgePath, encodePng(edge.data(), 4, 1));
  const auto resized = load(edgePath, 2);
  assert(resized.size() == 8);
  for (std::size_t i = 0; i < resized.size(); i += 4) {
    assert(resized[i] >= 250 && resized[i + 1] <= 5 && resized[i + 2] <= 5);
    assert(resized[i + 3] > 0 && resized[i + 3] < 255);
  }
  const auto cached = load(edgePath, 2);
  for (std::size_t i = 0; i < cached.size(); i += 4) {
    assert(cached[i] >= 240 && cached[i + 1] <= 10 && cached[i + 2] <= 10);
    assert(cached[i + 3] == resized[i + 3]);
  }

  const auto pumpUntil = [](ThumbnailService& service, RecordingTextures& textures, auto predicate) {
    std::vector<pollfd> fds;
    const auto index = service.addPollFds(fds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      (void)poll(fds.data(), fds.size(), 50);
      service.dispatch(fds, index);
      (void)service.uploadPending(textures);
    }
    assert(predicate());
  };

  // F-05.1: Private cache permissions (0700 dir, 0600 files).
  {
    const auto cacheDir = root / "nythera" / "thumbnails";
    struct stat dirStat{};
    assert(lstat(cacheDir.c_str(), &dirStat) == 0 && S_ISDIR(dirStat.st_mode));
    assert((dirStat.st_mode & 0777) == 0700);
    std::size_t webpCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cacheDir)) {
      const auto fname = entry.path().filename().string();
      assert(!fname.starts_with(".tmp-"));
      // Check all SHA-256 v6 cache entries written by ThumbnailService (excluding the legacy 16-hex v4 seed).
      if (fname.ends_with(".webp") && fname.size() == 69) {
        struct stat fileStat{};
        assert(lstat(entry.path().c_str(), &fileStat) == 0 && S_ISREG(fileStat.st_mode));
        assert((fileStat.st_mode & 0777) == 0600);
        ++webpCount;
      }
    }
    assert(webpCount > 0);
  }

  // F-05.2: Symlinked source files (valid outside target and dangling) are never followed.
  {
    const auto outsideDir = root / "outside-secret";
    const auto browseDir = root / "browse";
    std::filesystem::create_directories(outsideDir);
    std::filesystem::create_directories(browseDir);
    const std::array<std::uint8_t, 4> green{0, 255, 0, 255};
    const auto outsideImg = outsideDir / "secret.png";
    write(outsideImg, encodePng(green.data(), 1, 1));
    const auto linkToOutside = browseDir / "evil-link.png";
    const auto brokenLink = browseDir / "broken-link.png";
    std::filesystem::create_symlink(outsideImg, linkToOutside);
    std::filesystem::create_symlink(outsideDir / "missing.png", brokenLink);

    RecordingTextures textures;
    ThumbnailService service;
    assert(service.acquire(linkToOutside.string(), 8).id == 0);
    assert(service.acquire(brokenLink.string(), 8).id == 0);
    pumpUntil(service, textures, [&] {
      return ThumbnailServiceTestAccess::isFailed(service, linkToOutside.string(), 8)
          && ThumbnailServiceTestAccess::isFailed(service, brokenLink.string(), 8);
    });
    assert(textures.pixels.empty());
    assert(service.peek(linkToOutside.string(), 8).id == 0);
    assert(service.peek(brokenLink.string(), 8).id == 0);
  }

  // F-05.3: Symlinked cache directory and symlinked cache entry never read/clobber outside files.
  {
    const auto symlinkCacheBase = root / "symlink-cache-base";
    const auto outsideTargetDir = root / "outside-cache-target";
    std::filesystem::create_directories(symlinkCacheBase / "nythera");
    std::filesystem::create_directories(outsideTargetDir);
    std::filesystem::create_symlink(outsideTargetDir, symlinkCacheBase / "nythera" / "thumbnails");
    assert(setenv("XDG_CACHE_HOME", symlinkCacheBase.c_str(), 1) == 0);

    const auto srcImg = root / "symlink-cache-src.png";
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    write(srcImg, encodePng(red.data(), 1, 1));
    checkAlpha(load(srcImg, 8), 255);
    assert(std::filesystem::is_empty(outsideTargetDir));

    // Pre-staged symlink at the exact SHA-256 cache filename must not clobber victimFile.
    const auto entryCacheBase = root / "entry-symlink-cache";
    const auto entryCacheDir = entryCacheBase / "nythera" / "thumbnails";
    std::filesystem::create_directories(entryCacheDir);
    assert(chmod(entryCacheDir.c_str(), 0700) == 0);
    assert(setenv("XDG_CACHE_HOME", entryCacheBase.c_str(), 1) == 0);

    const auto victimFile = root / "victim.txt";
    {
      std::ofstream out(victimFile, std::ios::binary);
      out << "do-not-overwrite";
    }
    struct stat srcStat{};
    assert(lstat(srcImg.c_str(), &srcStat) == 0);
    const std::string key = srcImg.string()
        + '\n'
        + std::to_string(static_cast<long long>(srcStat.st_size))
        + '\n'
        + std::to_string(static_cast<long long>(srcStat.st_mtim.tv_sec))
        + '.'
        + std::to_string(static_cast<long long>(srcStat.st_mtim.tv_nsec))
        + "\n8\nthumbnail-service-v6";
    g_autofree gchar* digest =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(key.data()), key.size());
    assert(digest != nullptr);
    const auto poisonedLeaf = entryCacheDir / (std::string(digest) + ".webp");
    std::filesystem::create_symlink(victimFile, poisonedLeaf);

    checkAlpha(load(srcImg, 8), 255);
    assert(std::filesystem::is_symlink(std::filesystem::symlink_status(poisonedLeaf)));
    {
      std::ifstream in(victimFile, std::ios::binary);
      std::string content;
      std::getline(in, content);
      assert(content == "do-not-overwrite");
    }

    // Restoring XDG_CACHE_HOME to root.
    assert(setenv("XDG_CACHE_HOME", directory, 1) == 0);
  }

  // F-05.4: Unset XDG_CACHE_HOME and HOME disables on-disk cache instead of falling back to /tmp.
  {
    const char* prevHomeEnv = std::getenv("HOME");
    const std::string prevHome = prevHomeEnv ? prevHomeEnv : "";
    const bool hadHome = prevHomeEnv != nullptr;
    assert(unsetenv("XDG_CACHE_HOME") == 0);
    assert(unsetenv("HOME") == 0);

    const auto noEnvImg = root / "no-env.png";
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
    write(noEnvImg, encodePng(blue.data(), 1, 1));
    checkAlpha(load(noEnvImg, 8), 255);

    if (hadHome)
      assert(setenv("HOME", prevHome.c_str(), 1) == 0);
    assert(setenv("XDG_CACHE_HOME", directory, 1) == 0);
  }

  // F-05.5: Oversized files (> kMaxSourceFileBytes) are rejected before reading or decoding.
  {
    const auto hugeImg = root / "oversized.png";
    const int fd = ::open(hugeImg.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(ThumbnailService::kMaxSourceFileBytes + 4096)) == 0);
    assert(::close(fd) == 0);

    std::atomic_bool observerCalled{false};
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t) {
      observerCalled.store(true, std::memory_order_release);
    });
    RecordingTextures textures;
    ThumbnailService service;
    assert(service.acquire(hugeImg.string(), 8).id == 0);
    pumpUntil(service, textures, [&] { return ThumbnailServiceTestAccess::isFailed(service, hugeImg.string(), 8); });
    setThumbnailDecodeObserverHookForTesting({});
    assert(!observerCalled.load(std::memory_order_acquire));
    assert(textures.pixels.empty());
  }

  // F-05.6: Concurrent memory budget throttles parallel workers without starving large files.
  {
    std::vector<std::uint8_t> bigRgba(64 * 64 * 4, 255);
    std::uint32_t seed = 0x12345678U;
    for (std::size_t i = 0; i < bigRgba.size(); i += 4) {
      seed ^= seed << 13U;
      seed ^= seed >> 17U;
      seed ^= seed << 5U;
      bigRgba[i] = static_cast<std::uint8_t>(seed & 0xFFU);
      bigRgba[i + 1] = static_cast<std::uint8_t>((seed >> 8U) & 0xFFU);
      bigRgba[i + 2] = static_cast<std::uint8_t>((seed >> 16U) & 0xFFU);
    }
    const auto encodedBig = encodePng(bigRgba.data(), 64, 64);
    assert(encodedBig.size() > 1024);

    std::vector<std::filesystem::path> files;
    for (int i = 0; i < 4; ++i) {
      auto p = root / ("concurrent-" + std::to_string(i) + ".png");
      write(p, encodedBig);
      files.push_back(std::move(p));
    }

    const std::size_t tightBudget = encodedBig.size() + 128;
    std::atomic<std::size_t> peakUsed{0};
    std::atomic<int> activeDecodes{0};
    std::atomic<int> peakActive{0};
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t usedNow) {
      std::size_t prev = peakUsed.load(std::memory_order_relaxed);
      while (usedNow > prev && !peakUsed.compare_exchange_weak(prev, usedNow)) {
      }
      const int nowActive = activeDecodes.fetch_add(1, std::memory_order_acq_rel) + 1;
      int prevActive = peakActive.load(std::memory_order_relaxed);
      while (nowActive > prevActive && !peakActive.compare_exchange_weak(prevActive, nowActive)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      activeDecodes.fetch_sub(1, std::memory_order_acq_rel);
    });

    RecordingTextures textures;
    ThumbnailService service;
    ThumbnailServiceTestAccess::setMemoryBudget(service, tightBudget);
    for (const auto& p : files)
      (void)service.acquire(p.string(), 16);
    pumpUntil(service, textures, [&] {
      return std::ranges::all_of(files, [&](const auto& p) { return service.peek(p.string(), 16).id != 0; });
    });
    setThumbnailDecodeObserverHookForTesting({});
    assert(peakUsed.load(std::memory_order_acquire) <= tightBudget);
    assert(peakActive.load(std::memory_order_acquire) == 1);
  }

  // F-05.7: Interrupted cache write cleans up temp file and leaves no partial/corrupt entry.
  {
    const auto interruptedImg = root / "interrupted-write.png";
    const std::array<std::uint8_t, 4> yellow{255, 255, 0, 255};
    write(interruptedImg, encodePng(yellow.data(), 1, 1));

    std::atomic_bool hookHit{false};
    setThumbnailCacheWriteHookForTesting([&](const std::string& tempName, const std::string&) {
      assert(tempName.starts_with(".tmp-"));
      hookHit.store(true, std::memory_order_release);
      return false;
    });
    checkAlpha(load(interruptedImg, 8), 255);
    setThumbnailCacheWriteHookForTesting({});
    assert(hookHit.load(std::memory_order_acquire));

    const auto cacheDir = root / "nythera" / "thumbnails";
    for (const auto& entry : std::filesystem::directory_iterator(cacheDir))
      assert(!entry.path().filename().string().starts_with(".tmp-"));
  }

  // F-05.8: Cancellation (release during decode) and destruction while workers/subscriptions are active.
  {
    const auto cancelImgA = root / "cancel-a.png";
    const auto cancelImgB = root / "cancel-b.png";
    const std::array<std::uint8_t, 4> cyan{0, 255, 255, 255};
    write(cancelImgA, encodePng(cyan.data(), 1, 1));
    write(cancelImgB, encodePng(cyan.data(), 1, 1));

    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool releaseWorker = false;
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t) {
      std::unique_lock lock(mu);
      if (!entered) {
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return releaseWorker; });
      }
    });

    ThumbnailService::Subscription lingeringSub;
    {
      auto transient = std::make_unique<ThumbnailService>();
      lingeringSub = transient->subscribePendingUpload([] {});
      (void)transient->acquire(cancelImgA.string(), 8);
      (void)transient->acquire(cancelImgB.string(), 8);
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered; });
      }
      transient->release(cancelImgA.string(), 8);
      transient->release(cancelImgB.string(), 8);
      {
        std::scoped_lock lock(mu);
        releaseWorker = true;
      }
      cv.notify_all();
      transient.reset();
    }
    setThumbnailDecodeObserverHookForTesting({});
    // Disconnecting a Subscription after ThumbnailService destruction must be safe.
    lingeringSub.disconnect();

    // Re-entrant ReadyCallback destroying ThumbnailService during uploadPending().
    const auto selfDestructImg = root / "self-destruct.png";
    write(selfDestructImg, encodePng(cyan.data(), 1, 1));
    RecordingTextures textures;
    auto service = std::make_unique<ThumbnailService>();
    bool readyFired = false;
    auto readySub = service->subscribeReady(
        selfDestructImg.string(),
        [&](const std::string&, TextureHandle handle) {
          assert(handle.id != 0);
          readyFired = true;
          service.reset();
        },
        8
    );
    (void)service->acquire(selfDestructImg.string(), 8);
    std::vector<pollfd> fds;
    const auto index = service->addPollFds(fds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!readyFired && service && std::chrono::steady_clock::now() < deadline) {
      (void)poll(fds.data(), fds.size(), 50);
      service->dispatch(fds, index);
      if (service)
        (void)service->uploadPending(textures);
    }
    assert(readyFired && service == nullptr);
    readySub.disconnect();
  }

  // F-05.9: Oversized synthetic images and malformed inputs are rejected before large allocations.
  {
    auto crc32 = [](const std::uint8_t* data, std::size_t len) -> std::uint32_t {
      std::uint32_t crc = 0xFFFFFFFFU;
      for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
          crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : (crc >> 1U);
      }
      return ~crc;
    };
    auto writeBe32 = [](std::uint8_t* dst, std::uint32_t v) {
      dst[0] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
      dst[1] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
      dst[2] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
      dst[3] = static_cast<std::uint8_t>(v & 0xFFU);
    };

    // Synthetic PNG claiming 20000x20000 (1.6 GiB RGBA) inside a tiny <100-byte payload.
    const std::array<std::uint8_t, 4> px{255, 0, 0, 255};
    auto bombPng = encodePng(px.data(), 1, 1);
    assert(bombPng.size() >= 33);
    writeBe32(bombPng.data() + 16, 20000U);
    writeBe32(bombPng.data() + 20, 20000U);
    writeBe32(bombPng.data() + 29, crc32(bombPng.data() + 12, 17));

    const auto pngExtent = probeRasterImageExtent(bombPng.data(), bombPng.size());
    assert(pngExtent.has_value() && pngExtent->width == 20000 && pngExtent->height == 20000);
    assert(!decodeRasterImage(bombPng.data(), bombPng.size()).has_value());

    const auto bombPngPath = root / "bomb-20000x20000.png";
    write(bombPngPath, bombPng);
    {
      RecordingTextures textures;
      ThumbnailService service;
      (void)service.acquire(bombPngPath.string(), 64);
      pumpUntil(service, textures, [&] {
        return ThumbnailServiceTestAccess::isFailed(service, bombPngPath.string(), 64);
      });
      assert(service.peek(bombPngPath.string(), 64).id == 0);
    }

    // Synthetic BMP claiming 20000x20000 (54-byte header only).
    std::array<std::uint8_t, 54> bombBmp{};
    bombBmp[0] = 'B';
    bombBmp[1] = 'M';
    bombBmp[2] = 54;
    bombBmp[10] = 54;
    bombBmp[14] = 40;
    const std::uint32_t dim = 20000U;
    for (int i = 0; i < 4; ++i) {
      bombBmp[18 + i] = static_cast<std::uint8_t>((dim >> (8 * i)) & 0xFFU);
      bombBmp[22 + i] = static_cast<std::uint8_t>((dim >> (8 * i)) & 0xFFU);
    }
    bombBmp[26] = 1;
    bombBmp[28] = 32;
    const auto bmpExtent = probeRasterImageExtent(bombBmp.data(), bombBmp.size());
    assert(bmpExtent.has_value() && bmpExtent->width == 20000 && bmpExtent->height == 20000);
    assert(!decodeRasterImage(bombBmp.data(), bombBmp.size()).has_value());

    // Synthetic ICO with INT32_MIN BMP width (must not trigger signed-negation UB) and oversized PNG-in-ICO.
    std::vector<std::uint8_t> badIco(6 + 16 + 40, 0);
    badIco[2] = 1;         // ICO type
    badIco[4] = 1;         // 1 entry
    badIco[6 + 8] = 40;    // bytesInRes = 40
    badIco[6 + 12] = 22;   // imageOffset = 22
    badIco[22] = 40;       // BITMAPINFOHEADER size
    badIco[22 + 7] = 0x80; // dibWidth = INT32_MIN
    badIco[22 + 8] = 16;   // dibHeight = 16
    badIco[22 + 12] = 1;   // planes
    badIco[22 + 14] = 32;  // bpp
    assert(!decodeRasterImage(badIco.data(), badIco.size()).has_value());

    std::vector<std::uint8_t> bombIco(6 + 16 + bombPng.size(), 0);
    bombIco[2] = 1;
    bombIco[4] = 1;
    const auto pngLen = static_cast<std::uint32_t>(bombPng.size());
    for (int i = 0; i < 4; ++i)
      bombIco[6 + 8 + i] = static_cast<std::uint8_t>((pngLen >> (8 * i)) & 0xFFU);
    bombIco[6 + 12] = 22;
    std::ranges::copy(bombPng, bombIco.begin() + 22);
    assert(!decodeRasterImage(bombIco.data(), bombIco.size()).has_value());

    // Truncated / malformed PNG, WebP, and HEIF ftyp inputs must fail cleanly.
    assert(!decodeRasterImage(bombPng.data(), 20).has_value());
    constexpr std::array<std::uint8_t, 16> kBadWebp{'R', 'I', 'F', 'F', 0xFF, 0,   0,   0,
                                                    'W', 'E', 'B', 'P', 'V',  'P', '8', ' '};
    assert(!decodeRasterImage(kBadWebp.data(), kBadWebp.size()).has_value());
    constexpr std::array<std::uint8_t, 16> kBadHeif{0, 0, 0, 16, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0};
    assert(!decodeRasterImage(kBadHeif.data(), kBadHeif.size()).has_value());
  }

  // F-05.10: Concurrent decoding accounts for decoded RGBA memory even when compressed inputs are tiny.
  {
    // Solid 512x512 PNG compresses to ~1-3 KiB, while decoded RGBA is 1 MiB (working estimate ~2 MiB).
    std::vector<std::uint8_t> solid512(512ULL * 512ULL * 4ULL);
    for (std::size_t i = 0; i < solid512.size(); i += 4) {
      solid512[i] = 40;
      solid512[i + 1] = 120;
      solid512[i + 2] = 200;
      solid512[i + 3] = 255;
    }
    const auto smallCompressed = encodePng(solid512.data(), 512, 512);
    assert(smallCompressed.size() < 64ULL * 1024ULL);

    std::vector<std::filesystem::path> files;
    for (int i = 0; i < 4; ++i) {
      files.push_back(root / std::format("decoded-budget-{}.png", i));
      write(files.back(), smallCompressed);
    }

    // Budget is 2.5 MiB: 4 compressed files are < 256 KiB total, so only decoded-memory
    // reservation (~2 MiB per worker) forces serialization to 1 concurrent decode.
    const std::size_t decodedBudget = 2560ULL * 1024ULL;
    std::atomic_size_t peakUsed{0};
    std::atomic_int activeDecodes{0};
    std::atomic_int peakActive{0};
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t usedBytes) {
      const int cur = activeDecodes.fetch_add(1, std::memory_order_acq_rel) + 1;
      int expected = peakActive.load(std::memory_order_acquire);
      while (cur > expected && !peakActive.compare_exchange_weak(expected, cur)) {
      }
      std::size_t expUsed = peakUsed.load(std::memory_order_acquire);
      while (usedBytes > expUsed && !peakUsed.compare_exchange_weak(expUsed, usedBytes)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      activeDecodes.fetch_sub(1, std::memory_order_acq_rel);
    });

    RecordingTextures textures;
    ThumbnailService service;
    ThumbnailServiceTestAccess::setMemoryBudget(service, decodedBudget);
    for (const auto& p : files)
      (void)service.acquire(p.string(), 64);
    pumpUntil(service, textures, [&] {
      return std::ranges::all_of(files, [&](const auto& p) { return service.peek(p.string(), 64).id != 0; });
    });
    setThumbnailDecodeObserverHookForTesting({});
    assert(peakUsed.load(std::memory_order_acquire) <= decodedBudget);
    assert(peakUsed.load(std::memory_order_acquire) >= 2ULL * 1024ULL * 1024ULL);
    assert(peakActive.load(std::memory_order_acquire) == 1);
  }

  // SVG.1: DirectoryScanner eligibility for SVG/SVGZ preserves raster-only isImagePath().
  {
    assert(DirectoryScanner::isSvgPath("icon.svg"));
    assert(DirectoryScanner::isSvgPath("ICON.SVG"));
    assert(DirectoryScanner::isSvgPath("vector.svgz"));
    assert(DirectoryScanner::isSvgPath("VECTOR.SVGZ"));
    assert(!DirectoryScanner::isSvgPath("photo.png"));
    assert(!DirectoryScanner::isImagePath("icon.svg"));
    assert(!DirectoryScanner::isImagePath("vector.svgz"));
    assert(DirectoryScanner::isThumbnailPath("icon.svg"));
    assert(DirectoryScanner::isThumbnailPath("vector.svgz"));
    assert(DirectoryScanner::isThumbnailPath("photo.png"));
  }

  auto writeText = [](const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(out.good());
  };

  auto gzipCompress = [](std::string_view text) -> std::vector<std::uint8_t> {
    GInputStream* mem = g_memory_input_stream_new_from_data(text.data(), static_cast<gssize>(text.size()), nullptr);
    GZlibCompressor* comp = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
    GInputStream* conv = g_converter_input_stream_new(mem, G_CONVERTER(comp));
    g_object_unref(comp);
    g_object_unref(mem);
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 4096> buf{};
    GError* err = nullptr;
    while (true) {
      const gssize n = g_input_stream_read(conv, buf.data(), buf.size(), nullptr, &err);
      assert(n >= 0 && err == nullptr);
      if (n == 0)
        break;
      out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    g_object_unref(conv);
    return out;
  };

  // SVG.2: Valid SVGs preserve transparency, aspect ratio, and viewBox-only sizing.
  {
    const auto aspectSvg = root / "aspect-alpha.svg";
    writeText(
        aspectSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
  <rect x="0" y="0" width="100" height="100" fill="#ff0000" fill-opacity="0.5"/>
</svg>)SVG"
    );

    RecordingTextures textures;
    ThumbnailService service;
    (void)service.acquire(aspectSvg.string(), 64);
    pumpUntil(service, textures, [&] { return service.peek(aspectSvg.string(), 64).id != 0; });
    const TextureHandle handle = service.peek(aspectSvg.string(), 64);
    assert(handle.width == 64 && handle.height == 32);
    // Left side (x=16, y=16): semi-transparent red.
    const std::size_t leftIdx = (16U * 64U + 16U) * 4U;
    assert(textures.pixels[leftIdx + 0] >= 240);
    assert(textures.pixels[leftIdx + 1] <= 10);
    assert(textures.pixels[leftIdx + 2] <= 10);
    assert(std::abs(static_cast<int>(textures.pixels[leftIdx + 3]) - 128) <= 6);
    // Right side (x=48, y=16): fully transparent.
    const std::size_t rightIdx = (16U * 64U + 48U) * 4U;
    assert(textures.pixels[rightIdx + 3] == 0);

    // ViewBox-only tall SVG (1:2 aspect ratio).
    const auto viewboxSvg = root / "viewbox-only.svg";
    writeText(
        viewboxSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 200">
  <rect x="0" y="0" width="100" height="200" fill="#00ff00"/>
</svg>)SVG"
    );
    (void)service.acquire(viewboxSvg.string(), 64);
    pumpUntil(service, textures, [&] { return service.peek(viewboxSvg.string(), 64).id != 0; });
    const TextureHandle vbHandle = service.peek(viewboxSvg.string(), 64);
    assert(vbHandle.width == 32 && vbHandle.height == 64);
  }

  // SVG.3: Compressed SVGZ rendering and rejection of invalid/non-SVG gzip files.
  {
    const auto validSvgz = root / "valid.svgz";
    write(
        validSvgz,
        gzipCompress(
            R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="60" viewBox="0 0 120 60">
  <rect width="120" height="60" fill="#0080ff"/>
</svg>)SVG"
        )
    );
    RecordingTextures textures;
    ThumbnailService service;
    (void)service.acquire(validSvgz.string(), 64);
    pumpUntil(service, textures, [&] { return service.peek(validSvgz.string(), 64).id != 0; });
    const TextureHandle handle = service.peek(validSvgz.string(), 64);
    assert(handle.width == 64 && handle.height == 32);
    const std::size_t centerIdx = (16U * 64U + 32U) * 4U;
    assert(textures.pixels[centerIdx + 0] <= 10);
    assert(std::abs(static_cast<int>(textures.pixels[centerIdx + 1]) - 128) <= 6);
    assert(textures.pixels[centerIdx + 2] >= 240);
    assert(textures.pixels[centerIdx + 3] == 255);

    // Non-SVG gzip payload named .svgz must fail cleanly.
    const auto fakeSvgz = root / "not-svg.svgz";
    write(fakeSvgz, gzipCompress("just plain text, not xml or svg"));
    (void)service.acquire(fakeSvgz.string(), 64);
    pumpUntil(service, textures, [&] { return ThumbnailServiceTestAccess::isFailed(service, fakeSvgz.string(), 64); });
    assert(service.peek(fakeSvgz.string(), 64).id == 0);

    // Truncated/corrupt gzip payload named .svgz must fail cleanly.
    const auto corruptSvgz = root / "corrupt.svgz";
    write(corruptSvgz, {0x1F, 0x8B, 0x08, 0x00, 0xDE, 0xAD, 0xBE, 0xEF});
    (void)service.acquire(corruptSvgz.string(), 64);
    pumpUntil(service, textures, [&] {
      return ThumbnailServiceTestAccess::isFailed(service, corruptSvgz.string(), 64);
    });
    assert(service.peek(corruptSvgz.string(), 64).id == 0);
  }

  // SVG.4: Malformed, extreme-dimension, recursive <use>, and oversized SVGs are handled safely.
  {
    RecordingTextures textures;
    ThumbnailService service;

    // Malformed XML.
    const auto malformedSvg = root / "malformed.svg";
    writeText(malformedSvg, "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"10\"");
    (void)service.acquire(malformedSvg.string(), 64);
    pumpUntil(service, textures, [&] {
      return ThumbnailServiceTestAccess::isFailed(service, malformedSvg.string(), 64);
    });
    assert(service.peek(malformedSvg.string(), 64).id == 0);

    // Extreme intrinsic dimensions (100000x50000) render directly to thumbnail size (64x32).
    const auto hugeDimSvg = root / "extreme-dim.svg";
    writeText(
        hugeDimSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100000" height="50000" viewBox="0 0 100000 50000">
  <rect width="100000" height="50000" fill="#ff00ff"/>
</svg>)SVG"
    );
    (void)service.acquire(hugeDimSvg.string(), 64);
    pumpUntil(service, textures, [&] { return service.peek(hugeDimSvg.string(), 64).id != 0; });
    const TextureHandle hugeHandle = service.peek(hugeDimSvg.string(), 64);
    assert(hugeHandle.width == 64 && hugeHandle.height == 32);

    // Recursive <use> cycle must not hang or crash.
    const auto recursiveSvg = root / "recursive-use.svg";
    writeText(
        recursiveSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <defs>
    <g id="a"><use href="#b"/></g>
    <g id="b"><use href="#a"/></g>
  </defs>
  <use href="#a"/>
</svg>)SVG"
    );
    (void)service.acquire(recursiveSvg.string(), 64);
    pumpUntil(service, textures, [&] {
      return service.peek(recursiveSvg.string(), 64).id != 0
          || ThumbnailServiceTestAccess::isFailed(service, recursiveSvg.string(), 64);
    });

    // Oversized SVG (> kMaxSvgSourceBytes) rejected before reading.
    const auto oversizedSvg = root / "oversized.svg";
    {
      const int fd = ::open(oversizedSvg.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      assert(fd >= 0);
      const std::string_view header = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
      assert(::write(fd, header.data(), header.size()) == static_cast<ssize_t>(header.size()));
      assert(::ftruncate(fd, static_cast<off_t>(ThumbnailService::kMaxSvgSourceBytes + 1U)) == 0);
      ::close(fd);
    }
    (void)service.acquire(oversizedSvg.string(), 64);
    pumpUntil(service, textures, [&] {
      return ThumbnailServiceTestAccess::isFailed(service, oversizedSvg.string(), 64);
    });
    assert(service.peek(oversizedSvg.string(), 64).id == 0);
  }

  // SVG.5: External file://, relative, and http:// references are blocked.
  {
    const auto secretPng = root / "external-secret.png";
    std::vector<std::uint8_t> solidRed(64U * 64U * 4U);
    for (std::size_t i = 0; i < solidRed.size(); i += 4) {
      solidRed[i + 0] = 255;
      solidRed[i + 1] = 0;
      solidRed[i + 2] = 0;
      solidRed[i + 3] = 255;
    }
    write(secretPng, encodePng(solidRed.data(), 64, 64));

    const auto externalSvg = root / "external-refs.svg";
    writeText(
        externalSvg,
        std::format(
            R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <rect width="64" height="64" fill="#0000ff"/>
  <image href="file://{}" x="0" y="0" width="64" height="64"/>
  <image href="external-secret.png" x="0" y="0" width="64" height="64"/>
  <image href="http://127.0.0.1:1/never-fetched.png" x="0" y="0" width="64" height="64"/>
</svg>)SVG",
            secretPng.string()
        )
    );

    RecordingTextures textures;
    ThumbnailService service;
    (void)service.acquire(externalSvg.string(), 64);
    pumpUntil(service, textures, [&] { return service.peek(externalSvg.string(), 64).id != 0; });
    const std::size_t centerIdx = (32U * 64U + 32U) * 4U;
    // Must show the SVG's own blue rect, never the external red PNG.
    assert(textures.pixels[centerIdx + 0] <= 10);
    assert(textures.pixels[centerIdx + 1] <= 10);
    assert(textures.pixels[centerIdx + 2] >= 240);
  }

  // SVG.6: Concurrent SVG rendering respects memory budget and cancellation is safe.
  {
    std::vector<std::filesystem::path> svgFiles;
    for (int i = 0; i < 4; ++i) {
      svgFiles.push_back(root / std::format("concurrent-{}.svg", i));
      writeText(
          svgFiles.back(),
          R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
  <circle cx="48" cy="48" r="40" fill="#ffaa00"/>
</svg>)SVG"
      );
    }

    // For targetPx=96: working estimate is 96*96*4*2 = 73728 bytes (~72 KiB) + SVG bytes.
    // A 100 KiB budget forces serialization to 1 concurrent SVG render.
    const std::size_t svgBudget = 100ULL * 1024ULL;
    std::atomic_size_t peakUsed{0};
    std::atomic_int activeDecodes{0};
    std::atomic_int peakActive{0};
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t usedBytes) {
      const int cur = activeDecodes.fetch_add(1, std::memory_order_acq_rel) + 1;
      int expected = peakActive.load(std::memory_order_acquire);
      while (cur > expected && !peakActive.compare_exchange_weak(expected, cur)) {
      }
      std::size_t expUsed = peakUsed.load(std::memory_order_acquire);
      while (usedBytes > expUsed && !peakUsed.compare_exchange_weak(expUsed, usedBytes)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      activeDecodes.fetch_sub(1, std::memory_order_acq_rel);
    });

    RecordingTextures textures;
    ThumbnailService service;
    ThumbnailServiceTestAccess::setMemoryBudget(service, svgBudget);
    for (const auto& p : svgFiles)
      (void)service.acquire(p.string(), 96);
    pumpUntil(service, textures, [&] {
      return std::ranges::all_of(svgFiles, [&](const auto& p) { return service.peek(p.string(), 96).id != 0; });
    });
    setThumbnailDecodeObserverHookForTesting({});
    assert(peakUsed.load(std::memory_order_acquire) <= svgBudget);
    assert(peakActive.load(std::memory_order_acquire) == 1);

    // Cancellation while an SVG worker is in-flight.
    const auto cancelSvg = root / "cancel-svg.svg";
    writeText(
        cancelSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="red"/></svg>)SVG"
    );
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool allowExit = false;
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t) {
      std::unique_lock lock(mu);
      entered = true;
      cv.notify_all();
      cv.wait(lock, [&] { return allowExit; });
    });
    {
      ThumbnailService transient;
      (void)transient.acquire(cancelSvg.string(), 64);
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered; });
      }
      transient.release(cancelSvg.string(), 64);
      {
        std::scoped_lock lock(mu);
        allowExit = true;
      }
      cv.notify_all();
    }
    setThumbnailDecodeObserverHookForTesting({});
  }

  // SVG.7: Cache creation, reuse on hit, and invalidation when SVG content changes.
  {
    const auto cachedSvg = root / "cache-lifecycle.svg";
    writeText(
        cachedSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">
  <rect width="32" height="32" fill="#ff0000"/>
</svg>)SVG"
    );

    // First load: renders SVG and writes cache.
    std::atomic_int decodeHits{0};
    setThumbnailDecodeObserverHookForTesting([&](const std::string&, std::size_t) {
      decodeHits.fetch_add(1, std::memory_order_acq_rel);
    });
    {
      RecordingTextures textures;
      ThumbnailService service;
      (void)service.acquire(cachedSvg.string(), 32);
      pumpUntil(service, textures, [&] { return service.peek(cachedSvg.string(), 32).id != 0; });
      assert(decodeHits.load(std::memory_order_acquire) == 1);
      assert(textures.pixels[0] >= 240 && textures.pixels[1] <= 10);
    }

    // Second load with fresh service: must hit disk cache (decodeHits stays 1).
    {
      RecordingTextures textures;
      ThumbnailService service;
      (void)service.acquire(cachedSvg.string(), 32);
      pumpUntil(service, textures, [&] { return service.peek(cachedSvg.string(), 32).id != 0; });
      assert(decodeHits.load(std::memory_order_acquire) == 1);
      assert(textures.pixels[0] >= 240 && textures.pixels[1] <= 10);
    }

    // Modify SVG content (changes size & mtime): cache must be invalidated and re-rendered green.
    writeText(
        cachedSvg,
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">
  <rect width="32" height="32" fill="#00ff00"/><!-- modified to invalidate cache -->
</svg>)SVG"
    );
    {
      RecordingTextures textures;
      ThumbnailService service;
      (void)service.acquire(cachedSvg.string(), 32);
      pumpUntil(service, textures, [&] { return service.peek(cachedSvg.string(), 32).id != 0; });
      assert(decodeHits.load(std::memory_order_acquire) == 2);
      assert(textures.pixels[0] <= 10 && textures.pixels[1] >= 240);
    }
    setThumbnailDecodeObserverHookForTesting({});
  }

  if (hadCache)
    assert(setenv("XDG_CACHE_HOME", previousCache.c_str(), 1) == 0);
  else
    assert(unsetenv("XDG_CACHE_HOME") == 0);
  std::filesystem::remove_all(root);
  std::println("thumbnail service: alpha, symlink safety, private atomic cache, memory budget and lifecycle passed");
}
