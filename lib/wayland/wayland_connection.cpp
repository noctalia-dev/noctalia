#include "wayland/wayland_connection.h"

#include "core/log.h"
#include "core/process/process_fds.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wayland/text_input_service.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <poll.h>
#include <ranges>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr std::uint32_t kCompositorVersion = 4;
  constexpr std::uint32_t kSeatVersion = 8;
  constexpr std::uint32_t kShmVersion = 1;
  constexpr std::uint32_t kSubcompositorVersion = 1;
  constexpr std::uint32_t kXdgOutputManagerVersion = 3;
  constexpr std::uint32_t kXdgWmBaseVersion = 6;
  constexpr std::uint32_t kCursorShapeManagerVersion = 1;
  constexpr std::uint32_t kXdgActivationVersion = 1;
  constexpr std::uint32_t kFractionalScaleManagerVersion = 1;
  constexpr std::uint32_t kViewporterVersion = 1;
  constexpr std::uint32_t kOutputVersion = 4;
  constexpr std::uint32_t kTextInputManagerVersion = 2;

  const wl_registry_listener kRegistryListener = {
      .global = &WaylandConnection::handleGlobal,
      .global_remove = &WaylandConnection::handleGlobalRemove,
  };

  std::string errnoText(int value) {
    if (value == 0) {
      return "none";
    }
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : std::string("unknown");
  }

  struct DetectedOutputScale {
    double scale = 0.0;
    bool rotated = false;
    bool available = false;
  };

  DetectedOutputScale detectOutputScale(const WaylandOutput& output) {
    const wayland::DetectedScale detected =
        wayland::detectScaleFromDimensions(output.width, output.height, output.logicalWidth, output.logicalHeight);
    return {.scale = detected.scale, .rotated = detected.rotated, .available = detected.available};
  }

  std::string outputLabel(const WaylandOutput& output) {
    if (!output.connectorName.empty()) {
      return output.connectorName;
    }
    return std::format("#{}", output.name);
  }

  void outputGeometry(
      void* data, wl_output* wlOut, int32_t /*x*/, int32_t /*y*/, int32_t /*physW*/, int32_t /*physH*/,
      int32_t /*subpixel*/, const char* /*make*/, const char* /*model*/, int32_t transform
  ) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->transform = transform;
    }
  }

  void outputMode(void* data, wl_output* wlOut, uint32_t flags, int32_t w, int32_t h, int32_t /*refresh*/) {
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
      return;
    }
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->width = w;
      out->height = h;
      self->recomputeConfiguredScale(*out);
    }
  }

  void outputDone(void* data, wl_output* wlOut) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out == nullptr) {
      return;
    }
    // A hot-plugged output's first done precedes its xdg-output logical size.
    if (out->xdgOutput != nullptr && out->logicalWidth <= 0) {
      return;
    }
    out->done = true;
    self->notifyOutputReady(wlOut);
  }

  void outputScale(void* data, wl_output* wlOut, int32_t factor) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->scale = factor;
      self->recomputeConfiguredScale(*out);
    }
  }

  void outputName(void* data, wl_output* wlOut, const char* name) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out != nullptr && name != nullptr) {
      const std::string nextName = name;
      if (out->connectorName == nextName) {
        return;
      }
      out->connectorName = nextName;
      self->applyOutputMetadata();

      // The change callback can re-enter dispatch and reallocate m_outputs.
      out = self->findOutputByWl(wlOut);
      if (out != nullptr && out->done) {
        self->notifyOutputReady(wlOut);
      }
    }
  }

  void outputDescription(void* data, wl_output* wlOut, const char* desc) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->description = desc;
    }
  }

  const wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  void xdgOutputLogicalPosition(void* data, zxdg_output_v1* xdgOutput, int32_t x, int32_t y) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByXdg(xdgOutput);
    if (out != nullptr) {
      out->logicalX = x;
      out->logicalY = y;
    }
  }

  void xdgOutputLogicalSize(void* data, zxdg_output_v1* xdgOutput, int32_t w, int32_t h) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByXdg(xdgOutput);
    if (out != nullptr) {
      out->logicalWidth = w;
      out->logicalHeight = h;
      self->recomputeConfiguredScale(*out);
    }
  }

  void xdgOutputDone(void* data, zxdg_output_v1* xdgOutput) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByXdg(xdgOutput);
    if (out != nullptr && out->output != nullptr) {
      out->done = true;
      self->notifyOutputReady(out->output);
    }
  }

  void xdgOutputName(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*name*/) {}

  void xdgOutputDescription(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*desc*/) {}

  const zxdg_output_v1_listener kXdgOutputListener = {
      .logical_position = xdgOutputLogicalPosition,
      .logical_size = xdgOutputLogicalSize,
      .done = xdgOutputDone,
      .name = xdgOutputName,
      .description = xdgOutputDescription,
  };

  constexpr Logger kLog("wayland");

  void xdgWmBasePing(void* /*data*/, xdg_wm_base* wmBase, std::uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  const xdg_wm_base_listener kXdgWmBaseListener = {
      .ping = xdgWmBasePing,
  };

} // namespace

namespace wayland {

