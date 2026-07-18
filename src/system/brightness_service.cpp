#include "system/brightness_service.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_bus.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("brightness");
  constexpr float kDefaultBrightnessStep = 0.05f;

  namespace fs = std::filesystem;

  constexpr std::chrono::milliseconds kDdcDetectTimeout{15000};
  constexpr std::chrono::milliseconds kDdcQueryTimeout{10000};
  constexpr std::chrono::milliseconds kDdcSetTimeout{8000};
  constexpr std::chrono::seconds kDdcFailureCooldown{30};
  constexpr int kDdcFailureThreshold = 3;
  enum class RuntimeBackend : std::uint8_t {
    Backlight,
    Ddcutil,
  };

  struct DisplayInternal {
    BrightnessDisplay pub;
    RuntimeBackend backend = RuntimeBackend::Backlight;
    int maxRaw = 0;
    std::string backlightName;
    std::string sysfsPath;
    std::string connectorName;
    int inotifyWd = -1;

    int ddcBus = -1;
    // Incremented on each user brightness write; completions with an older epoch are ignored.
    std::uint64_t ddcWriteEpoch = 0;
    int failureCount = 0;
    bool quarantined = false;
    std::chrono::steady_clock::time_point cooldownUntil;
  };

  struct DdcCandidate {
    std::string connectorName;
    std::string label;
    int bus = -1;
    int currentRaw = -1;
    int maxRaw = 100;
  };

  struct CommandResult {
    bool launched = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    std::string output;
  };

  struct DdcJob {
    std::uint64_t generation = 0;
    std::uint64_t writeEpoch = 0;
    std::string displayId;
    int bus = -1;
    int targetRaw = -1;
    int maxRaw = 100;
  };

  struct WorkerCompletion {
    enum class Type : std::uint8_t {
      Detect,
      Refresh,
      Set,
    };

    Type type = Type::Detect;
    std::uint64_t generation = 0;
    std::uint64_t writeEpoch = 0;
    std::string displayId;
    bool success = false;
    bool timedOut = false;
    int currentRaw = -1;
    int maxRaw = 100;
    std::string detail;
    std::vector<DdcCandidate> candidates;
  };

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kLogindSessionInterface = "org.freedesktop.login1.Session";

  std::string joinBrightnessDisplayIds(const BrightnessService& service) {
    std::string out;
    for (const auto& display : service.displays()) {
      if (!display.controllable) {
        continue;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += display.id;
    }
    return out;
  }

  int readSysfsInt(const std::string& path) {
    std::ifstream file(path);
    int value = -1;
    if (file.is_open()) {
      file >> value;
    }
    return value;
  }

  float normalizedBrightness(int currentRaw, int maxRaw) {
    if (currentRaw < 0 || maxRaw <= 0) {
      return 0.0f;
    }
    return std::clamp(static_cast<float>(currentRaw) / static_cast<float>(maxRaw), 0.0f, 1.0f);
  }

  float readBacklightBrightness(const std::string& sysfsPath, int maxRaw) {
    const int requested = readSysfsInt(sysfsPath + "/brightness");
    return normalizedBrightness(requested, maxRaw);
  }

  const WaylandOutput* findOutputByConnector(const WaylandConnection& wayland, std::string_view connectorName) {
    if (connectorName.empty()) {
      return nullptr;
    }
    for (const auto& output : wayland.outputs()) {
      if (output.connectorName == connectorName) {
        return &output;
      }
    }
    return nullptr;
  }

  BrightnessBackendPreference backendPreferenceForOutput(const BrightnessConfig& config, const WaylandOutput* output) {
    if (output == nullptr) {
      return BrightnessBackendPreference::Auto;
    }

    for (const auto& override : config.monitorOverrides) {
      const std::string match = !override.match.empty() ? override.match : std::string{};
      if (match.empty() || !outputMatchesSelector(match, *output)) {
        continue;
      }
      if (override.backend.has_value()) {
        return *override.backend;
      }
      break;
    }

    return BrightnessBackendPreference::Auto;
  }

  // Returns the explicit backlight device name/path configured for this output, if any.
  std::optional<std::string> backlightDeviceForOutput(const BrightnessConfig& config, const WaylandOutput* output) {
    if (output == nullptr) {
      return std::nullopt;
    }

    for (const auto& override : config.monitorOverrides) {
      if (override.match.empty() || !outputMatchesSelector(override.match, *output)) {
        continue;
      }
      if (override.backlightDevice.has_value()) {
        return override.backlightDevice;
      }
      break;
    }

    return std::nullopt;
  }

  // Returns the sysfs device name from either a bare name ("intel_backlight") or a path.
  std::string_view extractBacklightDeviceName(std::string_view deviceSpec) {
    const auto lastSlash = deviceSpec.rfind('/');
    if (lastSlash != std::string_view::npos) {
      return deviceSpec.substr(lastSlash + 1);
    }
    return deviceSpec;
  }

  void applyOutputMetadata(BrightnessDisplay& display, const WaylandOutput& output) {
    display.label = output.description.empty() ? output.connectorName : output.description;
    display.physicalWidth = output.width;
    display.physicalHeight = output.height;
    display.logicalWidth = output.logicalWidth;
    display.logicalHeight = output.logicalHeight;
    display.scale = std::max(1, output.scale);
  }

  BrightnessDisplay disabledDisplayForOutput(const WaylandOutput& output) {
    BrightnessDisplay display;
    display.id = output.connectorName;
    display.controllable = false;
    applyOutputMetadata(display, output);
    if (display.label.empty()) {
      display.label = output.connectorName;
    }
    return display;
  }

  struct BacklightConnectorResolution {
    std::string connectorName;
    bool exactDrmMatch = false;
  };

  std::string readBacklightType(const std::string& sysfsPath) {
    std::ifstream file(sysfsPath + "/type");
    std::string type;
    if (!file.is_open()) {
      return {};
    }
    std::getline(file, type);
    return StringUtils::toLower(StringUtils::trim(type));
  }

  BacklightConnectorResolution
  resolveBacklightConnector(const std::string& sysfsPath, const WaylandConnection& wayland) {
    std::error_code ec;

    const auto devicePath = fs::canonical(sysfsPath + "/device", ec);
    if (ec) {
      return {};
    }

    const std::string devicePathStr = devicePath.string();
    const std::string drmClassPath = "/sys/class/drm";
    DIR* dir = ::opendir(drmClassPath.c_str());
    if (dir == nullptr) {
      return {};
    }

    std::string match;
    while (auto* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (!name.contains('-')) {
        continue;
      }

      const auto dashPos = name.find('-');
      if (dashPos == std::string::npos) {
        continue;
      }

      const std::string connector = name.substr(dashPos + 1);
      if (findOutputByConnector(wayland, connector) == nullptr) {
        continue;
      }

      const auto drmConnectorPath = fs::canonical(drmClassPath + "/" + name, ec);
      if (ec) {
        continue;
      }

      const std::string drmConnectorStr = drmConnectorPath.string();
      if (devicePathStr == drmConnectorStr || devicePathStr.starts_with(drmConnectorStr + "/")) {
        match = connector;
        break;
      }
    }
    ::closedir(dir);

    if (!match.empty()) {
      return {.connectorName = match, .exactDrmMatch = true};
    }

    if (match.empty()) {
      for (const auto& output : wayland.outputs()) {
        if (output.connectorName.starts_with("eDP")) {
          return {.connectorName = output.connectorName, .exactDrmMatch = false};
        }
      }
    }

    return {};
  }

  int backlightTypeRank(std::string_view type) {
    if (type == "raw") {
      return 0;
    }
    if (type == "platform") {
      return 1;
    }
    if (type == "firmware") {
      return 2;
    }
    return 3;
  }

  int backlightNamePenalty(std::string_view name) {
    if (name.starts_with("nvidia")) {
      return 2;
    }
    if (name.starts_with("acpi_video")) {
      return 1;
    }
    return 0;
  }

  struct BacklightCandidate {
    DisplayInternal display;
    bool exactDrmMatch = false;
    std::string type;
  };

  bool isBetterBacklightCandidate(const BacklightCandidate& current, const BacklightCandidate& next) {
    if (current.exactDrmMatch != next.exactDrmMatch) {
      return next.exactDrmMatch;
    }

    const int currentTypeRank = backlightTypeRank(current.type);
    const int nextTypeRank = backlightTypeRank(next.type);
    if (currentTypeRank != nextTypeRank) {
      return nextTypeRank < currentTypeRank;
    }

    const int currentPenalty = backlightNamePenalty(current.display.backlightName);
    const int nextPenalty = backlightNamePenalty(next.display.backlightName);
    if (currentPenalty != nextPenalty) {
      return nextPenalty < currentPenalty;
    }

    if (current.display.maxRaw != next.display.maxRaw) {
      return next.display.maxRaw > current.display.maxRaw;
    }

    return next.display.backlightName < current.display.backlightName;
  }

  sdbus::ObjectPath resolveSessionPath(sdbus::IConnection& connection) {
    try {
      auto managerProxy = sdbus::createProxy(connection, kLogindBusName, sdbus::ObjectPath{"/org/freedesktop/login1"});

      if (const char* sessionId = std::getenv("XDG_SESSION_ID"); sessionId != nullptr && sessionId[0] != '\0') {
        try {
          sdbus::ObjectPath sessionPath;
          managerProxy->callMethod("GetSession")
              .onInterface(kLogindManagerInterface)
              .withArguments(std::string(sessionId))
              .storeResultsTo(sessionPath);
          return sessionPath;
        } catch (const sdbus::Error& e) {
          kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", sessionId, e.what());
        }
      }

      sdbus::ObjectPath sessionPath;
      managerProxy->callMethod("GetSessionByPID")
          .onInterface(kLogindManagerInterface)
          .withArguments(static_cast<std::uint32_t>(::getpid()))
          .storeResultsTo(sessionPath);
      return sessionPath;
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to resolve logind session: {}", e.what());
      return sdbus::ObjectPath{"/org/freedesktop/login1/session/auto"};
    }
  }

  std::optional<int> parseTrailingInteger(std::string_view input) {
    std::size_t end = input.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(input[end - 1]))) {
      --end;
    }
    if (end == 0) {
      return std::nullopt;
    }

    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(input[start - 1])) != 0) {
      --start;
    }
    if (start == end) {
      return std::nullopt;
    }

    return std::atoi(std::string(input.substr(start, end - start)).c_str());
  }

  std::optional<int> parseI2cBus(std::string_view line) {
    const std::size_t pathPos = line.find("/dev/i2c-");
    if (pathPos != std::string::npos) {
      return parseTrailingInteger(line.substr(pathPos));
    }
    return parseTrailingInteger(line);
  }

  std::string normalizeConnectorName(std::string_view raw) {
    std::string connector = StringUtils::trim(raw);
    if (connector.starts_with("card")) {
      const auto dash = connector.find('-');
      if (dash != std::string::npos) {
        connector = connector.substr(dash + 1);
      }
    }
    return connector;
  }

  std::optional<std::pair<int, int>> parseDdcVcpBrightness(const std::string& output) {
    std::size_t start = 0;
    while (start <= output.size()) {
      const std::size_t end = output.find('\n', start);
      const std::string line = output.substr(start, end == std::string::npos ? output.size() - start : end - start);
      const std::string lower = StringUtils::toLower(line);
      const std::size_t currentPos = lower.find("current value");
      const std::size_t maxPos = lower.find("max value");
      if (currentPos == std::string::npos || maxPos == std::string::npos || maxPos <= currentPos) {
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
        continue;
      }

      const auto current = parseTrailingInteger(line.substr(currentPos, maxPos - currentPos));
      const auto max = parseTrailingInteger(line.substr(maxPos));
      if (current.has_value() && max.has_value() && *max > 0) {
        return std::pair<int, int>{*current, *max};
      }

      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return std::nullopt;
  }

  CommandResult runCommandCapture(
      const std::vector<std::string>& args, std::chrono::milliseconds timeout,
      const std::shared_ptr<std::atomic<bool>>& cancel
  ) {
    if (cancel->load(std::memory_order_relaxed)) {
      return {.cancelled = true};
    }

    process::RunOptions options;
    options.timeout = timeout;
    options.cancel = cancel;
    process::RunResult runResult = process::runSync(args, std::move(options));

    std::string output = std::move(runResult.out);
    if (!runResult.err.empty()) {
      if (!output.empty()) {
        output += '\n';
      }
      output += runResult.err;
    }

    const bool cancelled = cancel->load(std::memory_order_relaxed);
    return {
        .launched = runResult.exitCode >= 0,
        .timedOut = runResult.timedOut && !cancelled,
        .cancelled = cancelled,
        .exitCode = runResult.exitCode,
        .output = std::move(output),
    };
  }

  std::vector<std::string> ddcDetectArgs(const std::vector<std::string>& ignoreMmids) {
    std::vector<std::string> args{"ddcutil", "--noconfig"};
    for (const auto& mmid : ignoreMmids) {
      args.emplace_back("--ignore-mmid");
      args.push_back(mmid);
    }
    args.emplace_back("detect");
    return args;
  }

  std::vector<std::string> ddcBaseArgs(int bus) {
    return {"ddcutil", "--noconfig", "--enable-dynamic-sleep", "--sleep-multiplier",
            "0.1",     "--bus",      std::to_string(bus)};
  }

  std::optional<std::pair<int, int>> queryDdcBrightness(
      int bus, std::chrono::milliseconds timeout, std::string* detailOut,
      const std::shared_ptr<std::atomic<bool>>& cancel
  ) {
    auto args = ddcBaseArgs(bus);
    args.emplace_back("getvcp");
    args.emplace_back("10");

    const CommandResult result = runCommandCapture(args, timeout, cancel);
    if (detailOut != nullptr) {
      *detailOut = result.output;
    }
    if (!result.launched || result.timedOut || result.cancelled || result.exitCode != 0) {
      return std::nullopt;
    }
    return parseDdcVcpBrightness(result.output);
  }

  std::vector<DdcCandidate> detectDdcDisplays(
      std::chrono::milliseconds timeout, const std::vector<std::string>& ignoreMmids, std::string* detailOut,
      const std::shared_ptr<std::atomic<bool>>& cancel
  ) {
    auto args = ddcDetectArgs(ignoreMmids);
    const CommandResult detectResult = runCommandCapture(args, timeout, cancel);
    if (detailOut != nullptr) {
      *detailOut = detectResult.output;
    }
    if (detectResult.cancelled) {
      return {};
    }
    if (!detectResult.launched) {
      kLog.warn("ddcutil detect could not be launched");
      return {};
    }
    if (detectResult.timedOut) {
      kLog.warn("ddcutil detect timed out after {}ms", timeout.count());
      return {};
    }
    if (detectResult.exitCode != 0) {
      kLog.warn(
          "ddcutil detect failed with exit code {}: {}", detectResult.exitCode, StringUtils::trim(detectResult.output)
      );
      return {};
    }

    std::vector<DdcCandidate> candidates;
    DdcCandidate current;
    bool inDisplay = false;

    auto flushCurrent = [&]() {
      if (cancel->load(std::memory_order_relaxed)) {
        current = DdcCandidate{};
        return;
      }
      if (inDisplay && current.bus >= 0 && !current.connectorName.empty()) {
        if (std::string getvcpDetail; true) {
          const auto brightness = queryDdcBrightness(current.bus, kDdcQueryTimeout, &getvcpDetail, cancel);
          if (brightness.has_value()) {
            current.currentRaw = brightness->first;
            current.maxRaw = brightness->second;
          } else if (!cancel->load(std::memory_order_relaxed)) {
            kLog.warn(
                "ddcutil: skipping bus {} because brightness query failed: {}", current.bus,
                StringUtils::trim(getvcpDetail)
            );
          }
        }
        if (current.currentRaw >= 0 && current.maxRaw > 0) {
          candidates.push_back(current);
        }
      }
      current = DdcCandidate{};
    };

    std::size_t start = 0;
    while (start <= detectResult.output.size()) {
      const std::size_t end = detectResult.output.find('\n', start);
      const std::string line = StringUtils::trim(
          detectResult.output.substr(start, end == std::string::npos ? detectResult.output.size() - start : end - start)
      );
      if (line.starts_with("Display ")) {
        flushCurrent();
        inDisplay = !line.starts_with("Display not found");
      } else if (line.starts_with("Invalid display") || line.starts_with("DDC_disabled")) {
        flushCurrent();
        inDisplay = false;
      } else if (line.starts_with("I2C bus:")) {
        if (const auto bus = parseI2cBus(line); bus.has_value()) {
          current.bus = *bus;
        }
      } else if (line.starts_with("DRM connector:")) {
        current.connectorName = normalizeConnectorName(line.substr(std::strlen("DRM connector:")));
      } else if (line.starts_with("DRM_connector:")) {
        current.connectorName = normalizeConnectorName(line.substr(std::strlen("DRM_connector:")));
      } else if (line.starts_with("Monitor:")) {
        current.label = StringUtils::trim(line.substr(std::strlen("Monitor:")));
      } else if (line.starts_with("Model:") && current.label.empty()) {
        current.label = StringUtils::trim(line.substr(std::strlen("Model:")));
      }

      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    flushCurrent();
    if (cancel->load(std::memory_order_relaxed)) {
      return {};
    }
    kLog.info("ddcutil detect parsed {} candidate display(s)", candidates.size());
    return candidates;
  }

} // namespace

struct BrightnessService::Impl {
  SystemBus* bus = nullptr;
  WaylandConnection& wayland;
  CompositorPlatform& platform;
  BrightnessConfig activeConfig;
  ChangeCallback changeCallback;

  std::vector<BrightnessDisplay> publicDisplays;
  std::vector<DisplayInternal> internals;

  std::unique_ptr<sdbus::IProxy> sessionProxy;
  sdbus::ObjectPath sessionPath;

  int inotifyFd = -1;
  int eventFd = -1;
  int epollFd = -1;
  std::uint64_t generation = 0;
  bool warnedMissingDdcutil = false;

  std::mutex workerMutex;
  std::condition_variable workerCv;
  std::thread workerThread;
  std::shared_ptr<std::atomic<bool>> workerCancel = std::make_shared<std::atomic<bool>>(false);
  bool workerStop = false;
  bool detectPending = false;
  std::uint64_t detectGeneration = 0;
  std::unordered_map<std::string, DdcJob> pendingWrites;
  std::unordered_map<std::string, DdcJob> pendingRefreshes;
  std::queue<WorkerCompletion> completions;

  Impl(SystemBus* systemBus, CompositorPlatform& compositorPlatform, BrightnessConfig config)
      : bus(systemBus), wayland(compositorPlatform.wayland()), platform(compositorPlatform),
        activeConfig(std::move(config)) {
    setupPollFds();
    workerThread = std::thread([this]() { workerLoop(); });
  }

  ~Impl() {
    requestStop();
    if (workerThread.joinable()) {
      workerThread.join();
    }
    clearBacklightWatches();
    if (epollFd >= 0) {
      ::close(epollFd);
    }
    if (eventFd >= 0) {
      ::close(eventFd);
    }
    if (inotifyFd >= 0) {
      ::close(inotifyFd);
    }
  }

  void requestStop() {
    workerCancel->store(true, std::memory_order_relaxed);
    {
      std::scoped_lock lock(workerMutex);
      workerStop = true;
      detectPending = false;
      pendingWrites.clear();
      pendingRefreshes.clear();
      workerCv.notify_all();
    }
  }

  void setupPollFds() {
    if (inotifyFd < 0) {
      inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (inotifyFd < 0) {
        kLog.warn("inotify_init1 failed, external backlight changes won't be tracked");
      }
    }

    if (eventFd < 0) {
      eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (eventFd < 0) {
        kLog.warn("eventfd failed, asynchronous DDC brightness updates will be disabled");
      }
    }

    if (epollFd < 0 && (inotifyFd >= 0 || eventFd >= 0)) {
      epollFd = epoll_create1(EPOLL_CLOEXEC);
      if (epollFd < 0) {
        kLog.warn("epoll_create1 failed, brightness watcher integration degraded");
        return;
      }

      auto addFd = [this](int fd) {
        if (fd < 0) {
          return;
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
          kLog.warn("epoll_ctl add failed for fd {}", fd);
        }
      };

      addFd(inotifyFd);
      addFd(eventFd);
    }
  }

  void reload(const BrightnessConfig& config) {
    activeConfig = config;
    rebuildState(true);
  }

  void onOutputsChanged() { rebuildState(true); }

  void rebuildState(bool notify) {
    const auto oldPublic = publicDisplays;
    ++generation;

    clearBacklightWatches();
    removeDdcDisplays();
    updateSessionProxy();
    enumerateBacklights();
    rebuildPublic();
    scheduleDdcDetect();

    if (notify && oldPublic != publicDisplays && changeCallback) {
      changeCallback();
    }
  }

  void updateSessionProxy() {
    sessionProxy.reset();
    if (bus == nullptr) {
      return;
    }

    sessionPath = resolveSessionPath(bus->connection());
    sessionProxy = sdbus::createProxy(bus->connection(), kLogindBusName, sessionPath);
  }

  void clearBacklightWatches() {
    if (inotifyFd >= 0) {
      for (auto& display : internals) {
        if (display.inotifyWd >= 0) {
          inotify_rm_watch(inotifyFd, display.inotifyWd);
          display.inotifyWd = -1;
        }
      }
    }
    std::erase_if(internals, [](const DisplayInternal& display) {
      return display.backend == RuntimeBackend::Backlight;
    });
  }

  void removeDdcDisplays() {
    std::erase_if(internals, [](const DisplayInternal& display) { return display.backend == RuntimeBackend::Ddcutil; });
    {
      std::scoped_lock lock(workerMutex);
      pendingWrites.clear();
      pendingRefreshes.clear();
      detectPending = false;
    }
  }

  void enumerateBacklights() {
    const std::string backlightDir = "/sys/class/backlight";
    DIR* dir = ::opendir(backlightDir.c_str());
    if (dir == nullptr) {
      kLog.debug("no /sys/class/backlight directory");
      return;
    }

    std::unordered_map<std::string, BacklightCandidate> bestByConnector;
    while (auto* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") {
        continue;
      }

      const std::string path = backlightDir + "/" + name;
      const int maxBrightness = readSysfsInt(path + "/max_brightness");
      if (maxBrightness <= 0) {
        continue;
      }

      const BacklightConnectorResolution resolution = resolveBacklightConnector(path, wayland);
      const std::string& connectorName = resolution.connectorName;
      const WaylandOutput* output = findOutputByConnector(wayland, connectorName);

      if (connectorName.empty() || output == nullptr) {
        kLog.debug("skipping backlight '{}' because it could not be matched to an active output", name);
        continue;
      }

      const BrightnessBackendPreference preference = backendPreferenceForOutput(activeConfig, output);
      if (preference == BrightnessBackendPreference::None || preference == BrightnessBackendPreference::Ddcutil) {
        continue;
      }

      if (const auto explicitDevice = backlightDeviceForOutput(activeConfig, output); explicitDevice.has_value()) {
        if (extractBacklightDeviceName(*explicitDevice) != name) {
          kLog.debug(
              "skipping backlight '{}' for connector {} (explicit device '{}' configured)", name, connectorName,
              *explicitDevice
          );
          continue;
        }
      }

      DisplayInternal display;
      display.backend = RuntimeBackend::Backlight;
      display.maxRaw = maxBrightness;
      display.backlightName = name;
      display.sysfsPath = path;
      display.connectorName = connectorName;
      display.pub.id = !connectorName.empty() ? connectorName : name;
      display.pub.brightness = readBacklightBrightness(path, maxBrightness);

      if (output != nullptr) {
        applyOutputMetadata(display.pub, *output);
      }
      if (display.pub.label.empty()) {
        display.pub.label = !connectorName.empty() ? connectorName : name;
      }

      const std::string backlightType = readBacklightType(path);

      kLog.info(
          "found backlight candidate '{}' type='{}' current={:.0f}% connector={} match={}", name, backlightType,
          display.pub.brightness * 100.0f, display.connectorName.empty() ? "(none)" : display.connectorName,
          resolution.exactDrmMatch ? "exact" : "fallback"
      );
      BacklightCandidate candidate{
          .display = std::move(display),
          .exactDrmMatch = resolution.exactDrmMatch,
          .type = backlightType,
      };

      auto it = bestByConnector.find(connectorName);
      if (it == bestByConnector.end()) {
        bestByConnector.emplace(connectorName, std::move(candidate));
        continue;
      }

      if (isBetterBacklightCandidate(it->second, candidate)) {
        kLog.info(
            "preferring backlight '{}' over '{}' for connector {} (type='{}' exact={})",
            candidate.display.backlightName, it->second.display.backlightName, connectorName, candidate.type,
            candidate.exactDrmMatch
        );
        it->second = std::move(candidate);
      }
    }

    ::closedir(dir);

    for (auto& [connector, candidate] : bestByConnector) {
      if (inotifyFd >= 0) {
        const std::string watchPath = candidate.display.sysfsPath + "/brightness";
        candidate.display.inotifyWd = inotify_add_watch(inotifyFd, watchPath.c_str(), IN_MODIFY);
        if (candidate.display.inotifyWd < 0) {
          kLog.debug("inotify_add_watch failed for {}", watchPath);
        }
      }
      kLog.info(
          "selected backlight '{}' type='{}' connector={} match={}", candidate.display.backlightName, candidate.type,
          connector, candidate.exactDrmMatch ? "exact" : "fallback"
      );
      internals.push_back(std::move(candidate.display));
    }
  }

  void scheduleDdcDetect() {
    if (!activeConfig.enableDdcutil) {
      return;
    }
    if (!process::commandExists("ddcutil")) {
      if (!warnedMissingDdcutil) {
        kLog.warn("brightness.enable_ddcutil is set but ddcutil is not installed");
        warnedMissingDdcutil = true;
      }
      return;
    }

    warnedMissingDdcutil = false;
    std::scoped_lock lock(workerMutex);
    detectPending = true;
    detectGeneration = generation;
    workerCv.notify_all();
  }

  void rebuildPublic() {
    publicDisplays.clear();
    publicDisplays.reserve(internals.size() + wayland.outputs().size());
    for (const auto& display : internals) {
      publicDisplays.push_back(display.pub);
    }

    for (const auto& output : wayland.outputs()) {
      if (!output.done || output.connectorName.empty()) {
        continue;
      }

      const bool hasDisplay = std::ranges::any_of(internals, [&output](const DisplayInternal& display) {
        return display.connectorName == output.connectorName;
      });
      if (hasDisplay) {
        continue;
      }

      publicDisplays.push_back(disabledDisplayForOutput(output));
    }
  }

  DisplayInternal* findInternal(const std::string& id) {
    for (auto& display : internals) {
      if (display.pub.id == id) {
        return &display;
      }
    }
    return nullptr;
  }

  const DisplayInternal* findInternal(const std::string& id) const {
    for (const auto& display : internals) {
      if (display.pub.id == id) {
        return &display;
      }
    }
    return nullptr;
  }

  void syncPublicDisplay(const DisplayInternal& display) {
    for (auto& pub : publicDisplays) {
      if (pub.id == display.pub.id) {
        pub = display.pub;
        return;
      }
    }
  }

  void setBrightness(DisplayInternal* display, float value) {
    value = std::clamp(value, activeConfig.minimumBrightness, 1.0f);
    switch (display->backend) {
    case RuntimeBackend::Backlight:
      setBacklightBrightness(*display, value);
      break;
    case RuntimeBackend::Ddcutil:
      setDdcBrightness(*display, value);
      break;
    }
  }

  void setBrightness(const std::string& displayId, float value) {
    if (activeConfig.syncBrightnessOfAllMonitors) {
      setAllBrightness(value);
      return;
    }

    DisplayInternal* display = findInternal(displayId);
    if (display == nullptr) {
      return;
    }
    setBrightness(display, value);
  }

  void setAllBrightness(float value) {
    for (auto& display : internals) {
      setBrightness(&display, value);
    }
  }

  void setBacklightBrightness(DisplayInternal& display, float value) {
    const auto rawValue = static_cast<std::uint32_t>(std::round(value * static_cast<float>(display.maxRaw)));
    const std::string& backlightName = display.backlightName.empty() ? display.pub.id : display.backlightName;
    if (sessionProxy != nullptr) {
      try {
        sessionProxy->callMethod("SetBrightness")
            .onInterface(kLogindSessionInterface)
            .withArguments(std::string("backlight"), backlightName, rawValue);
        display.pub.brightness = value;
        syncPublicDisplay(display);
        if (changeCallback) {
          changeCallback();
        }
        return;
      } catch (const sdbus::Error& e) {
        kLog.warn("SetBrightness failed for '{}' via '{}': {}", display.pub.id, backlightName, e.what());
      }
    }
    writeSysfsBacklight(display, rawValue);
  }

  bool writeSysfsBacklight(DisplayInternal& display, std::uint32_t rawValue) {
    const std::string brightnessPath = display.sysfsPath + "/brightness";
    std::ofstream file(brightnessPath);
    if (!file.is_open()) {
      kLog.warn("cannot open sysfs brightness file '{}'", brightnessPath);
      return false;
    }
    file << rawValue;
    file.close();
    if (file.fail()) {
      kLog.warn("failed to write brightness to '{}'", brightnessPath);
      return false;
    }
    display.pub.brightness = static_cast<float>(rawValue) / static_cast<float>(display.maxRaw);
    syncPublicDisplay(display);
    if (changeCallback) {
      changeCallback();
    }
    return true;
  }

  void setDdcBrightness(DisplayInternal& display, float value) {
    const auto now = std::chrono::steady_clock::now();
    if (display.quarantined && now < display.cooldownUntil) {
      kLog.debug("ddcutil write skipped for '{}' during cooldown", display.pub.id);
      return;
    }

    ++display.ddcWriteEpoch;
    display.pub.brightness = value;
    display.quarantined = false;
    syncPublicDisplay(display);
    if (changeCallback) {
      changeCallback();
    }

    DdcJob job{
        .generation = generation,
        .writeEpoch = display.ddcWriteEpoch,
        .displayId = display.pub.id,
        .bus = display.ddcBus,
        .targetRaw = static_cast<int>(std::round(value * static_cast<float>(display.maxRaw))),
        .maxRaw = display.maxRaw,
    };

    std::scoped_lock lock(workerMutex);
    pendingWrites[display.pub.id] = job;
    pendingRefreshes.erase(display.pub.id);
    workerCv.notify_all();
  }

  void queueDdcRefreshes() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(workerMutex);

    for (const auto& display : internals) {
      if (display.backend != RuntimeBackend::Ddcutil) {
        continue;
      }
      if (display.quarantined && now < display.cooldownUntil) {
        continue;
      }
      if (pendingWrites.contains(display.pub.id)) {
        continue;
      }

      pendingRefreshes[display.pub.id] = DdcJob{
          .generation = generation,
          .writeEpoch = display.ddcWriteEpoch,
          .displayId = display.pub.id,
          .bus = display.ddcBus,
          .targetRaw = -1,
      };
    }

    workerCv.notify_all();
  }

  void workerLoop() {
    while (true) {
      bool runDetect = false;
      std::uint64_t detectGen = 0;
      std::optional<DdcJob> writeJob;
      std::optional<DdcJob> refreshJob;
      std::vector<std::string> ignoreMmids;

      {
        std::unique_lock lock(workerMutex);
        workerCv.wait(lock, [this]() {
          return workerStop || detectPending || !pendingWrites.empty() || !pendingRefreshes.empty();
        });

        if (workerStop) {
          return;
        }

        ignoreMmids = activeConfig.ddcutilIgnoreMmids;

        if (detectPending) {
          runDetect = true;
          detectGen = detectGeneration;
          detectPending = false;
        } else if (!pendingWrites.empty()) {
          auto it = pendingWrites.begin();
          writeJob = it->second;
          pendingWrites.erase(it);
          pendingRefreshes.erase(writeJob->displayId);
        } else if (!pendingRefreshes.empty()) {
          auto it = pendingRefreshes.begin();
          refreshJob = it->second;
          pendingRefreshes.erase(it);
        }
      }

      if (runDetect) {
        WorkerCompletion completion;
        completion.type = WorkerCompletion::Type::Detect;
        completion.generation = detectGen;
        completion.success = true;
        std::string detail;
        completion.candidates = detectDdcDisplays(kDdcDetectTimeout, ignoreMmids, &detail, workerCancel);
        completion.detail = std::move(detail);
        enqueueCompletion(std::move(completion));
        continue;
      }

      if (writeJob.has_value()) {
        WorkerCompletion completion;
        completion.type = WorkerCompletion::Type::Set;
        completion.generation = writeJob->generation;
        completion.writeEpoch = writeJob->writeEpoch;
        completion.displayId = writeJob->displayId;

        auto args = ddcBaseArgs(writeJob->bus);
        args.emplace_back("--noverify");
        args.emplace_back("setvcp");
        args.emplace_back("10");
        args.push_back(std::to_string(std::clamp(writeJob->targetRaw, 0, std::max(1, writeJob->maxRaw))));

        const CommandResult result = runCommandCapture(args, kDdcSetTimeout, workerCancel);
        completion.timedOut = result.timedOut;
        completion.detail = result.output;
        completion.success = result.launched && !result.timedOut && !result.cancelled && result.exitCode == 0;
        completion.currentRaw = writeJob->targetRaw;
        completion.maxRaw = writeJob->maxRaw;

        enqueueCompletion(std::move(completion));
        continue;
      }

      if (refreshJob.has_value()) {
        WorkerCompletion completion;
        completion.type = WorkerCompletion::Type::Refresh;
        completion.generation = refreshJob->generation;
        completion.writeEpoch = refreshJob->writeEpoch;
        completion.displayId = refreshJob->displayId;

        std::string detail;
        const auto brightness = queryDdcBrightness(refreshJob->bus, kDdcQueryTimeout, &detail, workerCancel);
        completion.detail = std::move(detail);
        completion.success = brightness.has_value();
        if (brightness.has_value()) {
          completion.currentRaw = brightness->first;
          completion.maxRaw = brightness->second;
        }

        enqueueCompletion(std::move(completion));
      }
    }
  }

  void enqueueCompletion(WorkerCompletion completion) {
    {
      std::scoped_lock lock(workerMutex);
      completions.push(std::move(completion));
    }

    if (eventFd >= 0) {
      const std::uint64_t one = 1;
      const ssize_t ignored = ::write(eventFd, &one, sizeof(one));
      (void)ignored;
    }
  }

  void handlePollEvents() {
    if (epollFd >= 0) {
      epoll_event events[8];
      const int count = epoll_wait(epollFd, events, 8, 0);
      for (int i = 0; i < count; ++i) {
        if (events[i].data.fd == inotifyFd) {
          handleInotify();
        } else if (events[i].data.fd == eventFd) {
          drainWorkerEvent();
          processCompletions();
        }
      }
      return;
    }

    if (inotifyFd >= 0) {
      handleInotify();
    }
  }

  void drainWorkerEvent() {
    if (eventFd < 0) {
      return;
    }

    std::uint64_t value = 0;
    while (::read(eventFd, &value, sizeof(value)) > 0) {
    }
  }

  void processCompletions() {
    std::queue<WorkerCompletion> ready;
    {
      std::scoped_lock lock(workerMutex);
      std::swap(ready, completions);
    }

    bool changed = false;
    while (!ready.empty()) {
      changed = applyCompletion(ready.front()) || changed;
      ready.pop();
    }

    if (changed) {
      rebuildPublic();
      if (changeCallback) {
        changeCallback();
      }
    }
  }

  bool applyCompletion(const WorkerCompletion& completion) {
    if (completion.generation != generation) {
      return false;
    }

    switch (completion.type) {
    case WorkerCompletion::Type::Detect:
      return applyDetectCompletion(completion);
    case WorkerCompletion::Type::Refresh:
    case WorkerCompletion::Type::Set:
      return applyDdcUpdateCompletion(completion);
    }
    return false;
  }

  bool applyDetectCompletion(const WorkerCompletion& completion) {
    const auto oldPublic = publicDisplays;

    std::erase_if(internals, [](const DisplayInternal& display) { return display.backend == RuntimeBackend::Ddcutil; });

    for (const auto& candidate : completion.candidates) {
      const WaylandOutput* output = findOutputByConnector(wayland, candidate.connectorName);
      if (output == nullptr) {
        kLog.debug(
            "ddcutil: skipping bus {} because connector '{}' is not active", candidate.bus, candidate.connectorName
        );
        continue;
      }

      const BrightnessBackendPreference preference = backendPreferenceForOutput(activeConfig, output);
      if (preference == BrightnessBackendPreference::None || preference == BrightnessBackendPreference::Backlight) {
        continue;
      }

      const bool hasBacklight = std::ranges::any_of(internals, [&candidate](const DisplayInternal& display) {
        return display.backend == RuntimeBackend::Backlight && display.connectorName == candidate.connectorName;
      });
      if (hasBacklight && preference != BrightnessBackendPreference::Ddcutil) {
        continue;
      }

      DisplayInternal display;
      display.backend = RuntimeBackend::Ddcutil;
      display.connectorName = candidate.connectorName;
      display.ddcBus = candidate.bus;
      display.maxRaw = candidate.maxRaw;
      display.pub.id = candidate.connectorName;
      display.pub.brightness = normalizedBrightness(candidate.currentRaw, candidate.maxRaw);
      applyOutputMetadata(display.pub, *output);
      internals.push_back(std::move(display));

      kLog.info(
          "found ddcutil display connector={} bus={} current={:.0f}%", candidate.connectorName, candidate.bus,
          normalizedBrightness(candidate.currentRaw, candidate.maxRaw) * 100.0f
      );
    }

    rebuildPublic();
    return oldPublic != publicDisplays;
  }

  bool applyDdcUpdateCompletion(const WorkerCompletion& completion) {
    DisplayInternal* display = findInternal(completion.displayId);
    if (display == nullptr || display->backend != RuntimeBackend::Ddcutil) {
      return false;
    }

    if (completion.writeEpoch != display->ddcWriteEpoch) {
      kLog.debug(
          "ddcutil {} completion for '{}' ignored (stale epoch {} vs {})",
          completion.type == WorkerCompletion::Type::Set ? "write" : "refresh", completion.displayId,
          completion.writeEpoch, display->ddcWriteEpoch
      );
      return false;
    }

    const float oldBrightness = display->pub.brightness;
    if (completion.success && completion.maxRaw > 0 && completion.currentRaw >= 0) {
      display->failureCount = 0;
      display->quarantined = false;
      display->cooldownUntil = {};
      display->maxRaw = completion.maxRaw;
      display->pub.brightness = normalizedBrightness(completion.currentRaw, completion.maxRaw);
      syncPublicDisplay(*display);
      return std::abs(display->pub.brightness - oldBrightness) > 0.001f;
    }

    ++display->failureCount;
    if (display->failureCount >= kDdcFailureThreshold) {
      display->quarantined = true;
      display->cooldownUntil = std::chrono::steady_clock::now() + kDdcFailureCooldown;
      kLog.warn(
          "ddcutil {} failed for '{}' {} times; cooling down for {}s",
          completion.type == WorkerCompletion::Type::Set ? "write" : "refresh", display->pub.id, display->failureCount,
          kDdcFailureCooldown.count()
      );
    } else {
      kLog.warn(
          "ddcutil {} failed for '{}': {}", completion.type == WorkerCompletion::Type::Set ? "write" : "refresh",
          display->pub.id, StringUtils::trim(completion.detail)
      );
    }

    return false;
  }

  void handleInotify() {
    if (inotifyFd < 0) {
      return;
    }

    alignas(inotify_event) char buffer[4096];
    bool changed = false;

    while (true) {
      const ssize_t n = ::read(inotifyFd, buffer, sizeof(buffer));
      if (n <= 0) {
        break;
      }

      std::size_t offset = 0;
      while (offset < static_cast<std::size_t>(n)) {
        auto* event = reinterpret_cast<inotify_event*>(buffer + offset);
        offset += sizeof(inotify_event) + event->len;

        for (auto& display : internals) {
          if (display.backend != RuntimeBackend::Backlight || display.inotifyWd != event->wd) {
            continue;
          }

          const float newBrightness = readBacklightBrightness(display.sysfsPath, display.maxRaw);
          if (std::abs(newBrightness - display.pub.brightness) > 0.001f) {
            display.pub.brightness = newBrightness;
            syncPublicDisplay(display);
            changed = true;
          }
          break;
        }
      }
    }

    if (changed && changeCallback) {
      changeCallback();
    }
  }

  const BrightnessDisplay* findByOutput(wl_output* output) const {
    if (output == nullptr) {
      return nullptr;
    }

    const WaylandOutput* wlOutput = nullptr;
    for (const auto& candidate : wayland.outputs()) {
      if (candidate.output == output) {
        wlOutput = &candidate;
        break;
      }
    }
    if (wlOutput == nullptr) {
      return nullptr;
    }

    for (const auto& display : internals) {
      if (display.connectorName != wlOutput->connectorName) {
        continue;
      }
      for (const auto& pub : publicDisplays) {
        if (pub.id == display.pub.id) {
          return &pub;
        }
      }
    }

    return nullptr;
  }
};

