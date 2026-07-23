#include "shell/wallpaper/wallpaper.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/random.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ipc/ipc_service.h"
#include "render/animation/animation.h"
#include "render/backend/render_backend.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "shell/wallpaper/wallpaper_instance.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "theme/theme_service.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using Random::randomFloat;

namespace {

  constexpr Easing kWallpaperTransitionEasing = Easing::EaseInOutCubic;

  [[nodiscard]] Color defaultWallpaperColor() { return rgba(0.0f, 0.0f, 0.0f, 1.0f); }

  [[nodiscard]] float transitionProgressForTime(float time) {
    return applyEasing(kWallpaperTransitionEasing, std::clamp(time, 0.0f, 1.0f));
  }

  void setTransitionTime(WallpaperInstance& instance, float time) {
    instance.transitionTime = std::clamp(time, 0.0f, 1.0f);
  }

  [[nodiscard]] float transitionTargetTime(WallpaperTransitionDirection direction) {
    return direction == WallpaperTransitionDirection::Forward ? 1.0f : 0.0f;
  }

  [[nodiscard]] std::optional<WallpaperTransitionDirection>
  directionForPath(const WallpaperInstance& instance, const std::string& path) {
    if (path == instance.currentPath) {
      return WallpaperTransitionDirection::Reverse;
    }

    if (path == instance.pendingPath) {
      return WallpaperTransitionDirection::Forward;
    }

    return std::nullopt;
  }

  TransitionParams randomizeParams(WallpaperTransition type, float smoothness, float aspectRatio) {
    TransitionParams params;
    params.smoothness = smoothness;
    params.aspectRatio = aspectRatio;

    switch (type) {
    case WallpaperTransition::Wipe:
      params.direction = std::floor(randomFloat(0.0f, 4.0f));
      break;
    case WallpaperTransition::Disc:
      params.centerX = randomFloat(0.2f, 0.8f);
      params.centerY = randomFloat(0.2f, 0.8f);
      break;
    case WallpaperTransition::Stripes:
      params.stripeCount = std::round(randomFloat(4.0f, 24.0f));
      params.angle = randomFloat(0.0f, 360.0f);
      break;
    case WallpaperTransition::Zoom:
      break;
    case WallpaperTransition::Honeycomb:
      params.cellSize = randomFloat(0.02f, 0.06f);
      params.centerX = randomFloat(0.2f, 0.8f);
      params.centerY = randomFloat(0.2f, 0.8f);
      break;
    case WallpaperTransition::Fade:
    default:
      break;
    }

    return params;
  }