  DispatchResult dispatchUntil(
      std::span<const DispatchTarget> targets, std::chrono::steady_clock::time_point deadline,
      const std::function<bool()>& completed
  ) {
    std::vector<DispatchTarget> activeTargets;
    activeTargets.reserve(targets.size());
    for (const auto& target : targets) {
      if (target.connection != nullptr) {
        activeTargets.push_back(target);
      }
    }

    if (completed()) {
      return {};
    }
    if (activeTargets.empty()) {
      return {
          .status = DispatchStatus::PollFailed,
          .error = "no Wayland connections available for event dispatch",
      };
    }

    std::vector<bool> prepared(activeTargets.size(), false);
    std::vector<pollfd> pollFds(activeTargets.size());
    auto cancelPreparedReads = [&] {
      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        if (prepared[i]) {
          wl_display_cancel_read(activeTargets[i].connection->display());
          prepared[i] = false;
        }
      }
    };
    auto connectionFailure = [&activeTargets](std::size_t index, std::string_view operation, int operationErrno) {
      WaylandConnection* connection = activeTargets[index].connection;
      return DispatchResult{
          .status = DispatchStatus::ConnectionFailed,
          .failedConnection = connection,
          .error = std::format(
              "{} on {} Wayland connection: {}", operation, activeTargets[index].role,
              connection->describeDisplayError(operationErrno)
          ),
      };
    };

    while (!completed()) {
      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        wl_display* display = activeTargets[i].connection->display();
        if (wl_display_dispatch_pending(display) < 0) {
          return connectionFailure(i, "Wayland dispatch failed", errno);
        }
        if (completed()) {
          return {};
        }
      }

      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        wl_display* display = activeTargets[i].connection->display();
        while (wl_display_prepare_read(display) != 0) {
          if (wl_display_dispatch_pending(display) < 0) {
            const int dispatchErrno = errno;
            cancelPreparedReads();
            return connectionFailure(i, "Wayland dispatch failed", dispatchErrno);
          }
          if (completed()) {
            cancelPreparedReads();
            return {};
          }
        }
        prepared[i] = true;
      }

      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        int flushResult = 0;
        do {
          flushResult = wl_display_flush(activeTargets[i].connection->display());
        } while (flushResult < 0 && errno == EINTR);
        const int flushErrno = errno;
        if (flushResult < 0 && flushErrno != EAGAIN && flushErrno != EPIPE) {
          cancelPreparedReads();
          return connectionFailure(i, "Wayland flush failed", flushErrno);
        }
        pollFds[i] = pollfd{
            .fd = wl_display_get_fd(activeTargets[i].connection->display()),
            .events = static_cast<short>(POLLIN | (flushResult < 0 && flushErrno == EAGAIN ? POLLOUT : 0)),
            .revents = 0,
        };
      }

      int ready = 0;
      while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          ready = 0;
          break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ready = ::poll(
            pollFds.data(), static_cast<nfds_t>(pollFds.size()), std::max(1, static_cast<int>(remaining.count()))
        );
        if (ready >= 0 || errno != EINTR) {
          break;
        }
      }

      if (ready == 0) {
        cancelPreparedReads();
        return {
            .status = DispatchStatus::TimedOut,
            .error = "Wayland event wait timed out",
        };
      }
      if (ready < 0) {
        const int pollErrno = errno;
        cancelPreparedReads();
        return {
            .status = DispatchStatus::PollFailed,
            .error = std::format("Wayland poll failed: {}", errnoText(pollErrno)),
        };
      }

      std::optional<std::pair<std::size_t, int>> readFailure;
      std::optional<std::size_t> transportFailure;
      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        if (!prepared[i]) {
          continue;
        }
        if ((pollFds[i].revents & POLLIN) != 0) {
          if (wl_display_read_events(activeTargets[i].connection->display()) < 0 && !readFailure.has_value()) {
            readFailure = std::pair{i, errno};
          }
        } else {
          wl_display_cancel_read(activeTargets[i].connection->display());
        }
        prepared[i] = false;
        if ((pollFds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && !transportFailure.has_value()) {
          transportFailure = i;
        }
      }

      if (readFailure.has_value()) {
        return connectionFailure(readFailure->first, "Wayland event read failed", readFailure->second);
      }

      for (std::size_t i = 0; i < activeTargets.size(); ++i) {
        if (wl_display_dispatch_pending(activeTargets[i].connection->display()) < 0) {
          return connectionFailure(i, "Wayland dispatch failed", errno);
        }
      }

      if (transportFailure.has_value()) {
        const std::size_t i = *transportFailure;
        const int displayError = wl_display_get_error(activeTargets[i].connection->display());
        return connectionFailure(
            i, "Wayland connection failed during event wait", displayError != 0 ? displayError : EPIPE
        );
      }
    }

    return {};
  }

} // namespace wayland

namespace {

  struct InitialSyncState {
    wl_callback* callback = nullptr;
    bool done = false;
  };

  void initialSyncDone(void* data, wl_callback* callback, std::uint32_t /*callbackData*/) {
    auto* state = static_cast<InitialSyncState*>(data);
    state->callback = nullptr;
    state->done = true;
    wl_callback_destroy(callback);
  }

  const wl_callback_listener kInitialSyncListener = {
      .done = initialSyncDone,
  };

