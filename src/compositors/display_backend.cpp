#include "compositors/display_backend.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_runtime.h"
#include "compositors/sway/sway_runtime.h"
#include "core/process/process.h"

#include <cmath>
#include <cstdio>

namespace compositors::display {

  namespace {

    [[nodiscard]] bool wlrRandrWorks() {
      if (!process::commandExists("wlr-randr")) {
        return false;
      }
      return static_cast<bool>(process::runSync({"wlr-randr", "--json"}));
    }

  } // namespace

  DisplayBackend::DisplayBackend(DisplayBackendSpec spec)
      : m_spec(std::move(spec)), m_fetchArgs(m_spec.fetchArgs ? m_spec.fetchArgs() : std::vector<std::string>{}) {}

  std::unique_ptr<DisplayBackend> createDisplayBackend(CompositorRuntimeRegistry& runtimeRegistry) {
    switch (compositors::detect()) {
    case compositors::CompositorKind::Hyprland:
      if (process::commandExists("hyprctl")) {
        return std::make_unique<DisplayBackend>(hyprlandSpec());
      }
      break;
    case compositors::CompositorKind::Niri:
      if (process::commandExists("niri")) {
        return std::make_unique<DisplayBackend>(niriSpec());
      }
      break;
    case compositors::CompositorKind::Sway:
      if (runtimeRegistry.sway().hasOutputCommand()) {
        return std::make_unique<DisplayBackend>(swaySpec(runtimeRegistry.sway()));
      }
      break;
    case compositors::CompositorKind::Mango:
      return std::make_unique<DisplayBackend>(mangoSpec(runtimeRegistry.mango()));
    case compositors::CompositorKind::Labwc:
    case compositors::CompositorKind::Triad:
    case compositors::CompositorKind::Dwl:
    case compositors::CompositorKind::Kde:
    case compositors::CompositorKind::Umbriel:
    case compositors::CompositorKind::Unknown:
      break;
    }
    if (wlrRandrWorks()) {
      return std::make_unique<DisplayBackend>(wlrSpec(false));
    }
    return std::make_unique<DisplayBackend>(wlrSpec(true));
  }

  std::string_view transformToName(std::string_view transform) {
    for (const auto& [canonical, name] : kTransforms) {
      if (canonical == transform) {
        return name;
      }
    }
    return "normal";
  }

  std::string transformFromName(std::string_view name) {
    for (const auto& [canonical, backendName] : kTransforms) {
      if (backendName == name) {
        return std::string(canonical);
      }
    }
    return "Normal";
  }

  std::string transformFromCode(int code) {
    if (code < 0 || static_cast<std::size_t>(code) >= kTransforms.size()) {
      return "Normal";
    }
    return std::string(kTransforms[static_cast<std::size_t>(code)].first);
  }

  int transformToCode(std::string_view transform) {
    for (std::size_t i = 0; i < kTransforms.size(); ++i) {
      if (kTransforms[i].first == transform) {
        return static_cast<int>(i);
      }
    }
    return 0;
  }

  OutputDiff diffOutputs(const OutputState& snapshot, const OutputState& current) {
    OutputDiff diff;
    diff.enabled = snapshot.enabled != current.enabled;
    diff.mode = !snapshot.modeStr.empty() && snapshot.modeStr != current.modeStr;
    diff.scale = std::abs(snapshot.scale - current.scale) > 0.01;
    diff.transform = snapshot.transform != current.transform;
    diff.position = snapshot.x != current.x || snapshot.y != current.y;
    diff.vrr = snapshot.vrr != current.vrr;
    diff.hdr = snapshot.hdr != current.hdr;
    return diff;
  }

  std::string swayModeStr(std::string_view modeStr) {
    const std::string raw(modeStr);
    if (raw.empty()) {
      return "preferred";
    }
    const auto at = raw.find('@');
    if (at == std::string::npos) {
      return raw;
    }
    const char* begin = raw.data() + at + 1;
    char* end = nullptr;
    const double hz = std::strtod(begin, &end);
    if (end == begin || hz <= 0.0) {
      return raw;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", hz);
    return raw.substr(0, at + 1) + buffer + "Hz";
  }

  std::string formatRateHz(int refreshMhz) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", refreshMhz / 1000.0);
    return buffer;
  }

  bool rotatedTransform(std::string_view transform) {
    return transform == "90" || transform == "270" || transform == "Flipped90" || transform == "Flipped270";
  }

} // namespace compositors::display
