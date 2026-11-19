#include "shell/wayland/file_drag.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("file-drag");
  constexpr std::uint32_t kDataDeviceManagerVersion = 3;
  constexpr std::string_view kUriListMime = "text/uri-list";
  // Drag icon edge in logical px, rendered at 2x.
  constexpr int kIconSize = 48;
  constexpr int kIconScale = 2;

  std::string fileUri(std::string_view path) {
    std::string uri = "file://";
    for (const char c : path) {
      const auto byte = static_cast<unsigned char>(c);
      const bool unreserved = (std::isalnum(byte) != 0) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
      if (unreserved) {
        uri.push_back(c);
      } else {
        uri += std::format("%{:02X}", byte);
      }
    }
    return uri;
  }

  void handleTarget(void* /*data*/, wl_data_source* /*source*/, const char* /*mimeType*/) {}

  void handleSend(void* data, wl_data_source* source, const char* mimeType, std::int32_t fd) {
    static_cast<FileDrag*>(data)->onSend(source, mimeType, fd);
  }

  void handleCancelled(void* data, wl_data_source* source) { static_cast<FileDrag*>(data)->onFinished(source); }

  void handleDropPerformed(void* data, wl_data_source* source) {
    static_cast<FileDrag*>(data)->onDropPerformed(source);
  }

  void handleFinished(void* data, wl_data_source* source) { static_cast<FileDrag*>(data)->onFinished(source); }

  void handleAction(void* /*data*/, wl_data_source* /*source*/, std::uint32_t /*action*/) {}

  const wl_data_source_listener kSourceListener = {
      .target = &handleTarget,
      .send = &handleSend,
      .cancelled = &handleCancelled,
      .dnd_drop_performed = &handleDropPerformed,
      .dnd_finished = &handleFinished,
      .action = &handleAction,
  };

} // namespace

FileDrag::FileDrag(WaylandConnection& connection) : m_connection(connection) {}

FileDrag::~FileDrag() = default;

void FileDrag::onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) {
  if (interface == wl_data_device_manager_interface.name && m_manager == nullptr) {
    m_manager = static_cast<wl_data_device_manager*>(wl_registry_bind(
        registry, name, &wl_data_device_manager_interface, std::min(version, kDataDeviceManagerVersion)
    ));
  }
}