  [[nodiscard]] wl_display* connectSiblingDisplay(
      const WaylandConnection& primary, std::chrono::steady_clock::time_point deadline, std::string& error
  ) {
    if (primary.display() == nullptr) {
      error = "primary Wayland connection is unavailable";
      return nullptr;
    }

    sockaddr_un peer{};
    socklen_t peerLength = sizeof(peer);
    if (getpeername(wl_display_get_fd(primary.display()), reinterpret_cast<sockaddr*>(&peer), &peerLength) < 0) {
      error = std::format("failed to identify the primary Wayland endpoint: {}", errnoText(errno));
      return nullptr;
    }
    if (peer.sun_family != AF_UNIX || peerLength <= offsetof(sockaddr_un, sun_path)) {
      error = "primary Wayland connection has no reconnectable endpoint";
      return nullptr;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      error = std::format("failed to create a Wayland capture socket: {}", errnoText(errno));
      return nullptr;
    }

    bool connectionPending = false;
    while (true) {
      const int connectResult = ::connect(fd, reinterpret_cast<const sockaddr*>(&peer), peerLength);
      if (connectResult == 0 || (connectResult < 0 && errno == EISCONN)) {
        break;
      }

      const int connectErrno = errno;
      if (connectErrno == EINPROGRESS || connectErrno == EINTR || connectErrno == EALREADY) {
        connectionPending = true;
        break;
      }
      if (connectErrno != EAGAIN) {
        error = std::format("failed to connect the Wayland capture socket: {}", errnoText(connectErrno));
        close(fd);
        return nullptr;
      }

      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        error = "Wayland capture socket connection timed out";
        close(fd);
        return nullptr;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int retryDelay = std::min(10, std::max(1, static_cast<int>(remaining.count())));
      int retryResult = 0;
      do {
        retryResult = ::poll(nullptr, 0, retryDelay);
      } while (retryResult < 0 && errno == EINTR);
      if (retryResult < 0) {
        error = std::format("failed while retrying the Wayland capture socket: {}", errnoText(errno));
        close(fd);
        return nullptr;
      }
    }

    if (connectionPending) {
      pollfd pollFd{
          .fd = fd,
          .events = POLLOUT,
          .revents = 0,
      };
      while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          error = "Wayland capture socket connection timed out";
          close(fd);
          return nullptr;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int ready = ::poll(&pollFd, 1, std::max(1, static_cast<int>(remaining.count())));
        if (ready > 0) {
          break;
        }
        if (ready == 0) {
          error = "Wayland capture socket connection timed out";
          close(fd);
          return nullptr;
        }
        if (errno != EINTR) {
          error = std::format("failed while connecting the Wayland capture socket: {}", errnoText(errno));
          close(fd);
          return nullptr;
        }
      }

      int socketError = 0;
      socklen_t socketErrorLength = sizeof(socketError);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) < 0) {
        error = std::format("failed to inspect the Wayland capture socket: {}", errnoText(errno));
        close(fd);
        return nullptr;
      }
      if (socketError != 0) {
        error = std::format("failed to connect the Wayland capture socket: {}", errnoText(socketError));
        close(fd);
        return nullptr;
      }
    }

    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
      error = std::format("failed to initialize the Wayland capture connection: {}", errnoText(errno));
    }
    return display;
  }

  [[nodiscard]] bool waitForInitialSync(
      WaylandConnection& connection, WaylandConnection* eventConnection, std::chrono::steady_clock::time_point deadline,
      std::string_view phase, std::string& error
  ) {
    if (std::chrono::steady_clock::now() >= deadline) {
      error = std::format("{} timed out", phase);
      return false;
    }

    InitialSyncState state;
    state.callback = wl_display_sync(connection.display());
    if (state.callback == nullptr) {
      error = std::format("{} failed: {}", phase, connection.describeDisplayError(errno));
      return false;
    }
    if (wl_callback_add_listener(state.callback, &kInitialSyncListener, &state) != 0) {
      const int listenerErrno = errno;
      wl_callback_destroy(state.callback);
      state.callback = nullptr;
      error = std::format("{} listener setup failed: {}", phase, errnoText(listenerErrno));
      return false;
    }

    const std::array targets{
        wayland::DispatchTarget{
            .connection = eventConnection != &connection ? eventConnection : nullptr,
            .role = "primary",
        },
        wayland::DispatchTarget{
            .connection = &connection,
            .role = "capture",
        },
    };
    const wayland::DispatchResult result = wayland::dispatchUntil(targets, deadline, [&state] { return state.done; });
    if (state.callback != nullptr) {
      wl_callback_destroy(state.callback);
      state.callback = nullptr;
    }
    if (result.status != wayland::DispatchStatus::Completed) {
      error = std::format("{}: {}", phase, result.error);
      return false;
    }
    return true;
  }

} // namespace

WaylandConnection::WaylandConnection(Purpose purpose) : m_purpose(purpose) {}

WaylandConnection::~WaylandConnection() { cleanup(); }

