#pragma once

#include "wayland/output_scale.h"
#include "wayland/wayland_extension.h"
#include "wayland/wayland_seat.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

struct wl_compositor;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct wl_subcompositor;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct xdg_wm_base;
struct wp_cursor_shape_manager_v1;
struct xdg_activation_v1;
struct zwp_text_input_manager_v3;
struct wp_fractional_scale_manager_v1;
struct wp_viewporter;
class PopupGrabHost;
class TextInputService;

struct WaylandOutput {
  std::uint32_t name = 0;
  std::string interfaceName;
  std::string connectorName;
  std::string description;
  // Supplied by an output metadata extension (wl_output has no serial); empty otherwise.
  std::string make;
  std::string model;
  std::string serialNumber;
  std::uint32_t version = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  std::int32_t logicalX = 0;
  std::int32_t logicalY = 0;
  std::int32_t transform = 0;
  // Transform from an output metadata extension, when it publishes one for this output.
  std::optional<std::int32_t> headTransform;
  zxdg_output_v1* xdgOutput = nullptr;
  bool done = false;
  // Fixed scale factor from an output metadata extension (0 == not reported).
  double headScaleFactor = 0.0;
  // Canonical published render scale as a /120 numerator (120 == 1.0). Resolved
  // from metadata scale, then mode/logical ratio, then integer wl_output.scale.
  std::int32_t configuredScaleNumerator = wayland::kScaleNumeratorBase;

  float configuredScale() const noexcept {
    return static_cast<float>(configuredScaleNumerator) / static_cast<float>(wayland::kScaleNumeratorBase);
  }

  std::int32_t effectiveTransform() const noexcept { return headTransform.value_or(transform); }

  std::int32_t effectiveLogicalWidth() const noexcept {
    if (logicalWidth > 0) {
      return logicalWidth;
    }
    if (width <= 0 || scale <= 0) {
      return 0;
    }
    const std::int32_t scaled = width / scale;
    return scaled > 0 ? scaled : 1;
  }

  std::int32_t effectiveLogicalHeight() const noexcept {
    if (logicalHeight > 0) {
      return logicalHeight;
    }
    if (height <= 0 || scale <= 0) {
      return 0;
    }
    const std::int32_t scaled = height / scale;
    return scaled > 0 ? scaled : 1;
  }

  bool hasUsableGeometry() const noexcept { return effectiveLogicalWidth() > 0 && effectiveLogicalHeight() > 0; }
};

// Output metadata an extension publishes per connector name (e.g. from wlr-output-management heads).
struct WaylandOutputMetadata {
  std::string connectorName;
  std::string make;
  std::string model;
  std::string serialNumber;
  // Fixed scale factor (0 == not reported).
  double scaleFactor = 0.0;
  std::optional<std::int32_t> transform;
};

class WaylandConnection {
public:
  // A sibling connection reaches the primary connection's compositor through connectUntil() and binds only
  // wl_shm and wl_output itself; its extensions bind the rest.
  enum class Purpose : std::uint8_t {
    Primary,
    Sibling,
  };

  explicit WaylandConnection(Purpose purpose = Purpose::Primary);
  ~WaylandConnection();

  WaylandConnection(const WaylandConnection&) = delete;
  WaylandConnection& operator=(const WaylandConnection&) = delete;

  using ChangeCallback = std::function<void()>;
  using OutputMetadataSource = std::function<std::span<const WaylandOutputMetadata>()>;

  bool connect();
  bool
  connectUntil(std::chrono::steady_clock::time_point deadline, WaylandConnection* eventConnection, std::string& error);

