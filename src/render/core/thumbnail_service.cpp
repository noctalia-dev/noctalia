#include "render/core/thumbnail_service.h"

#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"
#include "render/core/texture_manager.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <gio/gio.h>
#include <glib.h>
#include <limits>
#include <optional>
#include <random>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#include <webp/encode.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include <librsvg/rsvg.h>
#pragma GCC diagnostic pop

namespace {

  constexpr Logger kLog("thumbnail");
  constexpr float kThumbnailWebPQuality = 82.0F;
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;
  constexpr std::string_view kThumbnailCacheVersion = "thumbnail-service-v6";

  std::mutex g_testHookMutex;
  std::function<bool(const std::string&, const std::string&)> g_cacheWriteTestHook;
  std::function<void(const std::string&, std::size_t)> g_decodeObserverHook;

  class OwnedFd {
  public:
    OwnedFd() = default;
    explicit OwnedFd(int fd) noexcept : m_fd(fd) {}
    ~OwnedFd() { reset(); }

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

    OwnedFd(OwnedFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
      if (this != &other) {
        reset();
        m_fd = std::exchange(other.m_fd, -1);
      }
      return *this;
    }

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_fd >= 0; }

    int release() noexcept { return std::exchange(m_fd, -1); }

    bool closeChecked() noexcept {
      if (m_fd < 0) {
        return true;
      }
      const int fd = std::exchange(m_fd, -1);
      return ::close(fd) == 0;
    }