  bool hasImageExtension(const std::filesystem::path& path) {
    const std::string ext = StringUtils::toLower(path.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
  }

  [[nodiscard]] std::optional<std::string>
  resolveWallpaperPath(std::string_view path, std::optional<std::string_view> callerCwd = std::nullopt) {
    if (path.empty()) {
      return std::nullopt;
    }
    if (path.starts_with("color:")) {
      return std::string(path);
    }
    const std::filesystem::path resolved = FileUtils::resolvePath(path, callerCwd);
    std::error_code ec;
    if (std::filesystem::is_regular_file(resolved, ec)) {
      return resolved.string();
    }
    return std::nullopt;
  }

  struct WallpaperSetParsed {
    std::optional<std::string> connector;
    std::string path;
  };

  template <typename IsConnectorFn>
  [[nodiscard]] WallpaperSetParsed parseWallpaperSetTokens(
      const std::vector<std::string>& tokens, IsConnectorFn&& isConnector, std::optional<std::string_view> callerCwd
  ) {
    if (tokens.size() == 1) {
      return {.connector = std::nullopt, .path = tokens[0]};
    }

    const std::string allJoined = StringUtils::join(tokens, " ");
    if (resolveWallpaperPath(allJoined, callerCwd).has_value()) {
      return {.connector = std::nullopt, .path = allJoined};
    }

    if (isConnector(tokens[0])) {
      return {
          .connector = tokens[0],
          .path = StringUtils::join(std::vector<std::string>(tokens.begin() + 1, tokens.end()), " "),
      };
    }

    return {.connector = std::nullopt, .path = allJoined};
  }

  void
  collectWallpaperCandidates(const std::filesystem::path& directory, bool recursive, std::vector<std::string>& out) {
    out.clear();
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
      return;
    }

    if (recursive) {
      for (auto it = std::filesystem::recursive_directory_iterator(
               directory, std::filesystem::directory_options::skip_permission_denied, ec
           );
           !ec && it != std::filesystem::end(it); it.increment(ec)) {
        if (ec) {
          break;
        }
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) {
          continue;
        }
        if (hasImageExtension(it->path())) {
          out.push_back(it->path().string());
        }
      }
      return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, ec
         )) {
      if (ec) {
        break;
      }
      std::error_code typeEc;
      if (!entry.is_regular_file(typeEc) || typeEc) {
        continue;
      }
      if (hasImageExtension(entry.path())) {
        out.push_back(entry.path().string());
      }
    }
  }

  std::string pickRandomWallpaperPath(const std::vector<std::string>& candidates, const std::string& currentPath) {
    if (candidates.empty()) {
      return {};
    }
    if (candidates.size() == 1) {
      return candidates.front();
    }

    const std::size_t start = std::min<std::size_t>(
        static_cast<std::size_t>(std::floor(randomFloat(0.0f, static_cast<float>(candidates.size())))),
        candidates.size() - 1
    );
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const std::string& candidate = candidates[(start + i) % candidates.size()];
      if (candidate != currentPath) {
        return candidate;
      }
    }
    return candidates.front();
  }

  bool lessCaseInsensitive(std::string_view a, std::string_view b) {
    return StringUtils::naturalCaseInsensitiveLess(a, b);
  }

  std::string pickAlphabeticalWallpaperPath(
      std::vector<std::string> candidates, const std::string& currentPath, int direction = 1
  ) {
    if (candidates.empty()) {
      return {};
    }
    std::ranges::sort(candidates, [](const std::string& a, const std::string& b) { return lessCaseInsensitive(a, b); });
    if (candidates.size() == 1) {
      return candidates.front();
    }

    const auto it = std::ranges::find(candidates, currentPath);
    if (it == candidates.end()) {
      return candidates.front();
    }
    const auto n = candidates.size();
    const auto idx = static_cast<std::size_t>(std::distance(candidates.begin(), it));
    const std::size_t step = (direction < 0) ? n - 1 : 1;
    return candidates[(idx + step) % n];
  }

  std::string pickAutomationWallpaperPath(
      const WallpaperAutomationConfig& automation, std::vector<std::string> candidates, const std::string& currentPath
  ) {
    return automation.order == WallpaperAutomationConfig::Order::Alphabetical
        ? pickAlphabeticalWallpaperPath(std::move(candidates), currentPath)
        : pickRandomWallpaperPath(candidates, currentPath);
  }

  bool wallpaperOutputEnabled(const WallpaperConfig& config, const WaylandOutput& output) {
    if (const auto* ovr = wallpaper::findWallpaperMonitorOverride(config, output); ovr != nullptr && ovr->enabled) {
      return *ovr->enabled;
    }
    return true;
  }

  constexpr Logger kLog("wallpaper");

  Color resolveWallpaperFillColor(const WallpaperConfig& config, const WaylandOutput& output) {
    const ColorSpec* fillColor = nullptr;
    if (const auto* ovr = wallpaper::findWallpaperMonitorOverride(config, output); ovr != nullptr && ovr->fillColor) {
      fillColor = &*ovr->fillColor;
    } else if (config.fillColor) {
      fillColor = &*config.fillColor;
    }

    if (fillColor == nullptr) {
      return rgba(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return resolveColorSpec(*fillColor);
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  // Build the Span geometry for one output: the desktop bounding box across every
  // ready output and this output's offset/size within it. Returns a zeroed result
  // (which makes the shader fall back to Crop) when geometry is not yet available.
  WallpaperSpanParams computeSpanParams(const std::vector<WaylandOutput>& outputs, std::uint32_t outputName) {
    WallpaperSpanParams span;

    bool haveBounds = false;
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t maxX = 0;
    std::int32_t maxY = 0;
    const WaylandOutput* self = nullptr;

    for (const auto& out : outputs) {
      if (!out.done || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
        continue;
      }
      const std::int32_t left = out.logicalX;
      const std::int32_t top = out.logicalY;
      const std::int32_t right = out.logicalX + out.logicalWidth;
      const std::int32_t bottom = out.logicalY + out.logicalHeight;
      if (!haveBounds) {
        minX = left;
        minY = top;
        maxX = right;
        maxY = bottom;
        haveBounds = true;
      } else {
        minX = std::min(minX, left);
        minY = std::min(minY, top);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
      }
      if (out.name == outputName) {
        self = &out;
      }
    }

    if (!haveBounds || self == nullptr) {
      return span;
    }

    span.offsetX = static_cast<float>(self->logicalX - minX);
    span.offsetY = static_cast<float>(self->logicalY - minY);
    span.monitorWidth = static_cast<float>(self->logicalWidth);
    span.monitorHeight = static_cast<float>(self->logicalHeight);
    span.totalWidth = static_cast<float>(maxX - minX);
    span.totalHeight = static_cast<float>(maxY - minY);
    return span;
  }

  void
  notifyKdePlasmaWallpaper(const std::string& imagePath, const std::string& connector, const WaylandOutput* output) {
    if (!compositors::isKde()) {
      return;
    }
    if (imagePath.starts_with("color:")) {
      return;
    }

    kLog.info("syncing wallpaper to KDE Plasma: {} (connector: {})", imagePath, connector);

    std::string script;
    if (output == nullptr) {
      script = "var d = desktops();"
               "for (var i = 0; i < d.length; i++) {"
               "  d[i].wallpaperPlugin = 'org.kde.image';"
               "  d[i].currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General'];"
               "  d[i].writeConfig('Image', 'file://"
          + imagePath
          + "');"
            "}";
    } else {
      script = "var d = desktops();"
               "for (var i = 0; i < d.length; i++) {"
               "  var g = screenGeometry(d[i].screen);"
               "  if (g.x == "
          + std::to_string(output->logicalX)
          + " && g.y == "
          + std::to_string(output->logicalY)
          + " && g.width == "
          + std::to_string(output->logicalWidth)
          + " && g.height == "
          + std::to_string(output->logicalHeight)
          + ") {"
            "    d[i].wallpaperPlugin = 'org.kde.image';"
            "    d[i].currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General'];"
            "    d[i].writeConfig('Image', 'file://"
          + imagePath
          + "');"
            "  }"
            "}";
    }

    const bool launched = process::runAsync(
        {"dbus-send", "--session", "--dest=org.kde.plasmashell", "--type=method_call", "/PlasmaShell",
         "org.kde.PlasmaShell.evaluateScript", "string:" + script}
    );

    if (!launched) {
      kLog.warn("failed to launch dbus-send to sync plasma wallpaper");
    }
  }

} // namespace

Wallpaper::Wallpaper() = default;

Wallpaper::~Wallpaper() {
  for (auto& inst : m_instances) {
    releaseInstanceTextures(*inst);
  }
}

TextureHandle Wallpaper::currentTexture() const {
  for (const auto& inst : m_instances) {
    if (inst->currentTexture.id != 0) {
      return inst->currentTexture;
    }
  }
  return {};
}

std::string Wallpaper::currentPath() const {
  for (const auto& inst : m_instances) {
    if (inst->currentTexture.id != 0 && !inst->currentPath.empty()) {
      return inst->currentPath;
    }
  }
  return {};
}

bool Wallpaper::ownsSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return false;
  }
  for (const auto& instance : m_instances) {
    if (instance->surface != nullptr && instance->surface->wlSurface() == surface) {
      return true;
    }
  }
  return false;
}

bool Wallpaper::onPointerEvent(const PointerEvent& event) {
  if (!m_wallpaperEnabled || m_instances.empty() || m_wayland == nullptr) {
    return false;
  }

  wl_surface* eventSurface = event.surface;
  if (eventSurface == nullptr) {
    eventSurface = m_wayland->lastPointerSurface();
  }
  if (!ownsSurface(eventSurface)) {
    return false;
  }

  if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion) {
    const std::uint32_t serial = event.serial != 0 ? event.serial : m_wayland->lastInputSerial();
    if (serial != 0) {
      m_wayland->setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }
  }

  return true;
}

