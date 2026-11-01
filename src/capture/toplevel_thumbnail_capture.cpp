#include "capture/toplevel_thumbnail_capture.h"

#include "capture/screencopy_util.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "render/core/image_resize.h"
#include "wayland/wayland_connection.h"

#include <cerrno>
#include <chrono>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace {
  int formatPreference(std::uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888:
      return 4;
    case WL_SHM_FORMAT_ABGR8888:
      return 3;
    case WL_SHM_FORMAT_XRGB8888:
      return 2;
    case WL_SHM_FORMAT_XBGR8888:
      return 1;
    default:
      return 0;
    }
  }

  // wl_shm names formats by little-endian word order, so ARGB8888 is B, G, R, A in memory.
  [[nodiscard]] ResizeSourceLayout resizeLayoutFor(std::uint32_t format) noexcept {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888:
      return ResizeSourceLayout::BgraPremultiplied;
    case WL_SHM_FORMAT_XRGB8888:
      return ResizeSourceLayout::Bgrx;
    case WL_SHM_FORMAT_ABGR8888:
      return ResizeSourceLayout::RgbaPremultiplied;
    default:
      return ResizeSourceLayout::Rgbx;
    }
  }

  [[nodiscard]] bool transformSwapsAxes(std::int32_t transform) noexcept {
    return transform == WL_OUTPUT_TRANSFORM_90
        || transform == WL_OUTPUT_TRANSFORM_270
        || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
        || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
  }

  // Zero bounds request native size.
  [[nodiscard]] int effectiveCaptureBound(int bound) noexcept {
    return bound > 0 ? bound : std::numeric_limits<int>::max();
  }
} // namespace

std::optional<ScreencopyImage> capture::makeToplevelThumbnail(
    std::span<const std::uint8_t> pixels, int width, int height, std::uint32_t format, int maxWidth, int maxHeight,
    std::int32_t transform
) {
  if (width <= 0 || height <= 0 || maxWidth <= 0 || maxHeight <= 0 || formatPreference(format) == 0) {
    return std::nullopt;
  }
  const auto widthSize = static_cast<std::size_t>(width);
  const auto heightSize = static_cast<std::size_t>(height);
  if (widthSize > std::numeric_limits<std::size_t>::max() / 4U
      || heightSize > std::numeric_limits<std::size_t>::max() / (widthSize * 4U)
      || pixels.size() < widthSize * heightSize * 4U) {
    return std::nullopt;
  }

  const bool swapsAxes = transformSwapsAxes(transform);
  const ImageExtent oriented =
      fitImageWithin(swapsAxes ? height : width, swapsAxes ? width : height, maxWidth, maxHeight);
  ScreencopyImage image;
  image.width = swapsAxes ? oriented.height : oriented.width;
  image.height = swapsAxes ? oriented.width : oriented.height;
  image.rgba = resizeToRgba(pixels.data(), width, height, resizeLayoutFor(format), image.width, image.height);
  if (image.rgba.empty()) {
    return std::nullopt;
  }
  screencopy::orientCaptureForTransform(image, transform);
  return image;
}

struct ToplevelThumbnailCapturePending {
  ToplevelThumbnailCapture* owner = nullptr;
  ext_image_capture_source_v1* source = nullptr;
  ext_image_copy_capture_session_v1* session = nullptr;
  ext_image_copy_capture_frame_v1* frame = nullptr;
  wl_shm_pool* pool = nullptr;
  wl_buffer* buffer = nullptr;
  void* mapped = MAP_FAILED;
  std::size_t mappedSize = 0;
  int fd = -1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  bool hasFormat = false;
  bool receivingConstraints = false;
  std::uint32_t bufferFormat = 0;
  int bufferWidth = 0;
  int bufferHeight = 0;
  int maxWidth = 1;
  int maxHeight = 1;
  std::int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;

  ~ToplevelThumbnailCapturePending() {
    if (frame != nullptr) {
      ext_image_copy_capture_frame_v1_destroy(frame);
    }
    if (session != nullptr) {
      ext_image_copy_capture_session_v1_destroy(session);
    }
    if (source != nullptr) {
      ext_image_capture_source_v1_destroy(source);
    }
    if (buffer != nullptr) {
      wl_buffer_destroy(buffer);
    }
    if (pool != nullptr) {
      wl_shm_pool_destroy(pool);
    }
    if (mapped != MAP_FAILED) {
      munmap(mapped, mappedSize);
    }
    if (fd >= 0) {
      close(fd);
    }
  }