    void reset() noexcept {
      if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
      }
    }

  private:
    int m_fd = -1;
  };

  std::optional<std::filesystem::path> thumbnailCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      const std::filesystem::path base(xdg);
      if (base.is_absolute()) {
        return base / "nythera" / "thumbnails";
      }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      const std::filesystem::path base(home);
      if (base.is_absolute()) {
        return base / ".cache" / "nythera" / "thumbnails";
      }
    }
    // Never fall back to a shared /tmp directory when no user cache directory is configured.
    return std::nullopt;
  }

  OwnedFd openPrivateCacheDir(bool create) {
    const auto dir = thumbnailCacheDir();
    if (!dir.has_value()) {
      return {};
    }

    if (create) {
      std::error_code ec;
      if (!FileUtils::createPrivateDirectories(*dir, ec)) {
        return {};
      }
    }

    OwnedFd dirFd(::open(dir->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dirFd) {
      return {};
    }

    struct stat st{};
    if (::fstat(dirFd.get(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
      return {};
    }

    if ((st.st_mode & 0077) != 0) {
      if (::fchmod(dirFd.get(), 0700) != 0) {
        return {};
      }
      if (::fstat(dirFd.get(), &st) != 0 || (st.st_mode & 0077) != 0) {
        return {};
      }
    }

    return dirFd;
  }

  std::string sha256Hex(std::string_view text) {
    g_autofree gchar* digest =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(text.data()), text.size());
    return digest != nullptr ? std::string(digest) : std::string{};
  }

  std::string cacheLeafForStat(const std::string& sourcePath, const struct stat& st, int targetPx) {
    const std::string key = sourcePath
        + '\n'
        + std::to_string(static_cast<long long>(st.st_size))
        + '\n'
        + std::to_string(static_cast<long long>(st.st_mtim.tv_sec))
        + '.'
        + std::to_string(static_cast<long long>(st.st_mtim.tv_nsec))
        + '\n'
        + std::to_string(targetPx)
        + '\n'
        + std::string(kThumbnailCacheVersion);
    const std::string digest = sha256Hex(key);
    if (digest.empty()) {
      return {};
    }
    return digest + ".webp";
  }

  bool inspectRegularSource(const std::string& sourcePath, struct stat& st) {
    if (sourcePath.empty()) {
      return false;
    }
    if (::lstat(sourcePath.c_str(), &st) != 0) {
      return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
      return false;
    }
    return static_cast<std::uint64_t>(st.st_size) <= ThumbnailService::kMaxSourceFileBytes;
  }

  OwnedFd openRegularSource(const std::string& sourcePath, struct stat& st) {
    if (sourcePath.empty()) {
      return {};
    }
    OwnedFd fd(::open(sourcePath.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
      return {};
    }
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
      return {};
    }
    if (static_cast<std::uint64_t>(st.st_size) > ThumbnailService::kMaxSourceFileBytes) {
      return {};
    }
    return fd;
  }

  std::vector<std::uint8_t> readBoundedFd(int fd, std::size_t maxBytes, std::size_t reserveHint) {
    std::vector<std::uint8_t> out;
    out.reserve(std::min(reserveHint, maxBytes));

    std::array<std::uint8_t, 65536> chunk{};
    while (true) {
      const ssize_t n = ::read(fd, chunk.data(), chunk.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return {};
      }
      if (n == 0) {
        break;
      }
      const auto count = static_cast<std::size_t>(n);
      if (out.size() > maxBytes || count > maxBytes - out.size()) {
        return {};
      }
      out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return out;
  }

  std::vector<std::uint8_t> readCacheEntry(int dirFd, const std::string& leaf) {
    if (dirFd < 0 || leaf.empty()) {
      return {};
    }
    OwnedFd fd(::openat(dirFd, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
      return {};
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0
        || !S_ISREG(st.st_mode)
        || st.st_uid != ::geteuid()
        || st.st_size <= 0
        || static_cast<std::uint64_t>(st.st_size) > ThumbnailService::kMaxCacheFileBytes) {
      return {};
    }
    return readBoundedFd(fd.get(), ThumbnailService::kMaxCacheFileBytes, static_cast<std::size_t>(st.st_size));
  }

  void removeCorruptCacheEntry(int dirFd, const std::string& leaf) {
    if (dirFd < 0 || leaf.empty()) {
      return;
    }
    struct stat st{};
    if (::fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(st.st_mode)
        && st.st_uid == ::geteuid()) {
      (void)::unlinkat(dirFd, leaf.c_str(), 0);
    }
  }

  bool writeAllFd(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t n = ::write(fd, data + offset, size - offset);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (n == 0) {
        return false;
      }
      offset += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool writeCacheEntryAtomic(int dirFd, const std::string& leaf, const std::uint8_t* data, std::size_t size) {
    if (dirFd < 0 || leaf.empty() || data == nullptr || size == 0) {
      return false;
    }

    struct stat existing{};
    if (::fstatat(dirFd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISREG(existing.st_mode) || existing.st_uid != ::geteuid()) {
        return false;
      }
    } else if (errno != ENOENT) {
      return false;
    }

    thread_local std::mt19937_64 rng(std::random_device{}());
    const auto pid = static_cast<unsigned long>(::getpid());

    OwnedFd tempFd;
    std::string tempName;
    for (int attempt = 0; attempt < 32; ++attempt) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), ".tmp-%016llx-%lu.webp", static_cast<unsigned long long>(rng()), pid);
      tempName = buf;
      const int fd = ::openat(dirFd, tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd >= 0) {
        tempFd = OwnedFd(fd);
        break;
      }
      if (errno != EEXIST) {
        return false;
      }
    }
    if (!tempFd) {
      return false;
    }

    bool ok = (::fchmod(tempFd.get(), 0600) == 0) && writeAllFd(tempFd.get(), data, size);
    if (!tempFd.closeChecked()) {
      ok = false;
    }

    if (ok) {
      std::function<bool(const std::string&, const std::string&)> hook;
      {
        std::scoped_lock lock(g_testHookMutex);
        hook = g_cacheWriteTestHook;
      }
      if (hook && !hook(tempName, leaf)) {
        ok = false;
      }
    }

    if (ok) {
      if (::fstatat(dirFd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0
          && (!S_ISREG(existing.st_mode) || existing.st_uid != ::geteuid())) {
        ok = false;
      }
    }

    if (ok && ::renameat(dirFd, tempName.c_str(), dirFd, leaf.c_str()) != 0) {
      ok = false;
    }

    if (!ok) {
      (void)::unlinkat(dirFd, tempName.c_str(), 0);
      return false;
    }
    return true;
  }

  // Keep straight alpha; the shared resampler weights colour by alpha when filtering.
  bool resizeThumbnail(std::vector<std::uint8_t>& rgba, int& width, int& height, int targetPx) {
    const ImageExtent fitted = fitImageWithin(width, height, targetPx, targetPx);
    if (fitted.width == width && fitted.height == height) {
      return true;
    }

    auto resized = resizeToRgba(rgba.data(), width, height, ResizeSourceLayout::Rgba, fitted.width, fitted.height);
    if (resized.empty()) {
      return false;
    }
    rgba = std::move(resized);
    width = fitted.width;
    height = fitted.height;
    return true;
  }

  [[nodiscard]] bool isGzipHeader(const std::vector<std::uint8_t>& data) noexcept {
    return data.size() >= 2 && data[0] == 0x1FU && data[1] == 0x8BU;
  }

  [[nodiscard]] bool asciiWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

  [[nodiscard]] char asciiLower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
  }

  [[nodiscard]] bool looksLikeSvg(const std::vector<std::uint8_t>& data) noexcept {
    std::size_t pos = (data.size() >= 3 && data[0] == 0xEFU && data[1] == 0xBBU && data[2] == 0xBFU) ? 3U : 0U;
    while (pos < data.size() && asciiWhitespace(static_cast<char>(data[pos]))) {
      ++pos;
    }
    if (pos >= data.size() || data[pos] != '<') {
      return false;
    }
    const std::size_t scanEnd = std::min(data.size(), pos + 4096U);
    for (; pos + 4 <= scanEnd; ++pos) {
      if (data[pos] == '<'
          && asciiLower(static_cast<char>(data[pos + 1])) == 's'
          && asciiLower(static_cast<char>(data[pos + 2])) == 'v'
          && asciiLower(static_cast<char>(data[pos + 3])) == 'g') {
        if (pos + 4 == data.size()) {
          return true;
        }
        const char next = static_cast<char>(data[pos + 4]);
        if (asciiWhitespace(next) || next == '>' || next == '/' || next == ':') {
          return true;
        }
      }
    }
    return false;
  }

  std::expected<std::vector<std::uint8_t>, std::string>
  decompressGzipBounded(const std::uint8_t* data, std::size_t size, std::size_t maxOutputBytes) {
    if (data == nullptr || size == 0) {
      return std::unexpected("empty compressed SVGZ input");
    }
    GInputStream* memStream = g_memory_input_stream_new_from_data(data, static_cast<gssize>(size), nullptr);
    GZlibDecompressor* decompressor = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
    GInputStream* convStream = g_converter_input_stream_new(memStream, G_CONVERTER(decompressor));
    g_object_unref(decompressor);
    g_object_unref(memStream);

    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 16384> chunk{};
    GError* err = nullptr;
    while (true) {
      const gssize n = g_input_stream_read(convStream, chunk.data(), chunk.size(), nullptr, &err);
      if (n < 0) {
        std::string msg = err != nullptr ? err->message : "gzip decompression failed";
        if (err != nullptr) {
          g_error_free(err);
        }
        g_object_unref(convStream);
        return std::unexpected(std::move(msg));
      }
      if (n == 0) {
        break;
      }
      if (out.size() + static_cast<std::size_t>(n) > maxOutputBytes) {
        g_object_unref(convStream);
        return std::unexpected("decompressed SVGZ exceeds size limit");
      }
      out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    }
    g_object_unref(convStream);
    return out;
  }

  void argb32ToRgba(const unsigned char* src, int srcStride, std::uint8_t* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
      const auto* row = reinterpret_cast<const std::uint32_t*>(src + (y * srcStride));
      std::uint8_t* outRow = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
      for (int x = 0; x < width; ++x) {
        const std::uint32_t pixel = row[x];
        const auto a = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
        auto r = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
        auto g = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
        auto b = static_cast<std::uint8_t>(pixel & 0xFFU);
        if (a != 0 && a != 255) {
          r = static_cast<std::uint8_t>(std::min(255, ((r * 255) + (a / 2)) / a));
          g = static_cast<std::uint8_t>(std::min(255, ((g * 255) + (a / 2)) / a));
          b = static_cast<std::uint8_t>(std::min(255, ((b * 255) + (a / 2)) / a));
        }
        outRow[(x * 4) + 0] = r;
        outRow[(x * 4) + 1] = g;
        outRow[(x * 4) + 2] = b;
        outRow[(x * 4) + 3] = a;
      }
    }
  }

  std::expected<DecodedRasterImage, std::string>
  rasterizeSvgThumbnail(const std::uint8_t* data, std::size_t size, int clampedTargetPx) {
    if (data == nullptr || size == 0 || clampedTargetPx <= 0) {
      return std::unexpected("invalid SVG thumbnail input");
    }

    // Load from memory with base_file == nullptr and RSVG_HANDLE_FLAGS_NONE:
    // blocks external file:// and relative URIs, prevents network loads, and keeps
    // libxml2 / librsvg element, nesting, and recursion safety limits active.
    GInputStream* memStream = g_memory_input_stream_new_from_data(data, static_cast<gssize>(size), nullptr);
    GError* gerror = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_stream_sync(memStream, nullptr, RSVG_HANDLE_FLAGS_NONE, nullptr, &gerror);
    g_object_unref(memStream);
    if (handle == nullptr) {
      std::string error = std::string("failed to parse SVG: ") + (gerror != nullptr ? gerror->message : "unknown");
      if (gerror != nullptr) {
        g_error_free(gerror);
      }
      return std::unexpected(std::move(error));
    }

    gdouble intrinsicW = 0.0;
    gdouble intrinsicH = 0.0;
    const gboolean hasIntrinsic = rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsicW, &intrinsicH);
    if (hasIntrinsic == FALSE
        || !std::isfinite(intrinsicW)
        || !std::isfinite(intrinsicH)
        || intrinsicW <= 0.0
        || intrinsicH <= 0.0) {
      gboolean outHasW = FALSE;
      RsvgLength outW{};
      gboolean outHasH = FALSE;
      RsvgLength outH{};
      gboolean outHasViewbox = FALSE;
      RsvgRectangle outViewbox{};
      rsvg_handle_get_intrinsic_dimensions(handle, &outHasW, &outW, &outHasH, &outH, &outHasViewbox, &outViewbox);
      if (outHasViewbox == TRUE) {
        if (!std::isfinite(outViewbox.width)
            || !std::isfinite(outViewbox.height)
            || outViewbox.width <= 0.0
            || outViewbox.height <= 0.0) {
          g_object_unref(handle);
          return std::unexpected("invalid SVG viewBox dimensions");
        }
        intrinsicW = outViewbox.width;
        intrinsicH = outViewbox.height;
      } else if (
          outHasW == TRUE && outHasH == TRUE && outW.unit != RSVG_UNIT_PERCENT && outH.unit != RSVG_UNIT_PERCENT
      ) {
        if (!std::isfinite(outW.length) || !std::isfinite(outH.length) || outW.length <= 0.0 || outH.length <= 0.0) {
          g_object_unref(handle);
          return std::unexpected("invalid SVG intrinsic dimensions");
        }
        intrinsicW = outW.length;
        intrinsicH = outH.length;
      } else {
        intrinsicW = static_cast<double>(clampedTargetPx);
        intrinsicH = static_cast<double>(clampedTargetPx);
      }
    }

    const double maxSide = std::max(intrinsicW, intrinsicH);
    if (!std::isfinite(maxSide) || maxSide <= 0.0) {
      g_object_unref(handle);
      return std::unexpected("invalid SVG aspect ratio");
    }
    const double scale = static_cast<double>(clampedTargetPx) / maxSide;
    const int width = std::clamp(static_cast<int>(std::lround(intrinsicW * scale)), 1, clampedTargetPx);
    const int height = std::clamp(static_cast<int>(std::lround(intrinsicH * scale)), 1, clampedTargetPx);

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::unexpected("failed to create cairo surface for SVG thumbnail");
    }

    cairo_t* cr = cairo_create(surface);
    const RsvgRectangle viewport{
        .x = 0.0,
        .y = 0.0,
        .width = static_cast<double>(width),
        .height = static_cast<double>(height),
    };
    GError* renderError = nullptr;
    if (rsvg_handle_render_document(handle, cr, &viewport, &renderError) == FALSE) {
      std::string error =
          std::string("failed to render SVG: ") + (renderError != nullptr ? renderError->message : "unknown");
      if (renderError != nullptr) {
        g_error_free(renderError);
      }
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::unexpected(std::move(error));
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    DecodedRasterImage decoded{
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U),
        .width = width,
        .height = height,
    };
    argb32ToRgba(
        cairo_image_surface_get_data(surface), cairo_image_surface_get_stride(surface), decoded.pixels.data(), width,
        height
    );

    cairo_surface_destroy(surface);
    g_object_unref(handle);
    return decoded;
  }

} // namespace

