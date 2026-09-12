#include "shell/settings/display/display_service.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "shell/settings/display/display_layout.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

extern "C" {
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

namespace settings::display {

  namespace {

    constexpr Logger kLog("display");
    constexpr int kRevertTimeoutSeconds = 15;

    using namespace std::chrono_literals;

    [[nodiscard]] std::string joinArgs(const std::vector<std::string>& args) {
      std::string joined;
      for (const auto& arg : args) {
        if (!joined.empty()) {
          joined.push_back(' ');
        }
        joined += arg;
      }
      return joined;
    }

    [[nodiscard]] std::string hexEncode(const std::vector<std::uint8_t>& bytes) {
      static constexpr char kDigits[] = "0123456789abcdef";
      std::string hex;
      hex.reserve(bytes.size() * 2);
      for (const auto byte : bytes) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
      }
      return hex;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> readEdidBytes(const std::string& outputName) {
      const std::filesystem::path drmDir("/sys/class/drm");
      std::error_code ec;
      if (!std::filesystem::is_directory(drmDir, ec)) {
        return std::nullopt;
      }
      for (const auto& entry : std::filesystem::directory_iterator(drmDir, ec)) {
        const auto filename = entry.path().filename().string();
        const auto dash = filename.find('-');
        if (!filename.starts_with("card") || dash == std::string::npos || filename.substr(dash + 1) != outputName) {
          continue;
        }
        std::ifstream file(entry.path() / "edid", std::ios::binary);
        if (!file.is_open()) {
          return std::nullopt;
        }
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      }
      return std::nullopt;
    }

    [[nodiscard]] bool edidSupportsHdr(const std::string& outputName) {
      const auto bytes = readEdidBytes(outputName);
      if (!bytes.has_value() || bytes->empty()) {
        return false;
      }
      struct di_info* info = di_info_parse_edid(bytes->data(), bytes->size());
      if (info == nullptr) {
        return false;
      }
      const bool supports = di_info_get_hdr_static_metadata(info) != nullptr;
      di_info_destroy(info);
      return supports;
    }

  } // namespace

  DisplayService::DisplayService(
      std::unique_ptr<compositors::display::DisplayBackend> backend, WaylandConnection* wayland
  )
      : m_backend(std::move(backend)), m_wayland(wayland) {}

  void DisplayService::stampOutputMetadata(std::vector<compositors::display::OutputState>& outputs) {
    if (m_wayland == nullptr) {
      return;
    }
    for (auto& output : outputs) {
      for (const auto& wlOutput : m_wayland->outputs()) {
        if (wlOutput.connectorName != output.name) {
          continue;
        }
        if (output.make.empty()) {
          output.make = wlOutput.make;
        }
        if (output.model.empty()) {
          output.model = wlOutput.model;
        }
        break;
      }
    }
  }

  bool DisplayService::writable() const { return m_backend != nullptr && m_backend->writable(); }

  bool DisplayService::supportsHdr() const { return m_backend != nullptr && m_backend->supportsHdr(); }

  std::string DisplayService::backendKind() const {
    return m_backend != nullptr ? m_backend->kindName() : std::string{};
  }

  DisplayService::TargetMap DisplayService::currentSnapshot() const {
    TargetMap snapshot;
    for (const auto& out : m_outputs) {
      compositors::display::OutputState state;
      state.name = out.name;
      state.enabled = out.enabled;
      state.make = out.make;
      state.model = out.model;
      state.scale = out.scale;
      state.transform = out.transform;
      state.x = out.x;
      state.y = out.y;
      state.vrr = out.vrr;
      state.hdr = out.hdr;
      state.hdrSupported = out.hdrSupported;
      if (!out.modes.empty()) {
        const auto idx = std::clamp(out.currentModeIndex, 0, static_cast<int>(out.modes.size()) - 1);
        const auto& mode = out.modes[static_cast<std::size_t>(idx)];
        state.modeStr = std::to_string(mode.width)
            + "x"
            + std::to_string(mode.height)
            + "@"
            + compositors::display::formatRateHz(mode.refreshMhz);
      }
      state.modes = out.modes;
      state.currentModeIndex = out.currentModeIndex;
      snapshot[out.name] = std::move(state);
    }
    return snapshot;
  }

  int DisplayService::enabledOutputCount(const TargetMap& config) const {
    int count = 0;
    for (const auto& [name, cfg] : config) {
      if (cfg.enabled) {
        count++;
      }
    }
    return count;
  }

  bool DisplayService::hasChangesComparedToCurrent() const {
    const auto current = currentSnapshot();
    for (const auto& [name, cfg] : m_target) {
      const auto it = current.find(name);
      if (it == current.end()) {
        continue;
      }
      const auto& sc = it->second;
      if (sc.enabled != cfg.enabled)
        return true;
      if (sc.modeStr != cfg.modeStr)
        return true;
      if (std::abs(sc.scale - cfg.scale) > 0.01)
        return true;
      if (sc.transform != cfg.transform)
        return true;
      if (sc.x != cfg.x || sc.y != cfg.y)
        return true;
      if (sc.vrr != cfg.vrr)
        return true;
    }
    return false;
  }

  void DisplayService::startConfirmation(const TargetMap& snapshot) {
    if (!m_awaitingConfirmation) {
      m_pendingSnapshot = snapshot;
      m_revertCountdown = kRevertTimeoutSeconds;
      m_awaitingConfirmation = true;
    }
  }

  void DisplayService::stampHdrSupport(std::vector<compositors::display::OutputState>& outputs) {
    for (auto& output : outputs) {
      output.hdrSupported = edidSupportsHdr(output.name);
    }
  }

  void DisplayService::notifyStateChanged() {
    if (m_onStateChanged) {
      m_onStateChanged();
    }
  }

  void DisplayService::fetchAsync() {
    if (m_backend == nullptr) {
      return;
    }
    const auto args = m_backend->fetchArgs();
    if (args.empty()) {
      m_loading = false;
      std::string error;
      m_outputs = m_backend->parseFetch({}, error);
      stampHdrSupport(m_outputs);
      stampOutputMetadata(m_outputs);
      if (!error.empty()) {
        m_lastError = error;
        kLog.warn("display fetch failed: {}", error);
      }
      m_target = currentSnapshot();
      notifyStateChanged();
      return;
    }
    m_loading = true;
    const std::weak_ptr<int> lifetime = m_lifetime;
    const std::shared_ptr<compositors::display::DisplayBackend> backend = m_backend;
    (void)process::runAsync(args, process::RunCallbacks{.onExit = [this, lifetime, backend](process::RunResult result) {
                              if (lifetime.expired()) {
                                return;
                              }
                              std::string error;
                              auto outputs = result ? backend->parseFetch(result.out, error)
                                                    : std::vector<compositors::display::OutputState>{};
                              stampHdrSupport(outputs);
                              if (!result) {
                                error = result.err.empty() ? "display fetch failed" : StringUtils::trim(result.err);
                              }
                              DeferredCall::callLater([this, lifetime, backend, outputs = std::move(outputs),
                                                       error = std::move(error)]() mutable {
                                if (lifetime.expired()) {
                                  return;
                                }
                                m_loading = false;
                                if (!error.empty()) {
                                  m_lastError = error;
                                  kLog.warn("display fetch failed: {}", error);
                                  notifyStateChanged();
                                  return;
                                }
                                stampOutputMetadata(outputs);
                                m_outputs = std::move(outputs);
                                m_lastError.clear();
                                m_target = currentSnapshot();
                                notifyStateChanged();
                              });
                            }});
  }

  void DisplayService::enqueue(compositors::display::DisplayCommand command, const std::optional<TargetMap>& snapshot) {
    m_queue.push_back(std::move(command));
    if (snapshot.has_value()) {
      startConfirmation(*snapshot);
    }
    drainNext();
  }

  void DisplayService::enqueueAll(
      const std::vector<compositors::display::DisplayCommand>& commands, const std::optional<TargetMap>& snapshot
  ) {
    for (const auto& command : commands) {
      enqueue(command, snapshot);
    }
  }

  void DisplayService::drainNext() {
    if (m_busy || m_queue.empty()) {
      return;
    }
    auto command = std::move(m_queue.front());
    m_queue.pop_front();

    if (command.sleepMs.has_value()) {
      m_sleepTimer.start(std::chrono::milliseconds(*command.sleepMs), [this]() { drainNext(); });
      return;
    }
    if (command.action) {
      command.action();
      m_finishTimer.start(50ms, [this]() {
        if (!m_queue.empty()) {
          drainNext();
        } else {
          m_refreshTimer.start(300ms, [this]() { fetchAsync(); });
        }
      });
      return;
    }
    if (command.args.empty()) {
      drainNext();
      return;
    }

    m_busy = true;
    kLog.debug("apply: {}", joinArgs(command.args));
    const std::weak_ptr<int> lifetime = m_lifetime;
    (void)process::runAsync(
        command.args, process::RunCallbacks{.stdErr = nullptr, .onExit = [this, lifetime](process::RunResult result) {
                                              DeferredCall::callLater([this, lifetime, result]() {
                                                if (lifetime.expired()) {
                                                  return;
                                                }
                                                m_busy = false;
                                                if (!StringUtils::trim(result.err).empty()) {
                                                  kLog.warn("apply error: {}", StringUtils::trim(result.err));
                                                  m_lastError = StringUtils::trim(result.err);
                                                  m_queue.clear();
                                                }
                                                m_finishTimer.start(50ms, [this]() {
                                                  if (!m_queue.empty()) {
                                                    drainNext();
                                                  } else {
                                                    m_refreshTimer.start(300ms, [this]() { fetchAsync(); });
                                                  }
                                                });
                                              });
                                            }}
    );
  }

  void DisplayService::applyTopologyChange(
      const std::optional<TargetMap>& snapshot, std::vector<compositors::display::DisplayCommand> commands
  ) {
    if (!hasChangesComparedToCurrent()) {
      return;
    }
    if (backendKind() != "wlroots") {
      const auto positionCommands = m_backend->positionsCommands(m_target);
      for (const auto& positionCommand : positionCommands) {
        const auto joined = joinArgs(positionCommand.args);
        const auto duplicate =
            std::ranges::any_of(commands, [&joined](const auto& command) { return joinArgs(command.args) == joined; });
        if (!duplicate) {
          commands.push_back(positionCommand);
        }
      }
    }
    enqueueAll(commands, snapshot);
  }

  void DisplayService::setMode(const std::string& outputName, const std::string& modeStr) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.modeStr = modeStr;
    normalizeLayout(m_outputs, m_target);
    applyTopologyChange(snapshot, m_backend->applyCommands(m_target));
    notifyStateChanged();
  }

  void DisplayService::setScale(const std::string& outputName, double scale) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.scale = scale;
    normalizeLayout(m_outputs, m_target);
    applyTopologyChange(snapshot, m_backend->applyCommands(m_target));
    notifyStateChanged();
  }

  void DisplayService::setTransform(const std::string& outputName, const std::string& transform) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.transform = transform;
    normalizeLayout(m_outputs, m_target);
    applyTopologyChange(snapshot, m_backend->applyCommands(m_target));
    notifyStateChanged();
  }

  void DisplayService::setVrr(const std::string& outputName, bool enabled) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.vrr = enabled;
    enqueueAll(m_backend->applyCommands(m_target), snapshot);
    notifyStateChanged();
  }

  void DisplayService::setHdr(const std::string& outputName, bool enabled) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.hdr = enabled;
    enqueueAll(m_backend->applyCommands(m_target), snapshot);
    notifyStateChanged();
  }

  void DisplayService::toggleOutput(const std::string& outputName, bool enabled) {
    const auto it = m_target.find(outputName);
    if (it == m_target.end()) {
      return;
    }
    if (!enabled && it->second.enabled && enabledOutputCount(m_target) <= 1) {
      return;
    }
    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    it->second.enabled = enabled;
    normalizeLayout(m_outputs, m_target);
    applyTopologyChange(snapshot, m_backend->applyCommands(m_target));
    notifyStateChanged();
  }

  void DisplayService::setPosition(const std::string& outputName, int x, int y) {
    if (enabledOutputCount(m_target) <= 1) {
      return;
    }
    const auto sizes = predictedSizes(m_outputs, m_target);
    std::map<std::string, std::pair<int, int>> positions;
    for (const auto& [name, cfg] : m_target) {
      if (name == outputName) {
        positions[name] = {x, y};
      } else {
        positions[name] = {cfg.x, cfg.y};
      }
    }

    const auto clampRange = [](int desired, int otherPos, int otherSize, int dragSize) {
      return std::max(otherPos - dragSize + 1, std::min(desired, otherPos + otherSize - 1));
    };
    const auto dpIt = positions.find(outputName);
    if (dpIt != positions.end()) {
      const auto& [dx, dy] = dpIt->second;
      const auto& ds = sizes.at(outputName);
      bool touching = false;
      for (const auto& [name, pos] : positions) {
        if (name == outputName || !m_target.at(name).enabled) {
          continue;
        }
        const auto& [ox, oy] = pos;
        const auto& os = sizes.at(name);
        if (isTouching(dx, dy, ds.w, ds.h, ox, oy, os.w, os.h)) {
          touching = true;
          break;
        }
      }
      if (!touching) {
        double bestDist = std::numeric_limits<double>::max();
        int bestX = dx;
        int bestY = dy;
        for (const auto& [name, pos] : positions) {
          if (name == outputName || !m_target.at(name).enabled) {
            continue;
          }
          const auto& [ox, oy] = pos;
          const auto& os = sizes.at(name);
          const int candidates[4][2] = {
              {ox + os.w, clampRange(dy, oy, os.h, ds.h)},
              {ox - ds.w, clampRange(dy, oy, os.h, ds.h)},
              {clampRange(dx, ox, os.w, ds.w), oy + os.h},
              {clampRange(dx, ox, os.w, ds.w), oy - ds.h},
          };
          for (const auto& candidate : candidates) {
            const double dist = std::pow(candidate[0] - dx, 2.0) + std::pow(candidate[1] - dy, 2.0);
            if (dist < bestDist) {
              bestDist = dist;
              bestX = candidate[0];
              bestY = candidate[1];
            }
          }
        }
        positions[outputName] = {bestX, bestY};
      }
    }

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto& [name, pos] : positions) {
      minX = std::min(minX, pos.first);
      minY = std::min(minY, pos.second);
    }
    for (auto& [name, cfg] : m_target) {
      if (const auto posIt = positions.find(name); posIt != positions.end()) {
        cfg.x = posIt->second.first - minX;
        cfg.y = posIt->second.second - minY;
      }
    }

    if (!hasChangesComparedToCurrent()) {
      return;
    }

    const auto snapshot =
        m_pendingSnapshot.has_value() ? m_pendingSnapshot : std::optional<TargetMap>(currentSnapshot());
    const auto commands = m_backend->positionsCommands(m_target);
    enqueueAll(commands, snapshot);
    if (backendKind() == "hyprland" || backendKind() == "wlroots") {
      enqueue(compositors::display::DisplayCommand{{}, 250}, std::nullopt);
      enqueueAll(commands, std::nullopt);
    }
    notifyStateChanged();
  }

  void DisplayService::keepChanges() {
    m_awaitingConfirmation = false;
    m_pendingSnapshot.reset();
    m_revertCountdown = 0;
    notifyStateChanged();
  }

  void DisplayService::revertChanges() {
    m_awaitingConfirmation = false;
    m_revertCountdown = 0;
    const auto snapshot = m_pendingSnapshot;
    m_pendingSnapshot.reset();
    if (!snapshot.has_value()) {
      return;
    }
    m_queue.clear();
    const auto current = currentSnapshot();
    enqueueAll(m_backend->revertCommands(*snapshot, current), std::nullopt);
    notifyStateChanged();
  }

  void DisplayService::onSecondTick() {
    if (!m_awaitingConfirmation) {
      return;
    }
    m_revertCountdown--;
    notifyStateChanged();
    if (m_revertCountdown <= 0) {
      revertChanges();
    }
  }

  void DisplayService::readEdid(const std::string& outputName) {
    m_edidHex.clear();
    m_edidSummary = EdidSummary{};
    m_edidSummary.output = outputName;

    const auto bytes = readEdidBytes(outputName);
    if (!bytes.has_value() || bytes->empty()) {
      m_edidSummary.status = EdidStatus::ReadError;
      notifyStateChanged();
      return;
    }
    m_edidHex = hexEncode(*bytes);
    if (m_edidHex.empty()) {
      m_edidSummary.status = EdidStatus::DecodedEmpty;
      notifyStateChanged();
      return;
    }

    struct di_info* info = di_info_parse_edid(bytes->data(), bytes->size());
    if (info == nullptr) {
      m_edidSummary.status = EdidStatus::DecodeError;
      notifyStateChanged();
      return;
    }

    EdidSummary summary;
    summary.output = outputName;
    summary.status = EdidStatus::Decoded;
    if (const char* model = di_info_get_model(info); model != nullptr) {
      summary.monitorName = model;
    }
    if (const char* make = di_info_get_make(info); make != nullptr) {
      summary.manufacturerId = make;
    }
    if (const char* serial = di_info_get_serial(info); serial != nullptr) {
      summary.serialText = serial;
    }

    const struct di_edid* edid = di_info_get_edid(info);
    if (const auto* vendorProduct = di_edid_get_vendor_product(edid); vendorProduct != nullptr) {
      if (vendorProduct->product != 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "0x%04x", vendorProduct->product);
        summary.productCode = buffer;
      }
      if (vendorProduct->serial != 0) {
        summary.serialNumber = std::to_string(vendorProduct->serial);
      }
      summary.week = vendorProduct->manufacture_week;
      summary.year = vendorProduct->manufacture_year;
    }
    const int versionMajor = di_edid_get_version(edid);
    const int versionMinor = di_edid_get_revision(edid);
    summary.version = std::to_string(versionMajor) + "." + std::to_string(versionMinor);
    if (di_edid_get_video_input_digital(edid) != nullptr) {
      summary.inputType = "Digital";
    } else if (di_edid_get_video_input_analog(edid) != nullptr) {
      summary.inputType = "Analog";
    }
    if (const auto* screenSize = di_edid_get_screen_size(edid); screenSize != nullptr) {
      summary.sizeWidthCm = screenSize->width_cm;
      summary.sizeHeightCm = screenSize->height_cm;
    }
    if (const auto* const* timings = di_edid_get_detailed_timing_defs(edid);
        timings != nullptr && timings[0] != nullptr) {
      const auto* timing = timings[0];
      const int hTotal = timing->horiz_video + timing->horiz_blank;
      const int vTotal = timing->vert_video + timing->vert_blank;
      if (hTotal > 0 && vTotal > 0 && timing->pixel_clock_hz > 0) {
        const double refresh = static_cast<double>(timing->pixel_clock_hz) / (hTotal * vTotal);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%dx%d @ %.2f Hz", timing->horiz_video, timing->vert_video, refresh);
        summary.preferredMode = buffer;
      }
    }
    di_info_destroy(info);

    m_edidSummary = std::move(summary);
    notifyStateChanged();
  }

} // namespace settings::display