bool WaylandConnection::setupDisplay(wl_display* display, std::string& error) {
  if (m_display != nullptr) {
    return true;
  }

  if (display == nullptr) {
    error = "failed to connect to Wayland display";
    return false;
  }
  m_display = display;

  m_registry = wl_display_get_registry(m_display);
  if (m_registry == nullptr) {
    error = "failed to acquire Wayland registry";
    cleanup();
    return false;
  }

  if (wl_registry_add_listener(m_registry, &kRegistryListener, this) != 0) {
    error = "failed to add Wayland registry listener";
    cleanup();
    return false;
  }
  return true;
}

bool WaylandConnection::connect() {
  if (m_display != nullptr) {
    return true;
  }
  if (m_purpose != Purpose::Primary) {
    throw std::runtime_error("sibling Wayland connections require a bounded primary connection");
  }

  std::string setupError;
  if (!setupDisplay(wl_display_connect(nullptr), setupError)) {
    throw std::runtime_error(setupError);
  }

  if (wl_display_roundtrip(m_display) < 0) {
    const int roundtripErrno = errno;
    const std::string detail = describeDisplayError(roundtripErrno);
    cleanup();
    throw std::runtime_error(std::format("failed during Wayland registry roundtrip: {}", detail));
  }

  if (wl_display_roundtrip(m_display) < 0) {
    const int roundtripErrno = errno;
    const std::string detail = describeDisplayError(roundtripErrno);
    cleanup();
    throw std::runtime_error(std::format("failed during Wayland output discovery roundtrip: {}", detail));
  }

  logStartupSummary();
  return true;
}

bool WaylandConnection::connectUntil(
    std::chrono::steady_clock::time_point deadline, WaylandConnection* eventConnection, std::string& error
) {
  error.clear();
  if (m_display != nullptr) {
    return true;
  }
  if (m_purpose != Purpose::Sibling) {
    error = "bounded connection is only available for sibling connections";
    return false;
  }
  if (eventConnection == nullptr || eventConnection->display() == nullptr) {
    error = "primary Wayland connection is unavailable";
    return false;
  }

  wl_display* siblingDisplay = connectSiblingDisplay(*eventConnection, deadline, error);
  if (siblingDisplay == nullptr || !setupDisplay(siblingDisplay, error)) {
    return false;
  }

  if (!waitForInitialSync(*this, eventConnection, deadline, "Wayland registry discovery", error)
      || !waitForInitialSync(*this, eventConnection, deadline, "Wayland output discovery", error)) {
    cleanup();
    return false;
  }

  return true;
}

void WaylandConnection::setOutputChangeCallback(ChangeCallback callback) {
  m_outputChangeCallback = std::move(callback);
}

void WaylandConnection::setOutputLifecycleCallbacks(
    std::function<void(wl_output*)> added, std::function<void(wl_output*)> removed
) {
  m_outputAddedCallback = std::move(added);
  m_outputRemovedCallback = std::move(removed);
}

void WaylandConnection::setPointerEventCallback(WaylandSeat::PointerEventCallback callback) {
  m_pointerEventCallback = std::move(callback);
  m_seatHandler.setPointerEventCallback([this](const PointerEvent& event) {
    const auto it = m_surfaceOutputMap.find(event.surface);
    if (it != m_surfaceOutputMap.end() && it->second != nullptr) {
      m_lastPointerOutput = it->second;
      m_lastPointerOutputAt = std::chrono::steady_clock::now();
    }
    if (m_pointerEventCallback) {
      m_pointerEventCallback(event);
    }
  });
}

void WaylandConnection::registerSurfaceOutput(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }
  m_surfaceOutputMap[surface] = output;
}

void WaylandConnection::notifySurfaceOutputEnter(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }
  auto& outputs = m_surfaceOutputs[surface];
  if (!std::ranges::contains(outputs, output)) {
    outputs.push_back(output);
  }
  m_surfaceOutputMap[surface] = output;
}

void WaylandConnection::notifySurfaceOutputLeave(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }

  auto it = m_surfaceOutputs.find(surface);
  if (it != m_surfaceOutputs.end()) {
    auto& outputs = it->second;
    std::erase(outputs, output);
    if (outputs.empty()) {
      m_surfaceOutputs.erase(it);
    } else if (
        auto current = m_surfaceOutputMap.find(surface);
        current != m_surfaceOutputMap.end() && current->second == output
    ) {
      current->second = outputs.back();
    }
  }

  const auto current = m_surfaceOutputMap.find(surface);
  if (current != m_surfaceOutputMap.end() && current->second == output) {
    m_surfaceOutputMap.erase(current);
  }
}

void WaylandConnection::unregisterSurface(wl_surface* surface) {
  if (surface != nullptr) {
    m_seatHandler.forgetSurface(surface);
    m_surfaceOutputMap.erase(surface);
    m_surfaceOutputs.erase(surface);
    if (m_lastPointerOutput != nullptr) {
      // Clear last pointer output only if it was from this surface
      // (we don't track which surface set it, so just leave it — it's a hint anyway)
    }
  }
}

void WaylandConnection::notifyOutputReady(wl_output* output) {
  if (output == nullptr || m_outputChangeCallback == nullptr) {
    return;
  }
  m_outputChangeCallback();
}