BrightnessService::BrightnessService(SystemBus* bus, CompositorPlatform& platform, const BrightnessConfig& config)
    : m_impl(new Impl(bus, platform, config)) {
  m_impl->rebuildState(false);
}

BrightnessService::~BrightnessService() { delete m_impl; }

const std::vector<BrightnessDisplay>& BrightnessService::displays() const noexcept { return m_impl->publicDisplays; }

const BrightnessDisplay* BrightnessService::findDisplay(const std::string& id) const {
  for (const auto& display : m_impl->publicDisplays) {
    if (display.id == id) {
      return &display;
    }
  }
  return nullptr;
}

const BrightnessDisplay* BrightnessService::findByOutput(wl_output* output) const {
  return m_impl->findByOutput(output);
}

bool BrightnessService::available() const noexcept {
  return std::ranges::any_of(m_impl->publicDisplays, [](const BrightnessDisplay& display) {
    return display.controllable;
  });
}

void BrightnessService::setBrightness(const std::string& displayId, float value) {
  m_impl->setBrightness(displayId, value);
}

void BrightnessService::setAllBrightness(float value) { m_impl->setAllBrightness(value); }

void BrightnessService::requestDdcRefresh() { m_impl->queueDdcRefreshes(); }

void BrightnessService::reload(const BrightnessConfig& config) { m_impl->reload(config); }