  void beginConstraints() {
    if (!receivingConstraints) {
      receivingConstraints = true;
      width = 0;
      height = 0;
      hasFormat = false;
    }
  }

  static void bufferSize(void* data, ext_image_copy_capture_session_v1*, std::uint32_t width, std::uint32_t height) {
    auto& pending = *static_cast<ToplevelThumbnailCapturePending*>(data);
    pending.beginConstraints();
    pending.width = width;
    pending.height = height;
  }

  static void shmFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t format) {
    auto& pending = *static_cast<ToplevelThumbnailCapturePending*>(data);
    pending.beginConstraints();
    const int preference = formatPreference(format);
    if (preference > 0 && (!pending.hasFormat || preference > formatPreference(pending.format))) {
      pending.format = format;
      pending.hasFormat = true;
    }
  }

  static void dmabufDevice(void* data, ext_image_copy_capture_session_v1*, wl_array*) {
    static_cast<ToplevelThumbnailCapturePending*>(data)->beginConstraints();
  }

  static void dmabufFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t, wl_array*) {
    static_cast<ToplevelThumbnailCapturePending*>(data)->beginConstraints();
  }

  static void constraintsDone(void* data, ext_image_copy_capture_session_v1*) {
    auto& pending = *static_cast<ToplevelThumbnailCapturePending*>(data);
    pending.receivingConstraints = false;
    if (pending.frame == nullptr) {
      pending.captureFrame();
    }
  }

  static void stopped(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<ToplevelThumbnailCapturePending*>(data)->owner->fail("toplevel capture session stopped");
  }

  static void frameTransform(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t transform) {
    static_cast<ToplevelThumbnailCapturePending*>(data)->transform = static_cast<std::int32_t>(transform);
  }

  static void damage(void*, ext_image_copy_capture_frame_v1*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
  static void presentationTime(void*, ext_image_copy_capture_frame_v1*, std::uint32_t, std::uint32_t, std::uint32_t) {}

  static void ready(void* data, ext_image_copy_capture_frame_v1*) {
    auto& pending = *static_cast<ToplevelThumbnailCapturePending*>(data);
    try {
      auto thumbnail = capture::makeToplevelThumbnail(
          std::span(static_cast<const std::uint8_t*>(pending.mapped), pending.mappedSize), pending.bufferWidth,
          pending.bufferHeight, pending.bufferFormat, effectiveCaptureBound(pending.maxWidth),
          effectiveCaptureBound(pending.maxHeight), pending.transform
      );
      if (!thumbnail.has_value()) {
        pending.owner->fail("failed to decode toplevel thumbnail");
        return;
      }
      pending.owner->finish(std::move(*thumbnail));
    } catch (const std::bad_alloc&) {
      pending.owner->fail("failed to allocate toplevel thumbnail");
    }
  }

  static void failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t reason) {
    auto* owner = static_cast<ToplevelThumbnailCapturePending*>(data)->owner;
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
      owner->fail("toplevel capture buffer constraints changed");
      return;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
      owner->fail("toplevel capture session stopped");
      return;
    default:
      owner->fail("compositor failed to capture toplevel image");
      return;
    }
  }

  inline static const ext_image_copy_capture_session_v1_listener sessionListener = {
      .buffer_size = bufferSize,
      .shm_format = shmFormat,
      .dmabuf_device = dmabufDevice,
      .dmabuf_format = dmabufFormat,
      .done = constraintsDone,
      .stopped = stopped,
  };
  inline static const ext_image_copy_capture_frame_v1_listener frameListener = {
      .transform = frameTransform,
      .damage = damage,
      .presentation_time = presentationTime,
      .ready = ready,
      .failed = failed,
  };

  void captureFrame() {
    if (width == 0 || height == 0) {
      owner->fail("toplevel image is unavailable (empty buffer constraints)");
      return;
    }
    if (!hasFormat) {
      owner->fail("toplevel capture has no supported shared-memory format");
      return;
    }
    const auto maxPoolSize = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > maxPoolSize / 4U || height > maxPoolSize / (width * 4U)) {
      owner->fail("toplevel capture buffer is too large");
      return;
    }
    bufferWidth = static_cast<int>(width);
    bufferHeight = static_cast<int>(height);
    bufferFormat = format;
    const int stride = bufferWidth * 4;
    mappedSize = static_cast<std::size_t>(stride) * height;