wl_output* WaylandConnection::lastPointerOutput() const noexcept { return m_lastPointerOutput; }
wl_surface* WaylandConnection::lastPointerSurface() const noexcept { return m_seatHandler.lastPointerSurface(); }
wl_surface* WaylandConnection::lastKeyboardSurface() const noexcept { return m_seatHandler.lastKeyboardSurface(); }
std::uint32_t WaylandConnection::keyboardModifiers() const noexcept { return m_seatHandler.keyboardModifiers(); }
bool WaylandConnection::hasPointerPosition() const noexcept { return m_seatHandler.hasPointerPosition(); }
double WaylandConnection::lastPointerX() const noexcept { return m_seatHandler.lastPointerX(); }
double WaylandConnection::lastPointerY() const noexcept { return m_seatHandler.lastPointerY(); }
WaylandSeat::InputSource WaylandConnection::lastInputSource() const noexcept { return m_seatHandler.lastInputSource(); }
std::string WaylandConnection::currentKeyboardLayoutName() const { return m_seatHandler.currentLayoutName(); }
std::vector<std::string> WaylandConnection::keyboardLayoutNames() const { return m_seatHandler.layoutNames(); }
WaylandSeat::LockKeysState WaylandConnection::keyboardLockKeysState() const { return m_seatHandler.lockKeysState(); }
std::uint32_t WaylandConnection::lastInputSerial() const noexcept { return m_seatHandler.lastSerial(); }

double WaylandConnection::userIdleSeconds() const noexcept { return m_seatHandler.userIdleSeconds(); }

bool WaylandConnection::hasFreshPointerOutput(std::chrono::milliseconds maxAge) const noexcept {
  if (m_lastPointerOutput == nullptr || m_lastPointerOutputAt.time_since_epoch().count() == 0) {
    return false;
  }
  return std::chrono::steady_clock::now() - m_lastPointerOutputAt <= maxAge;
}

wl_output* WaylandConnection::outputForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceOutputMap.find(surface);
  return it != m_surfaceOutputMap.end() ? it->second : nullptr;
}

void WaylandConnection::setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback) {
  m_seatHandler.setKeyboardEventCallback(std::move(callback));
}

void WaylandConnection::setKeyboardModifiersCallback(WaylandSeat::KeyboardModifiersCallback callback) {
  m_seatHandler.setKeyboardModifiersCallback(std::move(callback));
}

void WaylandConnection::setLockKeysChangeCallback(WaylandSeat::LockKeysChangeCallback callback) {
  m_seatHandler.setLockKeysChangeCallback(std::move(callback));
}

void WaylandConnection::setKeyboardLayoutChangeCallback(WaylandSeat::KeyboardLayoutChangeCallback callback) {
  m_seatHandler.setKeyboardLayoutChangeCallback(std::move(callback));
}

void WaylandConnection::setTextInputService(TextInputService* textInputService) {
  m_textInputService = textInputService;
  if (textInputService != nullptr) {
    m_seatHandler.setKeyboardFocusCallback([textInputService](wl_surface* surface, bool entered) {
      textInputService->onKeyboardFocusSurface(surface, entered);
    });
  } else {
    m_seatHandler.setKeyboardFocusCallback({});
  }
  bindTextInputService();
}

int WaylandConnection::repeatPollTimeoutMs() const { return m_seatHandler.repeatPollTimeoutMs(); }

void WaylandConnection::repeatTick() { m_seatHandler.repeatTick(); }

void WaylandConnection::stopKeyRepeat() { m_seatHandler.stopKeyRepeat(); }

void WaylandConnection::setCursorShape(std::uint32_t serial, std::uint32_t shape) {
  m_seatHandler.setCursorShape(serial, shape);
}

bool WaylandConnection::isConnected() const noexcept { return m_display != nullptr; }

bool WaylandConnection::hasSubcompositor() const noexcept { return m_subcompositor != nullptr; }

bool WaylandConnection::hasXdgOutputManager() const noexcept { return m_xdgOutputManager != nullptr; }
bool WaylandConnection::hasXdgShell() const noexcept { return m_xdgWmBase != nullptr; }

bool WaylandConnection::hasXdgActivation() const noexcept { return m_xdgActivation != nullptr; }
bool WaylandConnection::hasFractionalScale() const noexcept {
  return m_fractionalScaleManager != nullptr && m_viewporter != nullptr;
}

std::string WaylandConnection::requestActivationToken(wl_surface* surface) const {
  if (m_xdgActivation == nullptr || m_display == nullptr) {
    return {};
  }

  struct TokenData {
    std::string token;
  } tokenData;

  auto* token = xdg_activation_v1_get_activation_token(m_xdgActivation);

  static const xdg_activation_token_v1_listener tokenListener = {
      .done = [](void* data, xdg_activation_token_v1* /*token*/, const char* tokenStr) {
        auto* td = static_cast<TokenData*>(data);
        td->token = tokenStr;
      },
  };

  xdg_activation_token_v1_add_listener(token, &tokenListener, &tokenData);
  xdg_activation_token_v1_set_serial(token, m_seatHandler.lastSerial(), m_seatHandler.seat());
  if (surface != nullptr) {
    xdg_activation_token_v1_set_surface(token, surface);
  }
  xdg_activation_token_v1_commit(token);
  wl_display_roundtrip(m_display);
  xdg_activation_token_v1_destroy(token);

  return tokenData.token;
}