void Wallpaper::onGpuResourcesInvalidated() {
  for (auto& inst : m_instances) {
    if (inst->currentSourceKind == WallpaperSourceKind::Image && !inst->currentPath.empty()) {
      if (m_textureCache != nullptr && m_textureCache->shared()) {
        inst->currentTexture = m_textureCache->peek(inst->currentPath);
      } else if (m_renderContext != nullptr) {
        if (inst->currentTexture.id != 0) {
          m_renderContext->backend().textureManager().unload(inst->currentTexture);
        }
        inst->currentTexture = m_renderContext->backend().textureManager().loadFromFile(inst->currentPath, 0, true);
      }
    }
    if (inst->nextSourceKind == WallpaperSourceKind::Image && !inst->pendingPath.empty()) {
      if (m_textureCache != nullptr && m_textureCache->shared()) {
        inst->nextTexture = m_textureCache->peek(inst->pendingPath);
      } else if (m_renderContext != nullptr) {
        if (inst->nextTexture.id != 0) {
          m_renderContext->backend().textureManager().unload(inst->nextTexture);
        }
        inst->nextTexture = m_renderContext->backend().textureManager().loadFromFile(inst->pendingPath, 0, true);
      }
    }
    updateRendererState(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

bool Wallpaper::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache,
    noctalia::theme::ThemeService* themeService
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_textureCache = textureCache;
  m_themeService = themeService;

  // Register reload callback unconditionally so toggling enabled in config works.
  m_config->addReloadCallback([this]() { reload(); }, "wallpaper");
  m_paletteConn = paletteChanged().connect([this] {
    for (auto& inst : m_instances) {
      updateRendererState(*inst);
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
      }
    }
  });

  if (!m_config->config().wallpaper.enabled) {
    m_wallpaperEnabled = false;
    m_lastWallpaperConfig = m_config->config().wallpaper;
    kLog.info("disabled in config");
    return true;
  }

  resetAutomationState();
  m_wallpaperEnabled = true;
  m_lastWallpaperConfig = m_config->config().wallpaper;
  applyStartupAutomation(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
  );
  return true;
}

void Wallpaper::reload() {
  const auto& wallpaperConfig = m_config->config().wallpaper;
  const bool nowEnabled = wallpaperConfig.enabled;

  if (nowEnabled && m_wallpaperEnabled && wallpaperConfig == m_lastWallpaperConfig) {
    return;
  }

  kLog.info("reloading config");

  const bool wasEnabled = m_wallpaperEnabled;

  if (!nowEnabled) {
    resetAutomationState();
    m_wallpaperEnabled = false;
    // Wallpaper disabled — full teardown
    for (auto& inst : m_instances) {
      releaseInstanceTextures(*inst);
    }
    m_instances.clear();
    m_lastWallpaperConfig = wallpaperConfig;
    return;
  }

  if (!wasEnabled) {
    resetAutomationState();
  }
  m_wallpaperEnabled = true;
  m_lastWallpaperConfig = wallpaperConfig;

  // Wallpaper remains (or becomes) enabled — sync instances without teardown
  // to avoid flickering. syncInstances handles monitor override changes
  // (adds/removes instances) without disturbing existing surfaces.
  syncInstances();

  // Refresh renderer state on all instances to pick up fill mode / smoothness
  // changes that take effect immediately without a texture reload.
  for (auto& inst : m_instances) {
    updateRendererState(*inst);
    inst->surface->requestRedraw();
  }
}

void Wallpaper::onOutputChange() {
  if (m_config == nullptr || !m_config->config().wallpaper.enabled) {
    return;
  }
  syncInstances();

  // Span fill mode maps a single image across the whole desktop, so a geometry
  // change on any output shifts the slice shown on every other output. Refresh
  // all instances; the node setters no-op when the span geometry is unchanged.
  if (m_config->config().wallpaper.fillMode == WallpaperFillMode::Span) {
    for (auto& inst : m_instances) {
      updateRendererState(*inst);
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
      }
    }
  }
}

std::vector<WallpaperChange> Wallpaper::onStateChange() {
  kLog.info("state file changed, checking for updates");

  std::vector<WallpaperChange> changes;
  for (auto& inst : m_instances) {
    auto newPath = m_config->getWallpaperPath(inst->connectorName);
    if (inst->surface == nullptr || inst->wallpaperNode == nullptr) {
      continue;
    }

    if (newPath.empty()) {
      if (!inst->currentPath.empty() || inst->currentTexture.id != 0 || inst->nextTexture.id != 0) {
        changes.push_back({.path = newPath, .connector = inst->connectorName});
        if (inst->transitionAnimId != 0) {
          inst->animations.cancel(inst->transitionAnimId);
          inst->transitionAnimId = 0;
        }
        releaseInstanceTextures(*inst);
        inst->currentTexture = {};
        inst->nextTexture = {};
        inst->currentSourceKind = WallpaperSourceKind::Image;
        inst->nextSourceKind = WallpaperSourceKind::Image;
        inst->currentColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
        inst->nextColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
        inst->currentPath.clear();
        inst->pendingPath.clear();
        inst->queuedPath.clear();
        inst->transitionTime = 0.0f;
        inst->transitioning = false;
        inst->transitionDirection = WallpaperTransitionDirection::Forward;
        updateRendererState(*inst);
        inst->surface->requestRedraw();
      }
      continue;
    }

    if (inst->transitioning) {
      switch (redirectActiveTransition(*inst, newPath)) {
      case TransitionRedirect::AlreadyTargeting:
        continue;
      case TransitionRedirect::Redirected:
        changes.push_back({.path = newPath, .connector = inst->connectorName});
        continue;
      case TransitionRedirect::Unrelated:
        inst->queuedPath = newPath;
        changes.push_back({.path = newPath, .connector = inst->connectorName});
        continue;
      }

      std::unreachable();
    }

    if (newPath == inst->currentPath) {
      continue;
    }

    kLog.info("changing {} → {}", inst->connectorName, newPath);
    loadWallpaper(*inst, newPath);
    changes.push_back({.path = newPath, .connector = inst->connectorName});

    const WaylandOutput* output = nullptr;
    if (m_wayland != nullptr) {
      for (const auto& out : m_wayland->outputs()) {
        if (out.name == inst->outputName) {
          output = &out;
          break;
        }
      }
    }
    notifyKdePlasmaWallpaper(newPath, inst->connectorName, output);
  }

  // Any wallpaper change (manual selection, IPC, or automation) restarts the
  // rotation interval so the next automatic switch is a full interval away.
  if (!changes.empty()) {
    using namespace std::chrono;
    m_lastAutomationSwitchSecond = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  }
  return changes;
}

void Wallpaper::onSecondTick() {
  if (m_config == nullptr || !m_config->config().wallpaper.enabled) {
    return;
  }

  using namespace std::chrono;
  const auto secondStamp = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  if (secondStamp == m_lastAutomationSecondStamp) {
    return;
  }
  m_lastAutomationSecondStamp = secondStamp;
  runAutomation(secondStamp);
}

bool Wallpaper::isConnectorKnown(std::string_view connector) const {
  if (m_wayland == nullptr) {
    return false;
  }
  return std::ranges::any_of(m_wayland->outputs(), [&](const WaylandOutput& out) {
    return !out.connectorName.empty() && out.connectorName == connector;
  });
}

