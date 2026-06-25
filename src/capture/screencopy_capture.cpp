#include "capture/screencopy_capture.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
  constexpr Logger kLog("screencopy");
}

struct ScreencopyCapturePending {
  ScreencopyCapture* owner = nullptr;
  zwlr_screencopy_frame_v1* frame = nullptr;
  wl_shm_pool* pool = nullptr;
  wl_buffer* buffer = nullptr;
  void* mapped = MAP_FAILED;
  std::size_t mappedSize = 0;
  int fd = -1;
  std::uint32_t shmFormat = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool bufferDone = false;
  bool yInvert = false;
};

namespace {

  void destroyShmBuffer(wl_shm_pool*& pool, wl_buffer*& buffer, void*& data, std::size_t& size, int& fd) {
    if (buffer != nullptr) {
      wl_buffer_destroy(buffer);
      buffer = nullptr;
    }
    if (pool != nullptr) {
      wl_shm_pool_destroy(pool);
      pool = nullptr;
    }
    if (data != MAP_FAILED) {
      munmap(data, size);
      data = MAP_FAILED;
      size = 0;
    }
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] int createAnonymousFile(std::size_t size) {
#ifdef __linux__
    const int fd = memfd_create("noctalia-screencopy", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    const int fd = -1;
#endif
    if (fd < 0) {
      return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  [[nodiscard]] int bytesPerPixelFromStride(int width, int stride) {
    if (width <= 0 || stride <= 0) {
      return 0;
    }
    if (stride >= width * 4) {
      return 4;
    }
    if (stride >= width * 3) {
      return 3;
    }
    return 0;
  }

  [[nodiscard]] bool isPacked2101010(std::uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_XRGB2101010:
    case WL_SHM_FORMAT_ARGB2101010:
    case WL_SHM_FORMAT_XBGR2101010:
    case WL_SHM_FORMAT_ABGR2101010:
    case WL_SHM_FORMAT_RGBX1010102:
    case WL_SHM_FORMAT_RGBA1010102:
    case WL_SHM_FORMAT_BGRX1010102:
    case WL_SHM_FORMAT_BGRA1010102:
      return true;
    default:
      return false;
    }
  }

  // Decode a single little-endian 32-bit 10:10:10:2 / 2:10:10:10 packed pixel into 8-bit RGBA.
  void decodePacked2101010(const std::uint8_t* px, std::uint32_t format, std::uint8_t* dst) {
    const std::uint32_t v = static_cast<std::uint32_t>(px[0])
        | (static_cast<std::uint32_t>(px[1]) << 8)
        | (static_cast<std::uint32_t>(px[2]) << 16)
        | (static_cast<std::uint32_t>(px[3]) << 24);

    std::uint32_t r10 = 0;
    std::uint32_t g10 = 0;
    std::uint32_t b10 = 0;
    std::uint32_t a2 = 3;
    bool hasAlpha = false;

    switch (format) {
    case WL_SHM_FORMAT_ARGB2101010:
      hasAlpha = true;
      [[fallthrough]];
    case WL_SHM_FORMAT_XRGB2101010:
      a2 = (v >> 30) & 0x3U;
      r10 = (v >> 20) & 0x3FFU;
      g10 = (v >> 10) & 0x3FFU;
      b10 = v & 0x3FFU;
      break;
    case WL_SHM_FORMAT_ABGR2101010:
      hasAlpha = true;
      [[fallthrough]];
    case WL_SHM_FORMAT_XBGR2101010:
      a2 = (v >> 30) & 0x3U;
      b10 = (v >> 20) & 0x3FFU;
      g10 = (v >> 10) & 0x3FFU;
      r10 = v & 0x3FFU;
      break;
    case WL_SHM_FORMAT_RGBA1010102:
      hasAlpha = true;
      [[fallthrough]];
    case WL_SHM_FORMAT_RGBX1010102:
      r10 = (v >> 22) & 0x3FFU;
      g10 = (v >> 12) & 0x3FFU;
      b10 = (v >> 2) & 0x3FFU;
      a2 = v & 0x3U;
      break;
    case WL_SHM_FORMAT_BGRA1010102:
      hasAlpha = true;
      [[fallthrough]];
    case WL_SHM_FORMAT_BGRX1010102:
      b10 = (v >> 22) & 0x3FFU;
      g10 = (v >> 12) & 0x3FFU;
      r10 = (v >> 2) & 0x3FFU;
      a2 = v & 0x3U;
      break;
    default:
      break;
    }

    dst[0] = static_cast<std::uint8_t>(r10 >> 2);
    dst[1] = static_cast<std::uint8_t>(g10 >> 2);
    dst[2] = static_cast<std::uint8_t>(b10 >> 2);
    dst[3] = hasAlpha ? static_cast<std::uint8_t>(a2 * 0x55U) : 255;
  }

  [[nodiscard]] bool convertToRgba(
      const std::uint8_t* src, int width, int height, int stride, std::uint32_t format, bool yInvert,
      std::vector<std::uint8_t>& out
  ) {
    const int bytesPerPixel = bytesPerPixelFromStride(width, stride);
    if (bytesPerPixel == 0) {
      return false;
    }
    const bool packed = bytesPerPixel == 4 && isPacked2101010(format);

    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (int y = 0; y < height; ++y) {
      const int srcY = yInvert ? (height - 1 - y) : y;
      const auto* row = src + static_cast<std::size_t>(srcY) * static_cast<std::size_t>(stride);
      auto* dst = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
      for (int x = 0; x < width; ++x) {
        if (bytesPerPixel == 3) {
          const auto* px = row + static_cast<std::size_t>(x) * 3U;
          // stride = width * 3: wire bytes are R, G, B even when the format enum says BGR888.
          dst[0] = px[0];
          dst[1] = px[1];
          dst[2] = px[2];
          dst[3] = 255;
        } else if (packed) {
          decodePacked2101010(row + static_cast<std::size_t>(x) * 4U, format, dst);
        } else {
          const auto* px = row + static_cast<std::size_t>(x) * 4U;
          if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888) {
            dst[0] = px[2];
            dst[1] = px[1];
            dst[2] = px[0];
            dst[3] = format == WL_SHM_FORMAT_XRGB8888 ? 255 : px[3];
          } else if (format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888) {
            dst[0] = px[0];
            dst[1] = px[1];
            dst[2] = px[2];
            dst[3] = format == WL_SHM_FORMAT_XBGR8888 ? 255 : px[3];
          } else {
            dst[0] = px[0];
            dst[1] = px[1];
            dst[2] = px[2];
            dst[3] = 255;
          }
        }
        dst += 4;
      }
    }
    return true;
  }

  const wl_buffer_listener kBufferListener = {
      .release = [](void* data, wl_buffer* /*buffer*/) {
        auto* pending = static_cast<ScreencopyCapturePending*>(data);
        if (pending == nullptr) {
          return;
        }
        destroyShmBuffer(pending->pool, pending->buffer, pending->mapped, pending->mappedSize, pending->fd);
      },
  };

  void tryIssueCopy(ScreencopyCapturePending* pending) {
    if (pending == nullptr || pending->owner == nullptr || pending->frame == nullptr) {
      return;
    }
    if (!pending->bufferDone || pending->width <= 0 || pending->height <= 0 || pending->stride <= 0) {
      return;
    }

    wl_shm* shm = pending->owner->wayland().shm();
    if (shm == nullptr) {
      pending->owner->fail("wl_shm unavailable");
      return;
    }

    const std::size_t bufferSize =
        static_cast<std::size_t>(pending->stride) * static_cast<std::size_t>(pending->height);
    pending->fd = createAnonymousFile(bufferSize);
    if (pending->fd < 0) {
      pending->owner->fail("failed to allocate SHM buffer");
      return;
    }
    pending->mappedSize = bufferSize;
    pending->mapped = mmap(nullptr, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, pending->fd, 0);
    if (pending->mapped == MAP_FAILED) {
      pending->owner->fail("failed to map SHM buffer");
      return;
    }

    pending->pool = wl_shm_create_pool(shm, pending->fd, static_cast<int32_t>(bufferSize));
    pending->buffer = wl_shm_pool_create_buffer(
        pending->pool, 0, pending->width, pending->height, pending->stride, pending->shmFormat
    );
    wl_buffer_add_listener(pending->buffer, &kBufferListener, pending);
    zwlr_screencopy_frame_v1_copy(pending->frame, pending->buffer);
    wl_display_flush(pending->owner->wayland().display());
  }

  const zwlr_screencopy_frame_v1_listener kFrameListener = {
      .buffer =
          [](void* data, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t format, std::uint32_t width,
             std::uint32_t height, std::uint32_t stride) {
            auto* pending = static_cast<ScreencopyCapturePending*>(data);
            if (pending == nullptr) {
              return;
            }
            pending->shmFormat = format;
            pending->width = static_cast<int>(width);
            pending->height = static_cast<int>(height);
            pending->stride = static_cast<int>(stride);
            // kLog.info(
            //     "frame buffer format=0x{:08x} {}x{} stride={} bpp={}", format, width, height, stride,
            //     bytesPerPixelFromStride(static_cast<int>(width), static_cast<int>(stride))
            // );
          },
      .flags =
          [](void* data, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t flags) {
            auto* pending = static_cast<ScreencopyCapturePending*>(data);
            if (pending == nullptr) {
              return;
            }
            pending->yInvert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
          },
      .ready =
          [](void* data, zwlr_screencopy_frame_v1* frame, std::uint32_t /*tvSecHi*/, std::uint32_t /*tvSecLo*/,
             std::uint32_t /*tvNsec*/) {
            auto* pending = static_cast<ScreencopyCapturePending*>(data);
            if (pending == nullptr || pending->owner == nullptr) {
              zwlr_screencopy_frame_v1_destroy(frame);
              return;
            }

            ScreencopyImage image;
            image.width = pending->width;
            image.height = pending->height;
            image.yInvert = pending->yInvert;
            if (!convertToRgba(
                    static_cast<const std::uint8_t*>(pending->mapped), pending->width, pending->height, pending->stride,
                    pending->shmFormat, pending->yInvert, image.rgba
                )) {
              zwlr_screencopy_frame_v1_destroy(frame);
              pending->frame = nullptr;
              destroyShmBuffer(pending->pool, pending->buffer, pending->mapped, pending->mappedSize, pending->fd);
              pending->owner->fail(
                  "unsupported screencopy buffer layout (format=0x"
                  + std::to_string(pending->shmFormat)
                  + " stride="
                  + std::to_string(pending->stride)
                  + " width="
                  + std::to_string(pending->width)
                  + ")"
              );
              return;
            }

            zwlr_screencopy_frame_v1_destroy(frame);
            pending->frame = nullptr;
            destroyShmBuffer(pending->pool, pending->buffer, pending->mapped, pending->mappedSize, pending->fd);
            pending->owner->finish(std::move(image));
          },
      .failed =
          [](void* data, zwlr_screencopy_frame_v1* frame) {
            auto* pending = static_cast<ScreencopyCapturePending*>(data);
            zwlr_screencopy_frame_v1_destroy(frame);
            if (pending != nullptr) {
              pending->frame = nullptr;
            }
            if (pending != nullptr && pending->owner != nullptr) {
              pending->owner->fail("screencopy frame failed");
            }
          },
      // copy_with_damage only; SHM path uses copy().
      .damage = [](void* /*data*/, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t /*x*/, std::uint32_t /*y*/,
                   std::uint32_t /*width*/, std::uint32_t /*height*/) {},
      // Advertised on v3; we capture via wl_shm only.
      .linux_dmabuf = [](void* /*data*/, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t /*format*/,
                         std::uint32_t /*width*/, std::uint32_t /*height*/) {},
      .buffer_done =
          [](void* data, zwlr_screencopy_frame_v1* /*frame*/) {
            auto* pending = static_cast<ScreencopyCapturePending*>(data);
            if (pending == nullptr) {
              return;
            }
            pending->bufferDone = true;
            tryIssueCopy(pending);
          },
  };

} // namespace

ScreencopyCapture::ScreencopyCapture(WaylandConnection& wayland) : m_wayland(wayland) {}

ScreencopyCapture::~ScreencopyCapture() { destroyPending(); }

bool ScreencopyCapture::available() const noexcept { return m_wayland.hasScreencopy() && m_wayland.shm() != nullptr; }

void ScreencopyCapture::capture(
    wl_output* output, std::optional<LogicalRect> region, bool overlayCursor, CompletionCallback onComplete
) {
  if (m_busy) {
    if (onComplete) {
      onComplete(std::nullopt, "capture already in progress");
    }
    return;
  }
  if (!available() || output == nullptr) {
    if (onComplete) {
      onComplete(std::nullopt, "screencopy unavailable");
    }
    return;
  }

  destroyPending();

  auto* manager = m_wayland.screencopyManager();
  if (manager == nullptr) {
    if (onComplete) {
      onComplete(std::nullopt, "screencopy manager missing");
    }
    return;
  }

  m_busy = true;
  m_onComplete = std::move(onComplete);
  m_pending = std::make_unique<ScreencopyCapturePending>();
  m_pending->owner = this;

  if (region.has_value() && region->width > 0 && region->height > 0) {
    m_pending->frame = zwlr_screencopy_manager_v1_capture_output_region(
        manager, overlayCursor ? 1 : 0, output, region->x, region->y, region->width, region->height
    );
  } else {
    m_pending->frame = zwlr_screencopy_manager_v1_capture_output(manager, overlayCursor ? 1 : 0, output);
  }

  zwlr_screencopy_frame_v1_add_listener(m_pending->frame, &kFrameListener, m_pending.get());
  wl_display_flush(wayland().display());
}

void ScreencopyCapture::cancelInFlight() {
  destroyPending();
  m_busy = false;
  m_onComplete = {};
}

void ScreencopyCapture::fail(std::string message) {
  destroyPending();
  m_busy = false;
  if (m_onComplete) {
    m_onComplete(std::nullopt, std::move(message));
    m_onComplete = {};
  }
}

void ScreencopyCapture::finish(ScreencopyImage image) {
  destroyPending();
  m_busy = false;
  if (m_onComplete) {
    m_onComplete(std::move(image), {});
    m_onComplete = {};
  }
}

void ScreencopyCapture::destroyPending() {
  if (m_pending == nullptr) {
    return;
  }
  if (m_pending->frame != nullptr) {
    zwlr_screencopy_frame_v1_destroy(m_pending->frame);
    m_pending->frame = nullptr;
  }
  destroyShmBuffer(m_pending->pool, m_pending->buffer, m_pending->mapped, m_pending->mappedSize, m_pending->fd);
  m_pending.reset();
}