void WaylandConnection::activateSurface(wl_surface* surface) {
  if (m_xdgActivation == nullptr || surface == nullptr) {
    return;
  }
  auto token = requestActivationToken(surface);
  if (!token.empty()) {
    xdg_activation_v1_activate(m_xdgActivation, token.c_str(), surface);
  }
}

wl_display* WaylandConnection::display() const noexcept { return m_display; }

std::string WaylandConnection::describeDisplayError(int operationErrno) const {
  if (m_display == nullptr) {
    if (operationErrno != 0) {
      return std::format("display=null, operation_errno={} ({})", operationErrno, errnoText(operationErrno));
    }
    return "display=null";
  }

  const int displayError = wl_display_get_error(m_display);
  std::string detail = std::format("display_error={} ({})", displayError, errnoText(displayError));
  if (operationErrno != 0 && operationErrno != displayError) {
    detail += std::format(", operation_errno={} ({})", operationErrno, errnoText(operationErrno));
  }

  if (displayError == EMFILE || displayError == ENFILE || operationErrno == EMFILE || operationErrno == ENFILE) {
    detail += std::format(", {}", ProcessFds::describeOpenFileDescriptors());
  }

  if (displayError == EPROTO) {
    const wl_interface* interface = nullptr;
    std::uint32_t objectId = 0;
    const std::uint32_t code = wl_display_get_protocol_error(m_display, &interface, &objectId);
    const char* interfaceName = interface != nullptr && interface->name != nullptr ? interface->name : "unknown";
    detail += std::format(", protocol_error.interface={}, object_id={}, code={}", interfaceName, objectId, code);
  }

  return detail;
}

wl_compositor* WaylandConnection::compositor() const noexcept { return m_compositor; }

wl_seat* WaylandConnection::seat() const noexcept { return m_seatHandler.seat(); }

wl_pointer* WaylandConnection::pointer() const noexcept { return m_seatHandler.pointer(); }

wl_shm* WaylandConnection::shm() const noexcept { return m_shm; }

wl_subcompositor* WaylandConnection::subcompositor() const noexcept { return m_subcompositor; }

xdg_wm_base* WaylandConnection::xdgWmBase() const noexcept { return m_xdgWmBase; }

wp_fractional_scale_manager_v1* WaylandConnection::fractionalScaleManager() const noexcept {
  return m_fractionalScaleManager;
}

void WaylandConnection::setPopupGrabHost(PopupGrabHost* host) noexcept { m_popupGrabHost = host; }
PopupGrabHost* WaylandConnection::popupGrabHost() const noexcept { return m_popupGrabHost; }
wp_viewporter* WaylandConnection::viewporter() const noexcept { return m_viewporter; }

void WaylandConnection::setOutputMetadataSource(OutputMetadataSource source) {
  m_outputMetadataSource = std::move(source);
}

void WaylandConnection::applyOutputMetadata() {
  if (!m_outputMetadataSource) {
    return;
  }
  bool changed = false;
  for (const WaylandOutputMetadata& info : m_outputMetadataSource()) {
    if (info.connectorName.empty()) {
      continue;
    }
    auto it = std::ranges::find_if(m_outputs, [&info](const WaylandOutput& output) {
      return output.connectorName == info.connectorName;
    });
    if (it == m_outputs.end()) {
      continue;
    }
    if (it->make != info.make || it->model != info.model || it->serialNumber != info.serialNumber) {
      it->make = info.make;
      it->model = info.model;
      it->serialNumber = info.serialNumber;
      changed = true;
    }
    it->headScaleFactor = info.scaleFactor;
    if (it->headTransform != info.transform) {
      it->headTransform = info.transform;
      changed = true;
    }
    if (recomputeConfiguredScale(*it)) {
      changed = true;
    }
  }
  if (changed && m_outputChangeCallback) {
    m_outputChangeCallback();
  }
}

bool WaylandConnection::recomputeConfiguredScale(WaylandOutput& out) {
  const DetectedOutputScale detected = detectOutputScale(out);
  const double detectedFactor = detected.available ? detected.scale : 0.0;
  const std::int32_t numerator =
      wayland::resolveConfiguredScaleNumerator(out.headScaleFactor, detectedFactor, out.scale);
  if (numerator == out.configuredScaleNumerator) {
    return false;
  }
  out.configuredScaleNumerator = numerator;
  return true;
}

const std::vector<WaylandOutput>& WaylandConnection::outputs() const noexcept { return m_outputs; }

WaylandOutput* WaylandConnection::findOutputByWl(wl_output* wlOutput) {
  for (auto& out : m_outputs) {
    if (out.output == wlOutput) {
      return &out;
    }
  }
  return nullptr;
}

const WaylandOutput* WaylandConnection::findOutputByWl(wl_output* wlOutput) const {
  for (const auto& out : m_outputs) {
    if (out.output == wlOutput) {
      return &out;
    }
  }
  return nullptr;
}

WaylandOutput* WaylandConnection::findOutputByXdg(zxdg_output_v1* xdgOutput) {
  for (auto& out : m_outputs) {
    if (out.xdgOutput == xdgOutput) {
      return &out;
    }
  }
  return nullptr;
}