void Wallpaper::applyResolvedWallpaper(const std::optional<std::string>& connector, const std::string& resolvedPath) {
  const WaylandOutput* output = nullptr;
  if (connector.has_value() && m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (out.connectorName == *connector) {
        output = &out;
        break;
      }
    }
  }
  notifyKdePlasmaWallpaper(resolvedPath, connector.value_or(""), output);

  if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(resolvedPath); favorite != nullptr) {
    if (connector.has_value()) {
      m_config->applyWallpaperSelection(connector, resolvedPath, favorite, {});
      return;
    }

    std::vector<std::string> connectors;
    if (m_wayland != nullptr) {
      for (const auto& out : m_wayland->outputs()) {
        if (!out.connectorName.empty()) {
          connectors.push_back(out.connectorName);
        }
      }
    }
    m_config->applyWallpaperSelection(std::nullopt, resolvedPath, favorite, connectors);
    return;
  }

  if (connector.has_value()) {
    m_config->setWallpaperPath(connector, resolvedPath);
    return;
  }

  // Match wallpaper panel "All monitors": per-output overrides win over default in
  // getWallpaperPath(), so set every connected output plus default or the image never updates.
  ConfigService::WallpaperBatch batch(*m_config);
  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (!out.connectorName.empty()) {
        m_config->setWallpaperPath(out.connectorName, resolvedPath);
      }
    }
  }
  m_config->setWallpaperPath(std::nullopt, resolvedPath);
}

bool Wallpaper::applyWallpaperImage(const std::optional<std::string>& connector, const std::string& path) {
  if (m_config == nullptr) {
    return false;
  }
  if (connector.has_value() && !isConnectorKnown(*connector)) {
    return false;
  }
  const auto resolved = resolveWallpaperPath(path);
  if (!resolved.has_value()) {
    return false;
  }
  applyResolvedWallpaper(connector, *resolved);
  return true;
}

void Wallpaper::registerIpc(IpcService& ipc) {
  auto validateOutputConnector = [this](std::string_view outputConnector) -> std::string {
    if (m_wayland == nullptr || isConnectorKnown(outputConnector)) {
      return {};
    }

    std::vector<std::string> known;
    const auto& outputs = m_wayland->outputs();
    for (const auto& out : outputs) {
      if (!out.connectorName.empty()) {
        known.push_back(out.connectorName);
      }
    }
    const std::string suffix =
        known.empty() ? std::string() : std::string("; known: ") + StringUtils::join(known, ", ");
    return "error: unknown output \"" + std::string(outputConnector) + "\"" + suffix + "\n";
  };

  // Map a switch outcome to an IPC response. NoChange (e.g. a single wallpaper in
  // the directory) is not an error, so it does not use the "error:" prefix.
  const auto switchResponse = [](SwitchOutcome outcome) -> std::string {
    switch (outcome) {
    case SwitchOutcome::Changed:
      return "ok\n";
    case SwitchOutcome::NoChange:
      return "no other wallpaper available to switch to\n";
    case SwitchOutcome::Unavailable:
      return "error: no wallpapers available or wallpaper disabled\n";
    }
    return "error: failed to switch wallpaper\n";
  };

  const auto switchWallpaperHandler = [this, validateOutputConnector,
                                       switchResponse](PickWallpaper action, const std::string& args) -> std::string {
    const auto trimmed = StringUtils::trim(args);
    std::optional<std::string_view> connector;
    if (!trimmed.empty()) {
      if (const std::string err = validateOutputConnector(trimmed); !err.empty()) {
        return err;
      }
      connector = trimmed;
    }
    return switchResponse(switchWallpaperTo(action, connector));
  };

  ipc.registerHandler(
      "wallpaper-random",
      [switchWallpaperHandler](const std::string& args) -> std::string {
        return switchWallpaperHandler(PickWallpaper::Random, args);
      },
      "wallpaper-random [<connector>]", "Switch to a random wallpaper immediately"
  );

  ipc.registerHandler(
      "wallpaper-next",
      [switchWallpaperHandler](const std::string& args) -> std::string {
        return switchWallpaperHandler(PickWallpaper::Next, args);
      },
      "wallpaper-next [<connector>]", "Switch to the next wallpaper immediately"
  );

  ipc.registerHandler(
      "wallpaper-previous",
      [switchWallpaperHandler](const std::string& args) -> std::string {
        return switchWallpaperHandler(PickWallpaper::Previous, args);
      },
      "wallpaper-previous [<connector>]", "Switch to the previous wallpaper immediately"
  );

  ipc.registerHandler(
      "wallpaper-get",
      [this, validateOutputConnector](const std::string& args) -> std::string {
        if (m_config == nullptr) {
          return "error: wallpaper service not initialized\n";
        }
        const auto tokens = StringUtils::splitWhitespace(StringUtils::trim(args));
        if (tokens.empty()) {
          std::string out = m_config->getDefaultWallpaperPath();
          out.push_back('\n');
          return out;
        }
        if (tokens.size() != 1) {
          return "error: wallpaper-get accepts at most <connector>\n";
        }
        if (const std::string error = validateOutputConnector(tokens[0]); !error.empty()) {
          return error;
        }
        std::string out = m_config->getWallpaperPath(tokens[0]);
        out.push_back('\n');
        return out;
      },
      "wallpaper-get [<connector>]", "Print default wallpaper path, or effective path for an output"
  );
  ipc.registerHandler(
      "wallpaper-set",
      [this, &ipc, validateOutputConnector](const std::string& args) -> std::string {
        if (m_config == nullptr) {
          return "error: wallpaper service not initialized\n";
        }
        const auto tokens = StringUtils::splitWhitespace(StringUtils::trim(args));
        if (tokens.empty()) {
          return "error: path required (wallpaper-set [<connector>] <path>)\n";
        }

        const std::optional<std::string_view> callerCwd =
            ipc.callerCwd().has_value() ? std::optional<std::string_view>{*ipc.callerCwd()} : std::nullopt;

        const auto isConnector = [&](const std::string& connector) {
          return validateOutputConnector(connector).empty();
        };
        const auto parsed = parseWallpaperSetTokens(tokens, isConnector, callerCwd);
        if (parsed.path.empty()) {
          return "error: path required (wallpaper-set [<connector>] <path>)\n";
        }

        std::optional<std::string> outputConnector = parsed.connector;
        std::string resolved;
        if (const auto path = resolveWallpaperPath(parsed.path, callerCwd); path.has_value()) {
          resolved = *path;
        } else {
          return "error: path does not exist or is not a regular file\n";
        }

        if (outputConnector.has_value()) {
          if (const std::string error = validateOutputConnector(*outputConnector); !error.empty()) {
            return error;
          }
        }
        applyResolvedWallpaper(outputConnector, resolved);
        return "ok\n";
      },
      "wallpaper-set [<connector>] <path>", "Set wallpaper for all or a specific output (persisted)"
  );
}