void BrightnessService::onOutputsChanged() { m_impl->onOutputsChanged(); }

void BrightnessService::registerIpc(IpcService& ipc, std::function<void()> onBatchChange) {
  auto resolveTargets = [this](std::string_view token, std::vector<std::string>& ids, std::string& error) -> bool {
    if (!available()) {
      error = "error: brightness control unavailable\n";
      return false;
    }

    auto appendUnique = [&ids](const std::string& id) {
      if (!std::ranges::contains(ids, id)) {
        ids.push_back(id);
      }
    };

    if (token.empty() || token == "current") {
      wl_output* output = m_impl->wayland.activeToplevelOutput();
      if (output == nullptr) {
        output = m_impl->platform.preferredInteractiveOutput();
      }
      if (output == nullptr) {
        error = "error: could not resolve the current output\n";
        return false;
      }
      const auto* display = findByOutput(output);
      if (display == nullptr) {
        error = "error: current output has no brightness control\n";
        return false;
      }
      appendUnique(display->id);
      return true;
    }

    if (token == "all" || token == "*") {
      for (const auto& display : displays()) {
        if (display.controllable) {
          appendUnique(display.id);
        }
      }
      return !ids.empty();
    }

    if (const auto* display = findDisplay(std::string(token)); display != nullptr) {
      if (!display->controllable) {
        error = "error: brightness target '" + std::string(token) + "' has no brightness control\n";
        return false;
      }
      appendUnique(display->id);
      return true;
    }

    for (const auto& output : m_impl->wayland.outputs()) {
      if (!outputMatchesSelector(std::string(token), output)) {
        continue;
      }
      if (const auto* display = findByOutput(output.output); display != nullptr) {
        if (!display->controllable) {
          continue;
        }
        appendUnique(display->id);
      }
    }

    if (!ids.empty()) {
      return true;
    }

    error = "error: unknown brightness target '" + std::string(token) + "'";
    if (available()) {
      error += " (available: " + joinBrightnessDisplayIds(*this) + ")";
    }
    error += "\n";
    return false;
  };

  auto applyToTargets = [this, resolveTargets, onBatchChange](const std::string& target, auto&& apply) -> std::string {
    std::vector<std::string> ids;
    std::string error;
    if (!resolveTargets(target, ids, error)) {
      return error;
    }

    if (ids.size() > 1 && onBatchChange) {
      onBatchChange();
    }

    for (const auto& id : ids) {
      const auto* display = findDisplay(id);
      if (display == nullptr || !display->controllable) {
        continue;
      }
      apply(*display);
    }
    return "ok\n";
  };

  ipc.registerHandler(
      "brightness-set",
      [this, applyToTargets](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.empty() || parts.size() > 2) {
          return "error: brightness-set requires <value> or <target> <value>\n";
        }

        std::string target = "current";
        std::string valueToken = parts[0];
        if (parts.size() == 2) {
          target = parts[0];
          valueToken = parts[1];
        }

        const auto amount = noctalia::ipc::parseNormalizedOrPercent(valueToken);
        if (!amount.has_value()) {
          return "error: invalid brightness value (use percent like 65 or 65%, or normalized like 0.65)\n";
        }

        return applyToTargets(target, [this, amount](const BrightnessDisplay& display) {
          setBrightness(display.id, *amount);
        });
      },
      "brightness-set <value> | brightness-set <current|*|all|monitor-selector> <value>",
      "Set brightness (defaults to current monitor)"
  );

  auto registerDeltaHandler =
      [this, &ipc,
       applyToTargets](const std::string& command, float direction, std::string usage, std::string description) {
        ipc.registerHandler(
            command,
            [this, applyToTargets, command, direction](const std::string& args) -> std::string {
              const auto parts = noctalia::ipc::splitWords(args);
              if (parts.size() > 2) {
                return "error: " + command + " accepts at most [target] [step]\n";
              }

              std::string target = "current";
              std::optional<float> step = kDefaultBrightnessStep;
              if (parts.size() == 1) {
                const auto maybeStep = noctalia::ipc::parseNormalizedOrPercent(parts[0]);
                if (maybeStep.has_value()) {
                  step = maybeStep;
                } else {
                  target = parts[0];
                }
              } else if (parts.size() == 2) {
                target = parts[0];
                step = noctalia::ipc::parseNormalizedOrPercent(parts[1]);
              }

              if (!step.has_value()) {
                return "error: invalid brightness step (use percent like 5 or 5%, or normalized like 0.05)\n";
              }

              return applyToTargets(target, [this, step, direction](const BrightnessDisplay& display) {
                setBrightness(display.id, display.brightness + direction * *step);
              });
            },
            std::move(usage), std::move(description)
        );
      };

  registerDeltaHandler(
      "brightness-up", 1.0f, "brightness-up [current|*|all|monitor-selector] [step]",
      "Increase brightness (defaults to current monitor)"
  );
  registerDeltaHandler(
      "brightness-down", -1.0f, "brightness-down [current|*|all|monitor-selector] [step]",
      "Decrease brightness (defaults to current monitor)"
  );

  ipc.registerHandler(
      "brightness-list-backlight-devices",
      [](const std::string& /*args*/) -> std::string {
        const std::string backlightDir = "/sys/class/backlight";
        DIR* dir = ::opendir(backlightDir.c_str());
        if (dir == nullptr) {
          return "error: no backlight devices available\n";
        }
        std::string result;
        while (auto* entry = ::readdir(dir)) {
          const std::string name = entry->d_name;
          if (name == "." || name == "..") {
            continue;
          }
          result += name + "\n";
        }
        ::closedir(dir);
        return result.empty() ? "error: no backlight devices available\n" : result;
      },
      "brightness-list-backlight-devices", "List available sysfs backlight device names"
  );
}

void BrightnessService::setChangeCallback(ChangeCallback callback) { m_impl->changeCallback = std::move(callback); }

int BrightnessService::watchFd() const noexcept { return m_impl->epollFd >= 0 ? m_impl->epollFd : m_impl->inotifyFd; }

void BrightnessService::dispatchWatch() { m_impl->handlePollEvents(); }
