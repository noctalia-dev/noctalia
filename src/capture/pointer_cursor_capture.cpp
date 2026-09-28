#include "capture/pointer_cursor_capture.h"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
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

  std::uint8_t straightChannel(std::uint32_t value, std::uint32_t alpha) {
    return alpha == 0 ? 0 : static_cast<std::uint8_t>(std::min(255U, (value * 255U + alpha / 2U) / alpha));
  }
} // namespace

struct PointerCursorCapturePending {
  PointerCursorCapture* owner = nullptr;
  ext_image_capture_source_v1* source = nullptr;
  ext_image_copy_capture_cursor_session_v1* cursorSession = nullptr;
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
  bool entered = false;
  bool hasPosition = false;
  bool hasHotspot = false;
  int x = 0;
  int y = 0;
  int hotspotX = 0;
  int hotspotY = 0;
  std::uint32_t transform = 0;
  // Submitted buffer constraints remain fixed if a new batch arrives in flight.
  std::uint32_t bufferFormat = 0;
  int bufferWidth = 0;
  int bufferHeight = 0;

  ~PointerCursorCapturePending() {
    if (frame != nullptr) {
      ext_image_copy_capture_frame_v1_destroy(frame);
    }
    if (session != nullptr) {
      ext_image_copy_capture_session_v1_destroy(session);
    }
    if (cursorSession != nullptr) {
      ext_image_copy_capture_cursor_session_v1_destroy(cursorSession);
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
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    pending.beginConstraints();
    pending.width = width;
    pending.height = height;
  }

  static void shmFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t format) {
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    pending.beginConstraints();
    const int preference = formatPreference(format);
    if (preference > 0 && (!pending.hasFormat || preference > formatPreference(pending.format))) {
      pending.format = format;
      pending.hasFormat = true;
    }
  }

  static void dmabufDevice(void* data, ext_image_copy_capture_session_v1*, wl_array*) {
    static_cast<PointerCursorCapturePending*>(data)->beginConstraints();
  }

  static void dmabufFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t, wl_array*) {
    static_cast<PointerCursorCapturePending*>(data)->beginConstraints();
  }

  static void constraintsDone(void* data, ext_image_copy_capture_session_v1*) {
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    pending.receivingConstraints = false;
    if (pending.frame == nullptr) {
      pending.captureFrame();
    }
  }

  static void stopped(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<PointerCursorCapturePending*>(data)->owner->fail("cursor capture session stopped");
  }

  static void enter(void* data, ext_image_copy_capture_cursor_session_v1*) {
    static_cast<PointerCursorCapturePending*>(data)->entered = true;
  }

  static void leave(void* data, ext_image_copy_capture_cursor_session_v1*) {
    static_cast<PointerCursorCapturePending*>(data)->owner->finish(capture::CapturedCursor{});
  }

  static void position(void* data, ext_image_copy_capture_cursor_session_v1*, std::int32_t x, std::int32_t y) {
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    pending.x = x;
    pending.y = y;
    pending.hasPosition = true;
  }

  static void hotspot(void* data, ext_image_copy_capture_cursor_session_v1*, std::int32_t x, std::int32_t y) {
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    pending.hotspotX = x;
    pending.hotspotY = y;
    pending.hasHotspot = true;
  }

  static void frameTransform(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t transform) {
    static_cast<PointerCursorCapturePending*>(data)->transform = transform;
  }

  static void damage(void*, ext_image_copy_capture_frame_v1*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
  static void presentationTime(void*, ext_image_copy_capture_frame_v1*, std::uint32_t, std::uint32_t, std::uint32_t) {}

  static void ready(void* data, ext_image_copy_capture_frame_v1*) {
    auto& pending = *static_cast<PointerCursorCapturePending*>(data);
    if (!pending.entered) {
      pending.owner->finish(capture::CapturedCursor{});
      return;
    }
    if (!pending.hasPosition || !pending.hasHotspot) {
      pending.owner->fail("cursor capture has no position or hotspot");
      return;
    }

    capture::CapturedCursor cursor;
    cursor.visible = true;
    cursor.transform = pending.transform;
    cursor.x = pending.x;
    cursor.y = pending.y;
    cursor.hotspotX = pending.hotspotX;
    cursor.hotspotY = pending.hotspotY;
    cursor.image.width = pending.bufferWidth;
    cursor.image.height = pending.bufferHeight;
    try {
      cursor.image.rgba.resize(pending.mappedSize);
    } catch (const std::bad_alloc&) {
      pending.owner->fail("failed to allocate cursor image");
      return;
    }

    const bool hasAlpha =
        pending.bufferFormat == WL_SHM_FORMAT_ARGB8888 || pending.bufferFormat == WL_SHM_FORMAT_ABGR8888;
    const bool blueFirst =
        pending.bufferFormat == WL_SHM_FORMAT_ARGB8888 || pending.bufferFormat == WL_SHM_FORMAT_XRGB8888;
    const auto* sourcePixels = static_cast<const std::uint8_t*>(pending.mapped);
    for (std::size_t offset = 0; offset < pending.mappedSize; offset += 4) {
      std::uint32_t pixel = 0;
      std::memcpy(&pixel, sourcePixels + offset, sizeof(pixel));
      const std::uint32_t alpha = hasAlpha ? pixel >> 24U : 255U;
      const std::uint32_t low = pixel & 255U;
      const std::uint32_t high = (pixel >> 16U) & 255U;
      auto* target = cursor.image.rgba.data() + offset;
      target[0] = straightChannel(blueFirst ? high : low, alpha);
      target[1] = straightChannel((pixel >> 8U) & 255U, alpha);
      target[2] = straightChannel(blueFirst ? low : high, alpha);
      target[3] = static_cast<std::uint8_t>(alpha);
    }
    pending.owner->finish(std::move(cursor));
  }

  static void failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t reason) {
    auto* owner = static_cast<PointerCursorCapturePending*>(data)->owner;
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
      owner->fail("cursor capture buffer constraints changed");
      return;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
      owner->fail("cursor capture session stopped");
      return;
    default:
      owner->fail("compositor failed to capture cursor image");
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
  inline static const ext_image_copy_capture_cursor_session_v1_listener cursorListener = {
      .enter = enter,
      .leave = leave,
      .position = position,
      .hotspot = hotspot,
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
      owner->fail("cursor image is unavailable (empty buffer constraints)");
      return;
    }
    if (!hasFormat) {
      owner->fail("cursor capture has no supported shared-memory format");
      return;
    }
    const auto maxPoolSize = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > maxPoolSize / 4U || height > maxPoolSize / (width * 4U)) {
      owner->fail("cursor capture buffer is too large");
      return;
    }
    bufferWidth = static_cast<int>(width);
    bufferHeight = static_cast<int>(height);
    bufferFormat = format;
    const int stride = bufferWidth * 4;
    mappedSize = static_cast<std::size_t>(stride) * height;