void Wallpaper::syncInstances() {
  const auto& outputs = m_wayland->outputs();

  // Remove instances for outputs that no longer exist or are now disabled by monitor override
  std::erase_if(m_instances, [&](const auto& inst) {
    const auto* output = [&]() -> const WaylandOutput* {
      for (const auto& out : outputs) {
        if (out.name == inst->outputName)
          return &out;
      }
      return nullptr;
    }();

    if (output == nullptr) {
      kLog.info("removing instance for output {} (disconnected)", inst->outputName);
      releaseInstanceTextures(*inst);
      return true;
    }
    if (!output->done || !output->hasUsableGeometry()) {
      kLog.info("removing instance for output {} (geometry unavailable)", inst->outputName);
      releaseInstanceTextures(*inst);
      return true;
    }

    // Check if a monitor override now disables this output
    if (const auto* ovr = wallpaper::findWallpaperMonitorOverride(m_config->config().wallpaper, *output);
        ovr != nullptr && ovr->enabled && !*ovr->enabled) {
      kLog.info("removing instance for {} — disabled by monitor override", output->connectorName);
      releaseInstanceTextures(*inst);
      return true;
    }

    // An external wallpaper source (e.g. mpvpaper plugin) now owns this output
    if (m_externallyManagedOutputs.contains(output->connectorName)) {
      kLog.info("removing instance for {} — managed by external source", output->connectorName);
      releaseInstanceTextures(*inst);
      return true;
    }

    return false;
  });

  // Create instances for new outputs
  for (const auto& output : outputs) {
    if (!output.done || output.connectorName.empty() || !output.hasUsableGeometry()) {
      continue;
    }

    bool exists =
        std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->outputName == output.name; });
    if (exists) {
      continue;
    }

    bool enabled = true;
    if (const auto* ovr = wallpaper::findWallpaperMonitorOverride(m_config->config().wallpaper, output);
        ovr != nullptr && ovr->enabled) {
      enabled = *ovr->enabled;
    }
    if (!enabled) {
      kLog.info("skipping {} ({}) — disabled by monitor override", output.connectorName, output.description);
      continue;
    }
    if (m_externallyManagedOutputs.contains(output.connectorName)) {
      kLog.info("skipping {} ({}) — managed by external source", output.connectorName, output.description);
      continue;
    }

    createInstance(output);
  }
}

void Wallpaper::setOutputExternallyManaged(const std::string& connector, bool managed) {
  if (connector.empty()) {
    return;
  }
  const bool changed =
      managed ? m_externallyManagedOutputs.insert(connector).second : (m_externallyManagedOutputs.erase(connector) > 0);
  if (changed && m_wallpaperEnabled && m_wayland != nullptr) {
    syncInstances();
  }
}

void Wallpaper::resetAutomationState() {
  m_lastAutomationSecondStamp = -1;
  m_lastAutomationSwitchSecond = -1;
}

void Wallpaper::setAutomationGate(std::function<bool()> gate) { m_automationGate = std::move(gate); }

bool Wallpaper::automationAllowed() const noexcept { return !m_automationGate || m_automationGate(); }

ThemeMode Wallpaper::directoryThemeMode() const noexcept {
  const ThemeMode configured = m_config != nullptr ? m_config->config().theme.mode : ThemeMode::Dark;
  const bool isLight = m_themeService != nullptr ? m_themeService->isLightMode() : configured == ThemeMode::Light;
  return wallpaper::effectiveThemeMode(configured, isLight);
}

void Wallpaper::applyStartupAutomation(std::int64_t secondStamp) {
  const auto& wallpaper = m_config->config().wallpaper;
  const auto& automation = wallpaper.automation;
  if (!automation.enabled || m_wayland == nullptr || !automationAllowed()) {
    return;
  }

  const auto& outputs = m_wayland->outputs();
  const ThemeMode mode = directoryThemeMode();
  bool attempted = false;

  ConfigService::WallpaperBatch batch(*m_config);

  if (wallpaper.perMonitorDirectories) {
    for (const auto& output : outputs) {
      if (!output.done
          || output.connectorName.empty()
          || !output.hasUsableGeometry()
          || !wallpaperOutputEnabled(wallpaper, output)) {
        continue;
      }

      attempted = true;
      std::vector<std::string> candidates;
      collectWallpaperCandidates(
          wallpaper::resolveWallpaperDirectory(wallpaper, output, mode), automation.recursive, candidates
      );
      if (candidates.empty()) {
        continue;
      }

      const std::string currentPath = m_config->getWallpaperPath(output.connectorName);
      const std::string picked = pickAutomationWallpaperPath(automation, std::move(candidates), currentPath);
      if (picked.empty() || picked == currentPath) {
        continue;
      }

      if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(picked); favorite != nullptr) {
        m_config->applyWallpaperSelection(output.connectorName, picked, favorite, {});
      } else {
        m_config->setWallpaperPath(output.connectorName, picked);
      }
      kLog.info("startup automation set {} → {}", output.connectorName, picked);
    }
  } else {
    for (const auto& output : outputs) {
      if (output.done
          && !output.connectorName.empty()
          && output.hasUsableGeometry()
          && wallpaperOutputEnabled(wallpaper, output)) {
        attempted = true;
        break;
      }
    }

    if (attempted) {
      std::vector<std::string> candidates;
      collectWallpaperCandidates(
          wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode), automation.recursive, candidates
      );
      if (!candidates.empty()) {
        const std::string currentDefault = m_config->getDefaultWallpaperPath();
        const std::string picked = pickAutomationWallpaperPath(automation, std::move(candidates), currentDefault);
        if (!picked.empty()) {
          if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(picked); favorite != nullptr) {
            std::vector<std::string> connectors;
            for (const auto& output : outputs) {
              if (output.done
                  && !output.connectorName.empty()
                  && output.hasUsableGeometry()
                  && wallpaperOutputEnabled(wallpaper, output)) {
                connectors.push_back(output.connectorName);
              }
            }
            m_config->applyWallpaperSelection(std::nullopt, picked, favorite, connectors);
          } else {
            for (const auto& output : outputs) {
              if (output.done
                  && !output.connectorName.empty()
                  && output.hasUsableGeometry()
                  && wallpaperOutputEnabled(wallpaper, output)) {
                m_config->setWallpaperPath(output.connectorName, picked);
              }
            }
            m_config->setWallpaperPath(std::nullopt, picked);
          }
          kLog.info("startup automation set all outputs → {}", picked);
        }
      }
    }
  }

  if (attempted) {
    m_lastAutomationSecondStamp = secondStamp;
    m_lastAutomationSwitchSecond = secondStamp;
  }
}