void FileDrag::releaseGlobals() {
  if (m_source != nullptr) {
    wl_data_source_destroy(m_source);
    m_source = nullptr;
  }
  m_onEnd = nullptr;
  endDrag();
  if (m_device != nullptr) {
    if (wl_data_device_get_version(m_device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(m_device);
    } else {
      wl_data_device_destroy(m_device);
    }
    m_device = nullptr;
  }
  if (m_manager != nullptr) {
    wl_data_device_manager_destroy(m_manager);
    m_manager = nullptr;
  }
}

bool FileDrag::start(
    wl_surface* origin, std::uint32_t serial, const std::vector<std::string>& paths, const std::string& iconPath,
    std::function<void()> onEnd
) {
  if (m_manager == nullptr || origin == nullptr || paths.empty()) {
    return false;
  }
  if (m_device == nullptr) {
    wl_seat* seat = m_connection.seat();
    if (seat == nullptr) {
      return false;
    }
    m_device = wl_data_device_manager_get_data_device(m_manager, seat);
  }
  if (m_source != nullptr) {
    wl_data_source_destroy(m_source);
    m_source = nullptr;
  }
  endDrag();
  m_onEnd = std::move(onEnd);

  m_uriList.clear();
  for (const auto& path : paths) {
    m_uriList += fileUri(path);
    m_uriList += "\r\n";
  }

  m_source = wl_data_device_manager_create_data_source(m_manager);
  wl_data_source_add_listener(m_source, &kSourceListener, this);
  wl_data_source_offer(m_source, kUriListMime.data());
  if (wl_data_source_get_version(m_source) >= WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION) {
    wl_data_source_set_actions(
        m_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
    );
  }
  if (m_connection.compositor() != nullptr) {
    m_icon = wl_compositor_create_surface(m_connection.compositor());
  }
  wl_data_device_start_drag(m_device, m_source, origin, m_icon, serial);
  attachIcon(iconPath);
  return true;
}

void FileDrag::attachIcon(const std::string& iconPath) {
  if (m_icon == nullptr || m_connection.shm() == nullptr || iconPath.empty()) {
    return;
  }
  const auto loaded = loadImageFile(iconPath, kIconSize * kIconScale, false);
  if (!loaded || loaded->width <= 0 || loaded->height <= 0) {
    return;
  }
  const int width = loaded->width;
  const int height = loaded->height;
  const int stride = width * 4;
  const auto size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
  const int fd = memfd_create("noctalia-drag-icon", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
    if (fd >= 0) {
      ::close(fd);
    }
    return;
  }
  void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return;
  }
  // RGBA straight alpha to ARGB8888 (premultiplied, little-endian BGRA).
  auto* out = static_cast<std::uint8_t*>(mapped);
  for (std::size_t i = 0; i + 3 < loaded->rgba.size() && i + 3 < size; i += 4) {
    const std::uint32_t a = loaded->rgba[i + 3];
    out[i + 0] = static_cast<std::uint8_t>(loaded->rgba[i + 2] * a / 255);
    out[i + 1] = static_cast<std::uint8_t>(loaded->rgba[i + 1] * a / 255);
    out[i + 2] = static_cast<std::uint8_t>(loaded->rgba[i + 0] * a / 255);
    out[i + 3] = static_cast<std::uint8_t>(a);
  }
  munmap(mapped, size);
  wl_shm_pool* pool = wl_shm_create_pool(m_connection.shm(), fd, static_cast<std::int32_t>(size));
  m_iconBuffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  ::close(fd);

  // Center the icon under the pointer.
  const int offsetX = -width / (2 * kIconScale);
  const int offsetY = -height / (2 * kIconScale);
  if (wl_surface_get_version(m_icon) >= WL_SURFACE_OFFSET_SINCE_VERSION) {
    wl_surface_attach(m_icon, m_iconBuffer, 0, 0);
    wl_surface_offset(m_icon, offsetX, offsetY);
  } else {
    wl_surface_attach(m_icon, m_iconBuffer, offsetX, offsetY);
  }
  if (wl_surface_get_version(m_icon) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
    wl_surface_set_buffer_scale(m_icon, kIconScale);
  }
  wl_surface_damage(m_icon, 0, 0, width, height);
  wl_surface_commit(m_icon);
}

void FileDrag::endDrag() {
  if (m_icon != nullptr) {
    wl_surface_destroy(m_icon);
    m_icon = nullptr;
  }
  if (m_iconBuffer != nullptr) {
    wl_buffer_destroy(m_iconBuffer);
    m_iconBuffer = nullptr;
  }
  if (auto onEnd = std::exchange(m_onEnd, nullptr)) {
    onEnd();
  }
}

void FileDrag::onSend(wl_data_source* source, const char* mimeType, int fd) {
  if (source == m_source && mimeType != nullptr && kUriListMime == mimeType) {
    std::string_view remaining = m_uriList;
    while (!remaining.empty()) {
      const ssize_t written = ::write(fd, remaining.data(), remaining.size());
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        kLog.warn("failed to send dragged files: {}", std::strerror(errno));
        break;
      }
      remaining.remove_prefix(static_cast<std::size_t>(written));
    }
  }
  ::close(fd);
}

void FileDrag::onDropPerformed(wl_data_source* source) {
  if (source == m_source) {
    endDrag();
  }
}

void FileDrag::onFinished(wl_data_source* source) {
  if (source == m_source) {
    m_source = nullptr;
    endDrag();
  }
  wl_data_source_destroy(source);
}