void WaylandConnection::handleGlobal(
    void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  auto* self = static_cast<WaylandConnection*>(data);
  self->m_advertisedGlobals.push_back({.name = name, .interface = interface, .version = version});
  self->bindGlobal(registry, name, interface, version);
  // Extensions created from inside onGlobal already saw this global through their replay.
  const std::size_t extensionCount = self->m_extensions.size();
  for (std::size_t i = 0; i < extensionCount; ++i) {
    self->m_extensions[i].second->onGlobal(registry, name, interface, version);
  }
}

void WaylandConnection::replayGlobals(WaylandExtension& extension) {
  if (m_registry == nullptr) {
    return;
  }
  // A snapshot: onGlobal may dispatch, and globals announced meanwhile reach the extension directly.
  const std::vector<AdvertisedGlobal> globals = m_advertisedGlobals;
  for (const AdvertisedGlobal& global : globals) {
    extension.onGlobal(m_registry, global.name, global.interface, global.version);
  }
}

void WaylandConnection::handleGlobalRemove(void* data, wl_registry* /*registry*/, std::uint32_t name) {
  auto* self = static_cast<WaylandConnection*>(data);
  std::erase_if(self->m_advertisedGlobals, [name](const AdvertisedGlobal& global) { return global.name == name; });
  const std::size_t extensionCount = self->m_extensions.size();
  for (std::size_t i = 0; i < extensionCount; ++i) {
    self->m_extensions[i].second->onGlobalRemove(name);
  }

  auto it = std::ranges::find_if(self->m_outputs, [name](const WaylandOutput& output) { return output.name == name; });
  if (it == self->m_outputs.end()) {
    return;
  }

  // Detach the entry from m_outputs before running any callbacks: m_outputRemovedCallback runs
  // shell code that reads m_outputs (and may re-enter Wayland dispatch), so the container must be
  // in a consistent state — never observed or mutated mid-removal.
  WaylandOutput output = std::move(*it);
  self->m_outputs.erase(it);

  if (self->m_outputRemovedCallback && output.output != nullptr) {
    self->m_outputRemovedCallback(output.output);
  }
  if (output.xdgOutput != nullptr) {
    zxdg_output_v1_destroy(output.xdgOutput);
  }
  if (output.output != nullptr) {
    if (wl_output_get_version(output.output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
      wl_output_release(output.output);
    } else {
      wl_output_destroy(output.output);
    }
  }
  if (self->m_outputChangeCallback) {
    self->m_outputChangeCallback();
  }
}

void WaylandConnection::bindGlobal(
    wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  const std::string interfaceName = interface;

  if (m_purpose == Purpose::Sibling
      && interfaceName != wl_shm_interface.name
      && interfaceName != wl_output_interface.name) {
    return;
  }

  if (interfaceName == wl_compositor_interface.name) {
    const auto bindVersion = std::min(version, kCompositorVersion);
    m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion));
    return;
  }

  if (interfaceName == wl_seat_interface.name) {
    const auto bindVersion = std::min(version, kSeatVersion);
    m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bindVersion));
    m_seatHandler.bind(m_seat);
    bindTextInputService();
    return;
  }

  if (interfaceName == wl_shm_interface.name) {
    const auto bindVersion = std::min(version, kShmVersion);
    m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, bindVersion));
    return;
  }

  if (interfaceName == wl_subcompositor_interface.name) {
    const auto bindVersion = std::min(version, kSubcompositorVersion);
    m_subcompositor =
        static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, bindVersion));
    return;
  }

  if (interfaceName == zxdg_output_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kXdgOutputManagerVersion);
    m_xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
        wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == xdg_wm_base_interface.name) {
    const auto bindVersion = std::min(version, kXdgWmBaseVersion);
    m_xdgWmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, bindVersion));
    xdg_wm_base_add_listener(m_xdgWmBase, &kXdgWmBaseListener, this);
    return;
  }

  if (interfaceName == wp_cursor_shape_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kCursorShapeManagerVersion);
    m_cursorShapeManager = static_cast<wp_cursor_shape_manager_v1*>(
        wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, bindVersion)
    );
    m_seatHandler.setCursorShapeManager(m_cursorShapeManager);
    return;
  }

  if (interfaceName == xdg_activation_v1_interface.name) {
    const auto bindVersion = std::min(version, kXdgActivationVersion);
    m_xdgActivation =
        static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, bindVersion));
    return;
  }

  if (interfaceName == wp_fractional_scale_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kFractionalScaleManagerVersion);
    m_fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == wp_viewporter_interface.name) {
    const auto bindVersion = std::min(version, kViewporterVersion);
    m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, bindVersion));
    return;
  }

  if (interfaceName == zwp_text_input_manager_v3_interface.name) {
    const auto bindVersion = std::min(version, kTextInputManagerVersion);
    m_textInputManager = static_cast<zwp_text_input_manager_v3*>(
        wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, bindVersion)
    );
    bindTextInputService();
    return;
  }

  if (interfaceName == wl_output_interface.name) {
    const auto bindVersion = std::min(version, kOutputVersion);
    auto* output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, bindVersion));
    m_outputs.push_back(
        WaylandOutput{
            .name = name,
            .interfaceName = interfaceName,
            .connectorName = {},
            .description = {},
            .version = version,
            .output = output,
        }
    );
    wl_output_add_listener(output, &kOutputListener, this);
    if (m_xdgOutputManager != nullptr) {
      auto* xdgOut = zxdg_output_manager_v1_get_xdg_output(m_xdgOutputManager, output);
      m_outputs.back().xdgOutput = xdgOut;
      zxdg_output_v1_add_listener(xdgOut, &kXdgOutputListener, this);
    }
    // Notify the shell only after m_outputs is fully populated for this output: the callback runs
    // shell code that may mutate m_outputs, which would invalidate the back() access above.
    if (m_outputAddedCallback) {
      m_outputAddedCallback(output);
    }
  }
}