void Wallpaper::runAutomation(std::int64_t secondStamp) {
  const auto& wallpaper = m_config->config().wallpaper;
  const auto& automation = wallpaper.automation;
  if (!automation.enabled || m_instances.empty() || !automationAllowed()) {
    return;
  }

  if (m_lastAutomationSwitchSecond >= 0
      && (secondStamp - m_lastAutomationSwitchSecond) < static_cast<std::int64_t>(automation.intervalSeconds)) {
    return;
  }

  const ThemeMode mode = directoryThemeMode();

  ConfigService::WallpaperBatch batch(*m_config);

  if (wallpaper.perMonitorDirectories) {
    for (const auto& inst : m_instances) {
      if (inst->connectorName.empty()) {
        continue;
      }
      const WaylandOutput* output = nullptr;
      if (m_wayland != nullptr) {
        for (const auto& out : m_wayland->outputs()) {
          if (out.output == inst->output) {
            output = &out;
            break;
          }
        }
      }
      std::vector<std::string> candidates;
      const std::string dir = output != nullptr ? wallpaper::resolveWallpaperDirectory(wallpaper, *output, mode)
                                                : wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode);
      collectWallpaperCandidates(dir, automation.recursive, candidates);
      if (candidates.empty()) {
        continue;
      }
      const std::string currentPath = m_config->getWallpaperPath(inst->connectorName);
      const std::string picked = pickAutomationWallpaperPath(automation, std::move(candidates), currentPath);
      if (picked.empty() || picked == currentPath) {
        continue;
      }
      if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(picked); favorite != nullptr) {
        m_config->applyWallpaperSelection(inst->connectorName, picked, favorite, {});
      } else {
        m_config->setWallpaperPath(inst->connectorName, picked);
      }
      kLog.info("automation set {} → {}", inst->connectorName, picked);
    }
  } else {
    std::vector<std::string> candidates;
    const std::string dir = wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode);
    collectWallpaperCandidates(dir, automation.recursive, candidates);
    if (!candidates.empty()) {
      const std::string currentDefault = m_config->getDefaultWallpaperPath();
      const std::string picked = pickAutomationWallpaperPath(automation, std::move(candidates), currentDefault);
      if (!picked.empty()) {
        if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(picked); favorite != nullptr) {
          std::vector<std::string> connectors;
          connectors.reserve(m_instances.size());
          for (const auto& inst : m_instances) {
            if (!inst->connectorName.empty()) {
              connectors.push_back(inst->connectorName);
            }
          }
          m_config->applyWallpaperSelection(std::nullopt, picked, favorite, connectors);
        } else {
          for (const auto& inst : m_instances) {
            if (!inst->connectorName.empty()) {
              m_config->setWallpaperPath(inst->connectorName, picked);
            }
          }
          m_config->setWallpaperPath(std::nullopt, picked);
        }
        kLog.info("automation set all outputs → {}", picked);
      }
    }
  }
  m_lastAutomationSwitchSecond = secondStamp;
}

Wallpaper::SwitchOutcome Wallpaper::switchWallpaperTo(PickWallpaper action, std::optional<std::string_view> connector) {
  if (m_config == nullptr || !m_config->config().wallpaper.enabled || m_instances.empty()) {
    return SwitchOutcome::Unavailable;
  }

  const auto& wallpaper = m_config->config().wallpaper;
  const ThemeMode mode = directoryThemeMode();

  const auto pick = [action](std::vector<std::string> candidates, const std::string& currentPath) -> std::string {
    switch (action) {
    case PickWallpaper::Random:
      return pickRandomWallpaperPath(candidates, currentPath);
    case PickWallpaper::Next:
      return pickAlphabeticalWallpaperPath(std::move(candidates), currentPath, 1);
    case PickWallpaper::Previous:
      return pickAlphabeticalWallpaperPath(std::move(candidates), currentPath, -1);
    }
    return {};
  };

  if (connector.has_value()) {
    if (m_wayland != nullptr) {
      const auto& outputs = m_wayland->outputs();
      const bool found = std::ranges::any_of(outputs, [&](const WaylandOutput& out) {
        return !out.connectorName.empty() && out.connectorName == *connector;
      });
      if (!found) {
        return SwitchOutcome::Unavailable;
      }
    }

    WallpaperInstance* targetInst = nullptr;
    for (const auto& inst : m_instances) {
      if (inst->connectorName == *connector) {
        targetInst = inst.get();
        break;
      }
    }
    if (targetInst == nullptr) {
      return SwitchOutcome::Unavailable;
    }

    const WaylandOutput* output = nullptr;
    if (m_wayland != nullptr) {
      for (const auto& out : m_wayland->outputs()) {
        if (out.output == targetInst->output) {
          output = &out;
          break;
        }
      }
    }
    std::vector<std::string> candidates;
    const std::string dir = output != nullptr ? wallpaper::resolveWallpaperDirectory(wallpaper, *output, mode)
                                              : wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode);
    collectWallpaperCandidates(dir, wallpaper.automation.recursive, candidates);
    if (candidates.empty()) {
      return SwitchOutcome::Unavailable;
    }
    const std::string currentPath = m_config->getWallpaperPath(std::string(*connector));
    const std::string picked = pick(std::move(candidates), currentPath);
    if (picked.empty() || picked == currentPath) {
      return SwitchOutcome::NoChange;
    }
    m_config->setWallpaperPath(std::string(*connector), picked);
    kLog.info("ipc set {} → {}", *connector, picked);
    return SwitchOutcome::Changed;
  }

  ConfigService::WallpaperBatch batch(*m_config);
  bool anyChanged = false;
  bool sawCandidates = false;

  if (wallpaper.perMonitorDirectories) {
    for (const auto& inst : m_instances) {
      if (inst->connectorName.empty()) {
        continue;
      }
      const WaylandOutput* output = nullptr;
      if (m_wayland != nullptr) {
        for (const auto& out : m_wayland->outputs()) {
          if (out.output == inst->output) {
            output = &out;
            break;
          }
        }
      }
      std::vector<std::string> candidates;
      const std::string dir = output != nullptr ? wallpaper::resolveWallpaperDirectory(wallpaper, *output, mode)
                                                : wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode);
      collectWallpaperCandidates(dir, wallpaper.automation.recursive, candidates);
      if (candidates.empty()) {
        continue;
      }
      sawCandidates = true;
      const std::string currentPath = m_config->getWallpaperPath(inst->connectorName);
      const std::string picked = pick(std::move(candidates), currentPath);
      if (picked.empty() || picked == currentPath) {
        continue;
      }
      m_config->setWallpaperPath(inst->connectorName, picked);
      kLog.info("ipc set {} → {}", inst->connectorName, picked);
      anyChanged = true;
    }
  } else {
    std::vector<std::string> candidates;
    const std::string dir = wallpaper::resolveGlobalWallpaperDirectory(wallpaper, mode);
    collectWallpaperCandidates(dir, wallpaper.automation.recursive, candidates);
    if (!candidates.empty()) {
      sawCandidates = true;
      const std::string currentDefault = m_config->getDefaultWallpaperPath();
      const std::string picked = pick(std::move(candidates), currentDefault);
      if (!picked.empty() && picked != currentDefault) {
        for (const auto& inst : m_instances) {
          if (!inst->connectorName.empty()) {
            m_config->setWallpaperPath(inst->connectorName, picked);
          }
        }
        m_config->setWallpaperPath(std::nullopt, picked);
        kLog.info("ipc set all outputs → {}", picked);
        anyChanged = true;
      }
    }
  }

  if (anyChanged) {
    return SwitchOutcome::Changed;
  }
  return sawCandidates ? SwitchOutcome::NoChange : SwitchOutcome::Unavailable;
}