std::size_t ThumbnailService::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  const std::size_t pathHash = std::hash<std::string>{}(key.path);
  const std::size_t sizeHash = std::hash<int>{}(key.targetPx);
  return pathHash ^ (sizeHash + 0x9e3779b97f4a7c15ULL + (pathHash << 6U) + (pathHash >> 2U));
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
    m_workers.emplace_back([this] { workerLoop(); });
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
  {
    std::scoped_lock lock(m_memoryMutex);
  }
  m_memoryCv.notify_all();
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
  return Subscription([this, token, id] {
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
  return Subscription([this, token, id] {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_readyListeners.erase(id);
  });
}

TextureHandle ThumbnailService::acquire(const std::string& path, int targetPx, bool diskCache) {
  if (path.empty()) {
    return {};
  }

  const RequestKey key{.path = path, .targetPx = targetPx, .diskCache = diskCache};
  CacheEntry& entry = m_entries[key];
  ++entry.refCount;
  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  enqueueDecodeIfNeeded(key);
  return {};
}

TextureHandle ThumbnailService::peek(const std::string& path, int targetPx, bool diskCache) const {
  const auto it = m_entries.find(RequestKey{.path = path, .targetPx = targetPx, .diskCache = diskCache});
  if (it == m_entries.end()) {
    return {};
  }
  return it->second.handle;
}

void ThumbnailService::release(const std::string& path, int targetPx, bool diskCache) {
  const RequestKey key{.path = path, .targetPx = targetPx, .diskCache = diskCache};
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

  bool canceledInFlight = false;
  {
    std::scoped_lock lock(m_queueMutex);
    if (m_inFlight.contains(key)) {
      m_canceled.insert(key);
      canceledInFlight = true;
    }
  }
  if (canceledInFlight) {
    {
      std::scoped_lock lock(m_memoryMutex);
    }
    m_memoryCv.notify_all();
  }
}

bool ThumbnailService::isCanceledOrShuttingDown(const RequestKey& key) const {
  if (m_shutdown.load()) {
    return true;
  }
  std::scoped_lock lock(m_queueMutex);
  return m_canceled.contains(key);
}

bool ThumbnailService::tryAbortIfCanceled(const RequestKey& key) {
  std::scoped_lock lock(m_queueMutex);
  if (m_shutdown.load()) {
    m_canceled.erase(key);
    m_inFlight.erase(key);
    return true;
  }
  if (m_canceled.erase(key) > 0) {
    m_inFlight.erase(key);
    return true;
  }
  return false;
}

bool ThumbnailService::acquireMemoryBudget(const RequestKey& key, std::size_t bytes) {
  std::unique_lock<std::mutex> lock(m_memoryMutex);
  const std::size_t reservation = std::min(bytes, m_memoryBudgetBytes);
  m_memoryCv.wait(lock, [this, &key, reservation] {
    return isCanceledOrShuttingDown(key)
        || m_usedMemoryBytes == 0
        || m_usedMemoryBytes + reservation <= m_memoryBudgetBytes;
  });
  if (isCanceledOrShuttingDown(key)) {
    return false;
  }
  m_usedMemoryBytes += reservation;
  return true;
}

void ThumbnailService::releaseMemoryBudget(std::size_t bytes) {
  {
    std::scoped_lock lock(m_memoryMutex);
    const std::size_t reservation = std::min(bytes, m_memoryBudgetBytes);
    m_usedMemoryBytes = m_usedMemoryBytes > reservation ? (m_usedMemoryBytes - reservation) : 0;
  }
  m_memoryCv.notify_all();
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

  const auto alive = m_lifetimeToken;
  bool changed = false;
  for (auto& job : jobs) {
    if (!*alive) {
      return changed;
    }
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
    if (!*alive) {
      return changed;
    }
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

void ThumbnailService::abandonGpuResources() noexcept {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    entry.handle = {};
    entry.failed = false;
  }
  m_textureManager = nullptr;
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
  const auto alive = m_lifetimeToken;
  std::vector<std::pair<std::uint64_t, PendingUploadCallback>> callbacks;
  callbacks.reserve(m_pendingListeners.size());
  for (const auto& [id, listener] : m_pendingListeners) {
    if (listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (auto& [id, callback] : callbacks) {
    if (!*alive) {
      return;
    }
    if (!m_pendingListeners.contains(id)) {
      continue;
    }
    callback();
  }
}

void ThumbnailService::notifyReady(const RequestKey& key, TextureHandle handle) {
  const auto alive = m_lifetimeToken;
  std::vector<std::pair<std::uint64_t, ReadyCallback>> callbacks;
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (const auto& [id, callback] : callbacks) {
    if (!*alive) {
      return;
    }
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
      m_queueCv.wait(lock, [this] { return m_shutdown.load() || !m_jobQueue.empty(); });
      if (m_shutdown.load()) {
        return;
      }
      key = std::move(m_jobQueue.front());
      m_jobQueue.pop_front();
      if (m_canceled.erase(key) > 0) {
        m_inFlight.erase(key);
        continue;
      }
    }

    const std::string& path = key.path;
    DecodedJob result;
    result.key = key;

    struct stat inspectedStat{};
    if (!inspectRegularSource(path, inspectedStat)) {
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    if (OwnedFd cacheDir = key.diskCache ? openPrivateCacheDir(false) : OwnedFd{}; cacheDir) {
      const std::string leaf = cacheLeafForStat(path, inspectedStat, key.targetPx);
      auto cachedBytes = readCacheEntry(cacheDir.get(), leaf);
      if (!cachedBytes.empty()) {
        const RasterDecodeLimits cacheDecodeLimits{
            .maxDimension = std::max(key.targetPx * 2, 2048),
            .maxDecodedBytes = kMaxCacheFileBytes * 4ULL,
        };
        if (auto cached = decodeRasterImage(cachedBytes.data(), cachedBytes.size(), cacheDecodeLimits)) {
          if (tryAbortIfCanceled(key)) {
            continue;
          }
          result.rgba = std::move(cached->pixels);
          result.width = cached->width;
          result.height = cached->height;
          pushResult(std::move(result));
          continue;
        }
        removeCorruptCacheEntry(cacheDir.get(), leaf);
      }
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    struct stat openedStat{};
    OwnedFd sourceFd = openRegularSource(path, openedStat);
    if (!sourceFd) {
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    const auto sourceSize = static_cast<std::size_t>(openedStat.st_size);
    const bool svgExt = DirectoryScanner::isSvgPath(path);
    const std::size_t maxSourceForPath = svgExt ? kMaxSvgSourceBytes : kMaxSourceFileBytes;
    if (sourceSize > maxSourceForPath) {
      kLog.warn("rejecting oversized thumbnail source {} ({} bytes > {} limit)", path, sourceSize, maxSourceForPath);
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    if (!acquireMemoryBudget(key, sourceSize)) {
      (void)tryAbortIfCanceled(key);
      continue;
    }

    struct MemoryLease {
      ThumbnailService* service = nullptr;
      std::size_t bytes = 0;
      void release() {
        if (service != nullptr) {
          service->releaseMemoryBudget(bytes);
          service = nullptr;
          bytes = 0;
        }
      }
      ~MemoryLease() { release(); }
    } lease{this, sourceSize};

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    auto bytes = readBoundedFd(sourceFd.get(), maxSourceForPath, sourceSize);
    sourceFd.reset();
    if (bytes.empty()) {
      lease.release();
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    bool isSvg = false;
    if (svgExt && isGzipHeader(bytes)) {
      auto decompressed = decompressGzipBounded(bytes.data(), bytes.size(), kMaxSvgSourceBytes);
      bytes.clear();
      bytes.shrink_to_fit();
      if (!decompressed || !looksLikeSvg(*decompressed)) {
        kLog.warn(
            "rejecting invalid SVGZ thumbnail source {}: {}", path,
            decompressed ? "decompressed payload is not SVG" : decompressed.error()
        );
        lease.release();
        if (tryAbortIfCanceled(key)) {
          continue;
        }
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }
      bytes = std::move(*decompressed);
      isSvg = true;
    } else if (svgExt || looksLikeSvg(bytes)) {
      if (bytes.size() > kMaxSvgSourceBytes || !looksLikeSvg(bytes)) {
        kLog.warn("rejecting invalid or oversized SVG thumbnail source {}", path);
        lease.release();
        if (tryAbortIfCanceled(key)) {
          continue;
        }
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }
      isSvg = true;
    }

    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> pixels;

    if (isSvg) {
      const int clampedTargetPx = std::clamp(key.targetPx > 0 ? key.targetPx : kDefaultTargetPx, 1, kMaxSvgRenderPx);
      const std::uint64_t surfaceBytes =
          static_cast<std::uint64_t>(clampedTargetPx) * static_cast<std::uint64_t>(clampedTargetPx) * 4ULL;
      const std::uint64_t workingBytes = surfaceBytes * 2ULL;
      const std::uint64_t totalEstimate = workingBytes <= (std::numeric_limits<std::uint64_t>::max() - bytes.size())
          ? static_cast<std::uint64_t>(bytes.size()) + workingBytes
          : std::numeric_limits<std::uint64_t>::max();
      const std::size_t decodeReserveBytes =
          static_cast<std::size_t>(std::min<std::uint64_t>(totalEstimate, std::numeric_limits<std::size_t>::max()));

      if (decodeReserveBytes > lease.bytes) {
        lease.release();
        if (!acquireMemoryBudget(key, decodeReserveBytes)) {
          (void)tryAbortIfCanceled(key);
          continue;
        }
        lease.service = this;
        lease.bytes = decodeReserveBytes;
      }

      {
        std::function<void(const std::string&, std::size_t)> observer;
        std::size_t usedNow = 0;
        {
          std::scoped_lock lock(g_testHookMutex);
          observer = g_decodeObserverHook;
        }
        if (observer) {
          {
            std::scoped_lock lock(m_memoryMutex);
            usedNow = m_usedMemoryBytes;
          }
          observer(path, usedNow);
        }
      }

      if (tryAbortIfCanceled(key)) {
        continue;
      }

      auto rendered = rasterizeSvgThumbnail(bytes.data(), bytes.size(), clampedTargetPx);
      bytes.clear();
      bytes.shrink_to_fit();
      if (!rendered) {
        kLog.warn("failed to rasterize SVG thumbnail {}: {}", path, rendered.error());
        lease.release();
        if (tryAbortIfCanceled(key)) {
          continue;
        }
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }

      w = rendered->width;
      h = rendered->height;
      pixels = std::move(rendered->pixels);
      lease.release();
    } else {
      const RasterDecodeLimits decodeLimits{
          .maxDimension = kMaxImageDimension,
          .maxDecodedBytes = kMaxDecodedBytes,
      };
      std::size_t decodeReserveBytes = bytes.size();
      if (const auto extent = probeRasterImageExtent(bytes.data(), bytes.size())) {
        const std::uint64_t decodedBytes =
            static_cast<std::uint64_t>(extent->width) * static_cast<std::uint64_t>(extent->height) * 4ULL;
        if (extent->width <= 0
            || extent->height <= 0
            || extent->width > decodeLimits.maxDimension
            || extent->height > decodeLimits.maxDimension
            || decodedBytes > decodeLimits.maxDecodedBytes) {
          kLog.warn(
              "rejecting oversized thumbnail source {} ({}x{}, {} bytes decoded)", path, extent->width, extent->height,
              decodedBytes
          );
          lease.release();
          if (tryAbortIfCanceled(key)) {
            continue;
          }
          result.failed = true;
          pushResult(std::move(result));
          continue;
        }
        // Account for compressed input + decoded RGBA pixels + decoder/resize working buffer.
        const std::uint64_t workingBytes = decodedBytes <= (std::numeric_limits<std::uint64_t>::max() / 2ULL)
            ? decodedBytes * 2ULL
            : std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t totalEstimate = workingBytes <= (std::numeric_limits<std::uint64_t>::max() - bytes.size())
            ? static_cast<std::uint64_t>(bytes.size()) + workingBytes
            : std::numeric_limits<std::uint64_t>::max();
        decodeReserveBytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(totalEstimate, std::numeric_limits<std::size_t>::max()));
      }

      if (decodeReserveBytes > lease.bytes) {
        // Release the compressed-input lease before acquiring the combined decode
        // reservation to avoid hold-and-wait deadlocks across workers.
        lease.release();
        if (!acquireMemoryBudget(key, decodeReserveBytes)) {
          (void)tryAbortIfCanceled(key);
          continue;
        }
        lease.service = this;
        lease.bytes = decodeReserveBytes;
      }

      {
        std::function<void(const std::string&, std::size_t)> observer;
        std::size_t usedNow = 0;
        {
          std::scoped_lock lock(g_testHookMutex);
          observer = g_decodeObserverHook;
        }
        if (observer) {
          {
            std::scoped_lock lock(m_memoryMutex);
            usedNow = m_usedMemoryBytes;
          }
          observer(path, usedNow);
        }
      }

      if (tryAbortIfCanceled(key)) {
        continue;
      }

      auto decoded = decodeRasterImage(bytes.data(), bytes.size(), decodeLimits);
      bytes.clear();
      bytes.shrink_to_fit();
      if (!decoded) {
        lease.release();
        if (tryAbortIfCanceled(key)) {
          continue;
        }
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }

      w = decoded->width;
      h = decoded->height;
      pixels = std::move(decoded->pixels);

      if (!resizeThumbnail(pixels, w, h, key.targetPx)) {
        lease.release();
        if (tryAbortIfCanceled(key)) {
          continue;
        }
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }

      lease.release();
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    if (OwnedFd cacheDir = key.diskCache ? openPrivateCacheDir(true) : OwnedFd{}; cacheDir) {
      const std::string leaf = cacheLeafForStat(path, openedStat, key.targetPx);
      std::uint8_t* encoded = nullptr;
      const std::size_t encodedSize = WebPEncodeRGBA(pixels.data(), w, h, w * 4, kThumbnailWebPQuality, &encoded);
      if (encoded != nullptr && encodedSize > 0) {
        (void)writeCacheEntryAtomic(cacheDir.get(), leaf, encoded, encodedSize);
        WebPFree(encoded);
      }
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    result.rgba = std::move(pixels);
    result.width = w;
    result.height = h;
    pushResult(std::move(result));
  }
}

void setThumbnailCacheWriteHookForTesting(std::function<bool(const std::string&, const std::string&)> hook);
void setThumbnailDecodeObserverHookForTesting(std::function<void(const std::string&, std::size_t)> hook);

void setThumbnailCacheWriteHookForTesting(std::function<bool(const std::string&, const std::string&)> hook) {
  std::scoped_lock lock(g_testHookMutex);
  g_cacheWriteTestHook = std::move(hook);
}

void setThumbnailDecodeObserverHookForTesting(std::function<void(const std::string&, std::size_t)> hook) {
  std::scoped_lock lock(g_testHookMutex);
  g_decodeObserverHook = std::move(hook);
}