  // Delegate setters
  void setOutputChangeCallback(ChangeCallback callback);
  void setOutputLifecycleCallbacks(std::function<void(wl_output*)> added, std::function<void(wl_output*)> removed);
  void setPointerEventCallback(WaylandSeat::PointerEventCallback callback);
  void setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback);
  void setKeyboardModifiersCallback(WaylandSeat::KeyboardModifiersCallback callback);
  void setLockKeysChangeCallback(WaylandSeat::LockKeysChangeCallback callback);
  void setKeyboardLayoutChangeCallback(WaylandSeat::KeyboardLayoutChangeCallback callback);
  void setTextInputService(TextInputService* textInputService);
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);

  // Installs an extension's live output metadata; pass {} to remove it. Entries apply to the output with the same
  // connector name when that output reports its name and on applyOutputMetadata().
  void setOutputMetadataSource(OutputMetadataSource source);
  // Applies the source's entries to matching outputs, recomputing their configured scale, and fires the output change
  // callback once if anything changed.
  void applyOutputMetadata();

  // Returns this connection's instance of T, constructing it from the connection on first use. A new
  // extension first receives every global advertised so far, in advertisement order, then each later
  // announcement. Before disconnecting the display the connection calls releaseGlobals() on every
  // extension, newest first.
  template <typename T> T& extension() {
    static_assert(std::is_base_of_v<WaylandExtension, T>);
    const std::type_index key(typeid(T));
    for (auto& [type, instance] : m_extensions) {
      if (type == key) {
        return static_cast<T&>(*instance);
      }
    }
    auto& created = static_cast<T&>(*m_extensions.emplace_back(key, std::make_unique<T>(*this)).second);
    replayGlobals(created);
    return created;
  }

  int repeatPollTimeoutMs() const;
  void repeatTick();
  void stopKeyRepeat();

  // Queries
  bool isConnected() const noexcept;
  bool hasSubcompositor() const noexcept;
  bool hasXdgOutputManager() const noexcept;
  bool hasXdgShell() const noexcept;
  bool hasFractionalScale() const noexcept;
  wp_fractional_scale_manager_v1* fractionalScaleManager() const noexcept;
  // The focus-grab owner that popups enroll with instead of taking xdg_popup_grab, or nullptr.
  void setPopupGrabHost(PopupGrabHost* host) noexcept;
  PopupGrabHost* popupGrabHost() const noexcept;
  TextInputService* textInputService() const noexcept { return m_textInputService; }
  wp_viewporter* viewporter() const noexcept;
  wl_display* display() const noexcept;
  std::string describeDisplayError(int operationErrno = 0) const;
  wl_compositor* compositor() const noexcept;
  wl_seat* seat() const noexcept;
  wl_pointer* pointer() const noexcept;
  wl_shm* shm() const noexcept;
  wl_subcompositor* subcompositor() const noexcept;
  xdg_wm_base* xdgWmBase() const noexcept;
  const std::vector<WaylandOutput>& outputs() const noexcept;
  WaylandOutput* findOutputByWl(wl_output* wlOutput);
  const WaylandOutput* findOutputByWl(wl_output* wlOutput) const;
  WaylandOutput* findOutputByXdg(zxdg_output_v1* xdgOutput);

  bool hasXdgActivation() const noexcept;
  std::string requestActivationToken(wl_surface* surface) const;
  void activateSurface(wl_surface* surface);

  wl_output* lastPointerOutput() const noexcept;
  wl_surface* lastPointerSurface() const noexcept;
  wl_surface* lastKeyboardSurface() const noexcept;
  std::uint32_t keyboardModifiers() const noexcept;
  bool hasPointerPosition() const noexcept;
  double lastPointerX() const noexcept;
  double lastPointerY() const noexcept;
  WaylandSeat::InputSource lastInputSource() const noexcept;
  std::string currentKeyboardLayoutName() const;
  std::vector<std::string> keyboardLayoutNames() const;
  WaylandSeat::LockKeysState keyboardLockKeysState() const;
  std::uint32_t lastInputSerial() const noexcept;
  double userIdleSeconds() const noexcept;
  bool hasFreshPointerOutput(std::chrono::milliseconds maxAge) const noexcept;
  wl_output* outputForSurface(wl_surface* surface) const noexcept;

  void registerSurfaceOutput(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputLeave(wl_surface* surface, wl_output* output);
  void unregisterSurface(wl_surface* surface);
  void notifyOutputReady(wl_output* output);

  // Registry listener entrypoints
  static void
  handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  static void handleGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);

  // Recompute out.configuredScaleNumerator from current metadata/mode/logical/wl-scale
  // data. Returns true when the published numerator changed.
  bool recomputeConfiguredScale(WaylandOutput& out);

private:
  struct AdvertisedGlobal {
    std::uint32_t name = 0;
    std::string interface;
    std::uint32_t version = 0;
  };

  void bindGlobal(wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  void replayGlobals(WaylandExtension& extension);
  void bindTextInputService();
  void cleanup();
  [[nodiscard]] bool setupDisplay(wl_display* display, std::string& error);
  void logStartupSummary() const;

  Purpose m_purpose = Purpose::Primary;
  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  wl_seat* m_seat = nullptr;
  wl_shm* m_shm = nullptr;
  wl_subcompositor* m_subcompositor = nullptr;
  zxdg_output_manager_v1* m_xdgOutputManager = nullptr;
  xdg_wm_base* m_xdgWmBase = nullptr;
  wp_cursor_shape_manager_v1* m_cursorShapeManager = nullptr;
  xdg_activation_v1* m_xdgActivation = nullptr;
  wp_fractional_scale_manager_v1* m_fractionalScaleManager = nullptr;
  OutputMetadataSource m_outputMetadataSource;
  PopupGrabHost* m_popupGrabHost = nullptr;
  wp_viewporter* m_viewporter = nullptr;
  zwp_text_input_manager_v3* m_textInputManager = nullptr;
  TextInputService* m_textInputService = nullptr;
  std::vector<WaylandOutput> m_outputs;
  ChangeCallback m_outputChangeCallback;
  std::function<void(wl_output*)> m_outputAddedCallback;
  std::function<void(wl_output*)> m_outputRemovedCallback;
  std::unordered_map<wl_surface*, wl_output*> m_surfaceOutputMap;
  std::unordered_map<wl_surface*, std::vector<wl_output*>> m_surfaceOutputs;
  wl_output* m_lastPointerOutput = nullptr;
  std::chrono::steady_clock::time_point m_lastPointerOutputAt;
  WaylandSeat::PointerEventCallback m_pointerEventCallback;

  WaylandSeat m_seatHandler;
  std::vector<AdvertisedGlobal> m_advertisedGlobals;
  std::vector<std::pair<std::type_index, std::unique_ptr<WaylandExtension>>> m_extensions;
};

namespace wayland {

  struct DispatchTarget {
    WaylandConnection* connection = nullptr;
    std::string_view role;
  };

  enum class DispatchStatus : std::uint8_t {
    Completed,
    TimedOut,
    PollFailed,
    ConnectionFailed,
  };

  struct [[nodiscard]] DispatchResult {
    DispatchStatus status = DispatchStatus::Completed;
    WaylandConnection* failedConnection = nullptr;
    std::string error;
  };

  DispatchResult dispatchUntil(
      std::span<const DispatchTarget> targets, std::chrono::steady_clock::time_point deadline,
      const std::function<bool()>& completed
  );

} // namespace wayland