void Wallpaper::createInstance(const WaylandOutput& output) {
  auto wallpaperPath = m_config->getWallpaperPath(output.connectorName);
  kLog.info("creating on {} ({}), path={}", output.connectorName, output.description, wallpaperPath);

  auto instance = std::make_unique<WallpaperInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->connectorName = output.connectorName;
  instance->description = output.description;

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-wallpaper",
      .layer = LayerShellLayer::Background,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = 0,
      .exclusiveZone = -1,
  };

  instance->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);
  instance->surface->setClickThrough(true);

  instance->sceneRoot = std::make_unique<Node>();
  instance->sceneRoot->setAnimationManager(&instance->animations);
  auto fillNode = std::make_unique<Box>();
  instance->fillNode = static_cast<Box*>(instance->sceneRoot->addChild(std::move(fillNode)));
  auto wallpaperNode = std::make_unique<WallpaperNode>();
  instance->wallpaperNode = static_cast<WallpaperNode*>(instance->sceneRoot->addChild(std::move(wallpaperNode)));
  instance->surface->setSceneRoot(instance->sceneRoot.get());

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst, wallpaperPath](std::uint32_t width, std::uint32_t height) {
    const auto sw = static_cast<float>(width);
    const auto sh = static_cast<float>(height);
    inst->sceneRoot->setSize(sw, sh);
    inst->fillNode->setPosition(0.0f, 0.0f);
    inst->fillNode->setSize(sw, sh);
    inst->wallpaperNode->setPosition(0.0f, 0.0f);
    inst->wallpaperNode->setSize(sw, sh);

    if (inst->currentPath.empty() && !wallpaperPath.empty()) {
      loadWallpaper(*inst, wallpaperPath);
    } else {
      updateRendererState(*inst);
    }
  });

  instance->surface->setAnimationManager(&instance->animations);

  instance->surface->setUpdateCallback([this, inst]() { updateRendererState(*inst); });

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  m_instances.push_back(std::move(instance));
}

void Wallpaper::releaseInstanceTextures(WallpaperInstance& inst) {
  releaseTexture(inst.currentTexture, inst.currentPath);
  releaseTexture(inst.nextTexture, inst.pendingPath);
}

TextureHandle Wallpaper::acquireTexture(const std::string& path) {
  if (path.empty() || m_textureCache == nullptr) {
    return {};
  }

  auto handle = m_textureCache->acquire(path);
  if (handle.id != 0 || m_textureCache->shared() || m_renderContext == nullptr) {
    return handle;
  }

  m_renderContext->backend().makeCurrentNoSurface();
  return m_renderContext->textureManager().loadFromFile(path, 0, true);
}

void Wallpaper::releaseTexture(TextureHandle& handle, const std::string& path) {
  if (handle.id == 0) {
    return;
  }

  if (m_textureCache != nullptr && m_textureCache->shared()) {
    m_textureCache->release(handle, path);
    return;
  }

  if (m_renderContext != nullptr) {
    m_renderContext->backend().makeCurrentNoSurface();
    m_renderContext->textureManager().unload(handle);
    return;
  }

  handle = {};
}

// ── Wallpaper loading & transitions ──────────────────────────────────────────

void Wallpaper::loadWallpaper(WallpaperInstance& instance, const std::string& path) {
  // Nothing to do if we're already at (or heading toward) this wallpaper.
  if (!instance.transitioning && path == instance.currentPath) {
    return;
  }

  if (instance.transitioning) {
    if (redirectActiveTransition(instance, path) == TransitionRedirect::Unrelated) {
      instance.queuedPath = path;
    }
    return;
  }

  TextureHandle newTex;
  Color newColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  WallpaperSourceKind newSourceKind = WallpaperSourceKind::Image;
  if (parseColorWallpaperPath(path, newColor)) {
    newSourceKind = WallpaperSourceKind::Color;
  } else {
    newTex = acquireTexture(path);
    if (newTex.id == 0) {
      kLog.warn("failed to load {}", path);
      return;
    }
  }

  if (instance.currentPath.empty()) {
    const auto& wpConfig = m_config->config().wallpaper;
    if (wpConfig.transitionOnStartup && !wpConfig.transitions.empty()) {
      instance.currentSourceKind = WallpaperSourceKind::Color;
      instance.currentTexture = {};
      instance.currentColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
      instance.nextSourceKind = newSourceKind;
      instance.nextTexture = newTex;
      instance.nextColor = newColor;
      instance.pendingPath = path;
      startTransition(instance);
      return;
    }

    instance.currentSourceKind = newSourceKind;
    instance.currentTexture = newTex;
    instance.currentColor = newColor;
    instance.currentPath = path;
    instance.pendingPath.clear();
    instance.queuedPath.clear();
    instance.transitionTime = 0.0f;
    instance.transitionDirection = WallpaperTransitionDirection::Forward;
    updateRendererState(instance);
    instance.surface->requestRedraw();
    m_changed.emit();
    return;
  }

  instance.nextSourceKind = newSourceKind;
  instance.nextTexture = newTex;
  instance.nextColor = newColor;
  instance.pendingPath = path;
  startTransition(instance);
}