void WaylandConnection::bindTextInputService() {
  if (m_textInputService == nullptr) {
    return;
  }
  if (m_textInputManager == nullptr || m_seat == nullptr) {
    return;
  }
  m_textInputService->bind(m_textInputManager, m_seat);
}

void WaylandConnection::cleanup() {
  for (auto& [type, extension] : std::views::reverse(m_extensions)) {
    extension->releaseGlobals();
  }
  m_advertisedGlobals.clear();

  if (m_textInputService != nullptr) {
    m_textInputService->cleanup();
  }

  for (auto& out : m_outputs) {
    if (out.xdgOutput != nullptr) {
      zxdg_output_v1_destroy(out.xdgOutput);
      out.xdgOutput = nullptr;
    }
  }

  if (m_xdgOutputManager != nullptr) {
    zxdg_output_manager_v1_destroy(m_xdgOutputManager);
    m_xdgOutputManager = nullptr;
  }

  if (m_xdgWmBase != nullptr) {
    xdg_wm_base_destroy(m_xdgWmBase);
    m_xdgWmBase = nullptr;
  }

  m_seatHandler.cleanup();

  if (m_xdgActivation != nullptr) {
    xdg_activation_v1_destroy(m_xdgActivation);
    m_xdgActivation = nullptr;
  }

  if (m_fractionalScaleManager != nullptr) {
    wp_fractional_scale_manager_v1_destroy(m_fractionalScaleManager);
    m_fractionalScaleManager = nullptr;
  }

  if (m_viewporter != nullptr) {
    wp_viewporter_destroy(m_viewporter);
    m_viewporter = nullptr;
  }

  if (m_textInputManager != nullptr) {
    zwp_text_input_manager_v3_destroy(m_textInputManager);
    m_textInputManager = nullptr;
  }

  if (m_cursorShapeManager != nullptr) {
    wp_cursor_shape_manager_v1_destroy(m_cursorShapeManager);
    m_cursorShapeManager = nullptr;
  }

  if (m_seat != nullptr) {
    wl_seat_destroy(m_seat);
    m_seat = nullptr;
  }

  if (m_shm != nullptr) {
    wl_shm_destroy(m_shm);
    m_shm = nullptr;
  }

  if (m_subcompositor != nullptr) {
    wl_subcompositor_destroy(m_subcompositor);
    m_subcompositor = nullptr;
  }

  if (m_compositor != nullptr) {
    wl_compositor_destroy(m_compositor);
    m_compositor = nullptr;
  }

  for (auto& output : m_outputs) {
    if (output.output != nullptr) {
      if (m_outputRemovedCallback) {
        m_outputRemovedCallback(output.output);
      }
      wl_output_destroy(output.output);
      output.output = nullptr;
    }
  }

  if (m_registry != nullptr) {
    wl_registry_destroy(m_registry);
    m_registry = nullptr;
  }

  if (m_display != nullptr) {
    wl_display_disconnect(m_display);
    m_display = nullptr;
  }

  m_outputs.clear();
  m_surfaceOutputMap.clear();
  m_surfaceOutputs.clear();
  m_outputAddedCallback = nullptr;
  m_outputRemovedCallback = nullptr;
}

void WaylandConnection::logStartupSummary() const {
  kLog.info(
      "connected compositor={} shm={} xdg-shell={} xdg-output={} fractional-scale={} outputs={}",
      m_compositor != nullptr ? "yes" : "no", m_shm != nullptr ? "yes" : "no", hasXdgShell() ? "yes" : "no",
      hasXdgOutputManager() ? "yes" : "no", hasFractionalScale() ? "yes" : "no", m_outputs.size()
  );

  for (const auto& output : m_outputs) {
    const DetectedOutputScale detectedScale = detectOutputScale(output);
    if (detectedScale.available) {
      kLog.info(
          "output {} global={} wl_scale={} detected_fractional_scale={:.3F} logical={}x{} mode={}x{} orientation={} "
          "desc=\"{}\"",
          outputLabel(output), output.name, output.scale, detectedScale.scale, output.logicalWidth,
          output.logicalHeight, output.width, output.height, detectedScale.rotated ? "rotated" : "normal",
          output.description
      );
    } else {
      kLog.info(
          "output {} global={} wl_scale={} detected_fractional_scale=unavailable logical={}x{} mode={}x{} desc=\"{}\"",
          outputLabel(output), output.name, output.scale, output.logicalWidth, output.logicalHeight, output.width,
          output.height, output.description
      );
    }
  }
}