#ifdef __linux__
    fd = memfd_create("noctalia-cursor-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(mappedSize)) < 0) {
      owner->fail("failed to allocate cursor shared-memory file");
      return;
    }
    mapped = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
      owner->fail("failed to map cursor shared-memory buffer");
      return;
    }
    pool = wl_shm_create_pool(owner->m_wayland.shm(), fd, static_cast<std::int32_t>(mappedSize));
    if (pool == nullptr) {
      owner->fail("failed to create cursor shared-memory pool");
      return;
    }
    buffer = wl_shm_pool_create_buffer(pool, 0, bufferWidth, bufferHeight, stride, bufferFormat);
    if (buffer == nullptr) {
      owner->fail("failed to create cursor shared-memory buffer");
      return;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (frame == nullptr || ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, this) != 0) {
      owner->fail("failed to create cursor capture frame");
      return;
    }
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, bufferWidth, bufferHeight);
    ext_image_copy_capture_frame_v1_capture(frame);
    if (wl_display_flush(owner->m_wayland.display()) < 0 && errno != EAGAIN) {
      owner->fail("failed to submit cursor capture frame");
    }
  }
};

PointerCursorCapture::PointerCursorCapture(WaylandConnection& wayland) : m_wayland(wayland) {}

PointerCursorCapture::~PointerCursorCapture() { cancelInFlight(); }

bool PointerCursorCapture::available() const noexcept {
  return m_wayland.imageCopyCaptureManager() != nullptr
      && m_wayland.outputImageCaptureSourceManager() != nullptr
      && m_wayland.pointer() != nullptr
      && m_wayland.shm() != nullptr
      && m_wayland.display() != nullptr;
}

void PointerCursorCapture::capture(wl_output* output, CompletionCallback onComplete) {
  if (busy() || !available() || output == nullptr) {
    if (onComplete) {
      onComplete(std::nullopt, busy() ? "cursor capture already in progress" : "separate cursor capture unavailable");
    }
    return;
  }
  try {
    m_pending = std::make_unique<PointerCursorCapturePending>();
  } catch (const std::bad_alloc&) {
    if (onComplete) {
      onComplete(std::nullopt, "failed to allocate cursor capture session");
    }
    return;
  }
  m_onComplete = std::move(onComplete);
  m_pending->owner = this;
  m_pending->source =
      ext_output_image_capture_source_manager_v1_create_source(m_wayland.outputImageCaptureSourceManager(), output);
  if (m_pending->source == nullptr) {
    fail("failed to create cursor output source");
    return;
  }
  m_pending->cursorSession = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
      m_wayland.imageCopyCaptureManager(), m_pending->source, m_wayland.pointer()
  );
  if (m_pending->cursorSession == nullptr
      || ext_image_copy_capture_cursor_session_v1_add_listener(
             m_pending->cursorSession, &PointerCursorCapturePending::cursorListener, m_pending.get()
         ) != 0) {
    fail("failed to create pointer cursor capture session");
    return;
  }
  m_pending->session = ext_image_copy_capture_cursor_session_v1_get_capture_session(m_pending->cursorSession);
  if (m_pending->session == nullptr
      || ext_image_copy_capture_session_v1_add_listener(
             m_pending->session, &PointerCursorCapturePending::sessionListener, m_pending.get()
         ) != 0) {
    fail("failed to create cursor image capture session");
    return;
  }
  m_timeout.start(std::chrono::seconds(1), [this]() {
    fail("cursor capture timed out: compositor did not provide a capturable cursor image");
  });
  if (wl_display_flush(m_wayland.display()) < 0 && errno != EAGAIN) {
    fail("failed to submit cursor capture session");
  }
}

void PointerCursorCapture::cancelInFlight() {
  m_timeout.stop();
  m_pending.reset();
  m_onComplete = {};
}

void PointerCursorCapture::fail(std::string message) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::nullopt, std::move(message));
  }
}

void PointerCursorCapture::finish(capture::CapturedCursor cursor) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::move(cursor), {});
  }
}