Wallpaper::TransitionRedirect
Wallpaper::redirectActiveTransition(WallpaperInstance& instance, const std::string& path) {
  if (!instance.transitioning) {
    return TransitionRedirect::Unrelated;
  }

  const auto direction = directionForPath(instance, path);
  if (!direction.has_value()) {
    return TransitionRedirect::Unrelated;
  }

  instance.queuedPath.clear();
  if (instance.transitionDirection == *direction) {
    return TransitionRedirect::AlreadyTargeting;
  }

  startTransitionAnimation(instance, instance.transitionTime, *direction);
  return TransitionRedirect::Redirected;
}

void Wallpaper::startTransition(WallpaperInstance& instance) {
  const auto& wpConfig = m_config->config().wallpaper;

  if (wpConfig.transitions.empty()) {
    instance.transitioning = true;
    instance.transitionDirection = WallpaperTransitionDirection::Forward;
    finishTransition(instance);
    return;
  }

  float aspectRatio = 1.777f;
  if (instance.surface->height() > 0) {
    aspectRatio = static_cast<float>(instance.surface->width()) / static_cast<float>(instance.surface->height());
  }

  const auto& transitions = wpConfig.transitions;
  const auto picked =
      transitions[static_cast<std::size_t>(std::floor(randomFloat(0.0f, static_cast<float>(transitions.size()))))];
  instance.activeTransition = picked;
  instance.transitionParams = randomizeParams(picked, wpConfig.edgeSmoothness, aspectRatio);
  startTransitionAnimation(instance, 0.0f, WallpaperTransitionDirection::Forward);
}

void Wallpaper::startTransitionAnimation(
    WallpaperInstance& instance, float fromTime, WallpaperTransitionDirection direction
) {
  if (const auto animId = std::exchange(instance.transitionAnimId, 0); animId != 0) {
    instance.animations.cancel(animId);
  }

  const auto& wpConfig = m_config->config().wallpaper;
  fromTime = std::clamp(fromTime, 0.0f, 1.0f);
  const float toTime = transitionTargetTime(direction);

  instance.transitioning = true;
  instance.transitionDirection = direction;
  setTransitionTime(instance, fromTime);

  const float durationMs = std::abs(toTime - fromTime) * wpConfig.transitionDurationMs;
  if (durationMs <= 0.0f) {
    setTransitionTime(instance, toTime);
    finishTransition(instance);
    return;
  }

  auto* inst = &instance;
  // The transition runs on its own configured duration, decoupled from the global
  // motion system: animateTimer ignores both the animation-speed multiplier and the
  // animations-enabled toggle (disabling animations must not skip the crossfade).
  instance.transitionAnimId = instance.animations.animateTimer(
      fromTime, toTime, durationMs, Easing::Linear, [inst](float time) { setTransitionTime(*inst, time); },
      [this, inst]() { finishTransition(*inst); }
  );

  updateRendererState(instance);
  instance.surface->requestRedraw();
}

void Wallpaper::finishTransition(WallpaperInstance& instance) {
  const bool displayedWallpaperChanged = instance.transitionDirection == WallpaperTransitionDirection::Forward;
  instance.transitionAnimId = 0;

  if (displayedWallpaperChanged) {
    promotePendingWallpaper(instance);
  } else {
    discardPendingWallpaper(instance);
  }

  instance.transitionTime = 0.0f;
  instance.transitioning = false;
  instance.transitionDirection = WallpaperTransitionDirection::Forward;
  updateRendererState(instance);
  instance.surface->requestRedraw();

  if (displayedWallpaperChanged) {
    m_changed.emit();
  }

  runQueuedWallpaper(instance);
}

void Wallpaper::promotePendingWallpaper(WallpaperInstance& instance) {
  releaseTexture(instance.currentTexture, instance.currentPath);
  instance.currentSourceKind = std::exchange(instance.nextSourceKind, WallpaperSourceKind::Image);
  instance.currentTexture = std::exchange(instance.nextTexture, {});
  instance.currentColor = std::exchange(instance.nextColor, defaultWallpaperColor());
  instance.currentPath = std::exchange(instance.pendingPath, std::string{});
}

void Wallpaper::discardPendingWallpaper(WallpaperInstance& instance) {
  releaseTexture(instance.nextTexture, instance.pendingPath);
  instance.nextSourceKind = WallpaperSourceKind::Image;
  instance.nextColor = defaultWallpaperColor();
  instance.pendingPath.clear();
}

void Wallpaper::runQueuedWallpaper(WallpaperInstance& instance) {
  const auto queuedPath = std::exchange(instance.queuedPath, std::string{});

  if (!queuedPath.empty() && queuedPath != instance.currentPath) {
    loadWallpaper(instance, queuedPath);
  }
}

void Wallpaper::updateRendererState(WallpaperInstance& instance) {
  auto* wallpaperNode = instance.wallpaperNode;
  if (wallpaperNode == nullptr) {
    return;
  }

  const auto& wpConfig = m_config->config().wallpaper;
  WaylandOutput output;
  output.name = instance.outputName;
  output.connectorName = instance.connectorName;
  output.description = instance.description;
  output.output = instance.output;
  output.scale = instance.scale;
  output.done = true;
  const Color fillColor = resolveWallpaperFillColor(wpConfig, output);

  if (instance.fillNode != nullptr) {
    instance.fillNode->setStyle(
        RoundedRectStyle{
            .fill = fillColor,
            .fillMode = FillMode::Solid,
        }
    );
  }
  wallpaperNode->setSources(
      instance.currentSourceKind, instance.currentTexture.id, instance.currentColor, instance.nextSourceKind,
      instance.nextTexture.id, instance.nextColor, static_cast<float>(instance.currentTexture.width),
      static_cast<float>(instance.currentTexture.height), static_cast<float>(instance.nextTexture.width),
      static_cast<float>(instance.nextTexture.height)
  );
  wallpaperNode->setTransition(
      instance.activeTransition, transitionProgressForTime(instance.transitionTime), instance.transitionParams
  );
  wallpaperNode->setFillMode(wpConfig.fillMode);
  wallpaperNode->setFillColor(fillColor);

  if (wpConfig.fillMode == WallpaperFillMode::Span && m_wayland != nullptr) {
    wallpaperNode->setSpan(computeSpanParams(m_wayland->outputs(), instance.outputName));
  } else {
    wallpaperNode->setSpan(WallpaperSpanParams{});
  }
}