#ifdef __linux__
    fd = memfd_create("noctalia-toplevel-thumbnail", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(mappedSize)) < 0) {
      owner->fail("failed to allocate toplevel capture shared-memory file");
      return;
    }
    mapped = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
      owner->fail("failed to map toplevel capture shared-memory buffer");
      return;
    }
    pool = wl_shm_create_pool(owner->m_wayland.shm(), fd, static_cast<std::int32_t>(mappedSize));
    if (pool == nullptr) {
      owner->fail("failed to create toplevel capture shared-memory pool");
      return;
    }
    buffer = wl_shm_pool_create_buffer(pool, 0, bufferWidth, bufferHeight, stride, bufferFormat);
    if (buffer == nullptr) {
      owner->fail("failed to create toplevel capture shared-memory buffer");
      return;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (frame == nullptr || ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, this) != 0) {
      owner->fail("failed to create toplevel capture frame");
      return;
    }
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, bufferWidth, bufferHeight);
    ext_image_copy_capture_frame_v1_capture(frame);
    if (wl_display_flush(owner->m_wayland.display()) < 0 && errno != EAGAIN) {
      owner->fail("failed to submit toplevel capture frame");
    }
  }
};

ToplevelThumbnailCapture::ToplevelThumbnailCapture(WaylandConnection& wayland) : m_wayland(wayland) {}

ToplevelThumbnailCapture::~ToplevelThumbnailCapture() { cancelInFlight(); }

bool ToplevelThumbnailCapture::available() const noexcept {
  return m_wayland.imageCopyCaptureManager() != nullptr
      && m_wayland.foreignToplevelImageCaptureSourceManager() != nullptr
      && m_wayland.shm() != nullptr
      && m_wayland.display() != nullptr;
}

void ToplevelThumbnailCapture::capture(
    ext_foreign_toplevel_handle_v1* handle, int maxWidth, int maxHeight, CompletionCallback onComplete
) {
  // Zero bounds on both axes request native size.
  const bool nativeSize = maxWidth == 0 && maxHeight == 0;
  if (busy() || !available() || handle == nullptr || (!nativeSize && (maxWidth <= 0 || maxHeight <= 0))) {
    if (onComplete) {
      onComplete(std::nullopt, busy() ? "toplevel capture already in progress" : "toplevel capture unavailable");
    }
    return;
  }
  try {
    m_pending = std::make_unique<ToplevelThumbnailCapturePending>();
  } catch (const std::bad_alloc&) {
    if (onComplete) {
      onComplete(std::nullopt, "failed to allocate toplevel capture session");
    }
    return;
  }
  m_onComplete = std::move(onComplete);
  m_pending->owner = this;
  m_pending->maxWidth = maxWidth;
  m_pending->maxHeight = maxHeight;
  m_pending->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
      m_wayland.foreignToplevelImageCaptureSourceManager(), handle
  );
  if (m_pending->source == nullptr) {
    fail("failed to create foreign-toplevel capture source");
    return;
  }
  m_pending->session =
      ext_image_copy_capture_manager_v1_create_session(m_wayland.imageCopyCaptureManager(), m_pending->source, 0);
  if (m_pending->session == nullptr
      || ext_image_copy_capture_session_v1_add_listener(
             m_pending->session, &ToplevelThumbnailCapturePending::sessionListener, m_pending.get()
         ) != 0) {
    fail("failed to create toplevel image capture session");
    return;
  }
  m_timeout.start(std::chrono::seconds(1), [this] {
    fail("toplevel capture timed out: compositor did not provide a capturable image");
  });
  if (wl_display_flush(m_wayland.display()) < 0 && errno != EAGAIN) {
    fail("failed to submit toplevel capture session");
  }
}

void ToplevelThumbnailCapture::cancelInFlight() {
  m_timeout.stop();
  m_pending.reset();
  m_onComplete = {};
}

void ToplevelThumbnailCapture::fail(std::string message) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::nullopt, std::move(message));
  }
}

void ToplevelThumbnailCapture::finish(ScreencopyImage image) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::move(image), {});
  }
}
