#include "shell/wallpaper/wallpaper.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/random.h"
#include "ipc/ipc_service.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

using Random::randomFloat;

namespace {

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

  const WallpaperMonitorOverride*
  findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output) {
    for (const auto& ovr : config.monitorOverrides) {
      if (outputMatchesSelector(ovr.match, output)) {
        return &ovr;
      }
    }
    return nullptr;
  }

  std::string resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode) {
    if (config.perMonitorDirectories) {
      if (const auto* ovr = findWallpaperMonitorOverride(config, output); ovr != nullptr) {
        if (mode == ThemeMode::Light && ovr->directoryLight.has_value() && !ovr->directoryLight->empty()) {
          return *ovr->directoryLight;
        }
        if (mode == ThemeMode::Dark && ovr->directoryDark.has_value() && !ovr->directoryDark->empty()) {
          return *ovr->directoryDark;
        }
        if (ovr->directory.has_value() && !ovr->directory->empty()) {
          return *ovr->directory;
        }
      }
    }
    // Fallback to global directory
    if (mode == ThemeMode::Light && !config.directoryLight.empty()) {
      return config.directoryLight;
    }
    if (mode == ThemeMode::Dark && !config.directoryDark.empty()) {
      return config.directoryDark;
    }
    return config.directory;
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
    const std::size_t minLen = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < minLen; ++i) {
      const auto ac = static_cast<unsigned char>(a[i]);
      const auto bc = static_cast<unsigned char>(b[i]);
      const auto alc = static_cast<unsigned char>(std::tolower(ac));
      const auto blc = static_cast<unsigned char>(std::tolower(bc));
      if (alc != blc) {
        return alc < blc;
      }
    }
    return a.size() < b.size();
  }

  std::string pickAlphabeticalWallpaperPath(std::vector<std::string> candidates, const std::string& currentPath) {
    if (candidates.empty()) {
      return {};
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
      return lessCaseInsensitive(a, b);
    });
    if (candidates.size() == 1) {
      return candidates.front();
    }

    const auto it = std::find(candidates.begin(), candidates.end(), currentPath);
    if (it == candidates.end()) {
      return candidates.front();
    }
    const std::size_t idx = static_cast<std::size_t>(std::distance(candidates.begin(), it));
    return candidates[(idx + 1) % candidates.size()];
  }

  constexpr Logger kLog("wallpaper");

  Color resolveWallpaperFillColor(const WallpaperConfig& config, const WaylandOutput& output) {
    const ColorSpec* fillColor = nullptr;
    if (const auto* ovr = findWallpaperMonitorOverride(config, output); ovr != nullptr && ovr->fillColor) {
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

bool Wallpaper::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_textureCache = textureCache;

  if (!m_config->config().wallpaper.enabled) {
    m_wallpaperEnabled = false;
    kLog.info("disabled in config");
    return true;
  }

  m_config->addReloadCallback([this]() { reload(); });
  m_paletteConn = paletteChanged().connect([this] {
    for (auto& inst : m_instances) {
      updateRendererState(*inst);
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
      }
    }
  });

  resetAutomationState();
  m_wallpaperEnabled = true;
  syncInstances();
  return true;
}

void Wallpaper::reload() {
  kLog.info("reloading config");

  const bool wasEnabled = m_wallpaperEnabled;
  const bool nowEnabled = m_config->config().wallpaper.enabled;

  if (!nowEnabled) {
    resetAutomationState();
    m_wallpaperEnabled = false;
    // Wallpaper disabled — full teardown
    for (auto& inst : m_instances) {
      releaseInstanceTextures(*inst);
    }
    m_instances.clear();
    return;
  }

  if (!wasEnabled) {
    resetAutomationState();
  }
  m_wallpaperEnabled = true;

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
}

void Wallpaper::onStateChange() {
  kLog.info("state file changed, checking for updates");

  for (auto& inst : m_instances) {
    auto newPath = m_config->getWallpaperPath(inst->connectorName);
    if (inst->surface == nullptr || inst->wallpaperNode == nullptr) {
      continue;
    }

    if (newPath.empty()) {
      if (!inst->currentPath.empty() || inst->currentTexture.id != 0 || inst->nextTexture.id != 0) {
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
        inst->transitioning = false;
        updateRendererState(*inst);
        inst->surface->requestRedraw();
      }
      continue;
    }

    if (inst->transitioning) {
      if (newPath == inst->pendingPath) {
        inst->queuedPath.clear();
        continue;
      }

      inst->queuedPath = newPath;
      continue;
    }

    if (newPath == inst->currentPath) {
      continue;
    }

    kLog.info("changing {} → {}", inst->connectorName, newPath);
    loadWallpaper(*inst, newPath);
  }
}

void Wallpaper::onSecondTick() {
  if (m_config == nullptr || !m_config->config().wallpaper.enabled) {
    return;
  }

  using namespace std::chrono;
  const auto minuteStamp = duration_cast<minutes>(system_clock::now().time_since_epoch()).count();
  if (minuteStamp == m_lastAutomationMinuteStamp) {
    return;
  }
  m_lastAutomationMinuteStamp = minuteStamp;
  runAutomation(minuteStamp);
}

void Wallpaper::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "wallpaper-random",
      [this](const std::string& args) -> std::string {
        const auto trimmed = StringUtils::trim(args);
        std::optional<std::string_view> connector;
        if (!trimmed.empty()) {
          connector = trimmed;
        }
        if (!switchToRandomWallpaper(connector)) {
          return "error: failed to pick a random wallpaper\n";
        }
        return "ok\n";
      },
      "wallpaper-random [<connector>]", "Switch to a random wallpaper immediately"
  );
  ipc.registerHandler(
      "wallpaper-set",
      [this](const std::string& args) -> std::string {
        if (m_config == nullptr) {
          return "error: wallpaper service not initialized\n";
        }
        const auto tokens = StringUtils::splitWhitespace(StringUtils::trim(args));
        if (tokens.empty()) {
          return "error: path required (wallpaper-set [<connector>] <path>)\n";
        }
        std::optional<std::string> outputConnector;
        std::string path;
        if (tokens.size() == 1) {
          path = tokens[0];
        } else {
          outputConnector = tokens[0];
          std::string joined = tokens[1];
          for (std::size_t i = 2; i < tokens.size(); ++i) {
            joined.push_back(' ');
            joined += tokens[i];
          }
          path = std::move(joined);
        }
        if (path.empty()) {
          return "error: path required (wallpaper-set [<connector>] <path>)\n";
        }
        std::string resolved = path;
        if (!path.starts_with("color:")) {
          resolved = FileUtils::expandUserPath(path).string();
          std::error_code ec;
          if (!std::filesystem::exists(resolved, ec)) {
            return "error: path does not exist\n";
          }
        }

        if (outputConnector.has_value()) {
          if (m_wayland != nullptr) {
            const auto& outputs = m_wayland->outputs();
            const bool found = std::any_of(outputs.begin(), outputs.end(), [&](const WaylandOutput& out) {
              return !out.connectorName.empty() && out.connectorName == *outputConnector;
            });
            if (!found) {
              std::vector<std::string> known;
              for (const auto& out : outputs) {
                if (!out.connectorName.empty()) {
                  known.push_back(out.connectorName);
                }
              }
              const std::string suffix =
                  known.empty() ? std::string() : std::string("; known: ") + StringUtils::join(known, ", ");
              return "error: unknown output \"" + *outputConnector + "\"" + suffix + "\n";
            }
          }
          m_config->setWallpaperPath(*outputConnector, resolved);
          return "ok\n";
        }

        // Match wallpaper panel "All monitors": per-output overrides win over default in
        // getWallpaperPath(), so set every connected output plus default or the image never updates.
        ConfigService::WallpaperBatch batch(*m_config);
        if (m_wayland != nullptr) {
          for (const auto& out : m_wayland->outputs()) {
            if (!out.connectorName.empty()) {
              m_config->setWallpaperPath(out.connectorName, resolved);
            }
          }
        }
        m_config->setWallpaperPath(std::nullopt, resolved);
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

    // Check if a monitor override now disables this output
    if (const auto* ovr = findWallpaperMonitorOverride(m_config->config().wallpaper, *output);
        ovr != nullptr && ovr->enabled && !*ovr->enabled) {
      kLog.info("removing instance for {} — disabled by monitor override", output->connectorName);
      releaseInstanceTextures(*inst);
      return true;
    }

    return false;
  });

  // Create instances for new outputs
  for (const auto& output : outputs) {
    if (!output.done || output.connectorName.empty()) {
      continue;
    }

    bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&output](const auto& inst) {
      return inst->outputName == output.name;
    });
    if (exists) {
      continue;
    }

    bool enabled = true;
    if (const auto* ovr = findWallpaperMonitorOverride(m_config->config().wallpaper, output);
        ovr != nullptr && ovr->enabled) {
      enabled = *ovr->enabled;
    }
    if (!enabled) {
      kLog.info("skipping {} ({}) — disabled by monitor override", output.connectorName, output.description);
      continue;
    }

    createInstance(output);
  }
}

void Wallpaper::resetAutomationState() {
  m_lastAutomationMinuteStamp = -1;
  m_lastAutomationSwitchMinute = -1;
}

void Wallpaper::runAutomation(std::int64_t minuteStamp) {
  const auto& wallpaper = m_config->config().wallpaper;
  const auto& automation = wallpaper.automation;
  if (!automation.enabled || automation.intervalMinutes <= 0 || m_instances.empty()) {
    return;
  }

  if (m_lastAutomationSwitchMinute >= 0 &&
      (minuteStamp - m_lastAutomationSwitchMinute) < static_cast<std::int64_t>(automation.intervalMinutes)) {
    return;
  }

  const ThemeMode mode = wallpaper.perMonitorDirectories
                             ? (m_config->config().theme.mode == ThemeMode::Light ? ThemeMode::Light : ThemeMode::Dark)
                             : ThemeMode::Dark;

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
      const std::string dir =
          output != nullptr ? resolveWallpaperDirectory(wallpaper, *output, mode) : wallpaper.directory;
      collectWallpaperCandidates(dir, automation.recursive, candidates);
      if (candidates.empty()) {
        continue;
      }
      const std::string currentPath = m_config->getWallpaperPath(inst->connectorName);
      const std::string picked = automation.order == WallpaperAutomationConfig::Order::Alphabetical
                                     ? pickAlphabeticalWallpaperPath(candidates, currentPath)
                                     : pickRandomWallpaperPath(candidates, currentPath);
      if (picked.empty() || picked == currentPath) {
        continue;
      }
      m_config->setWallpaperPath(inst->connectorName, picked);
      kLog.info("automation set {} → {}", inst->connectorName, picked);
    }
  } else {
    std::vector<std::string> candidates;
    collectWallpaperCandidates(wallpaper.directory, automation.recursive, candidates);
    if (!candidates.empty()) {
      const std::string currentDefault = m_config->getDefaultWallpaperPath();
      const std::string picked = automation.order == WallpaperAutomationConfig::Order::Alphabetical
                                     ? pickAlphabeticalWallpaperPath(candidates, currentDefault)
                                     : pickRandomWallpaperPath(candidates, currentDefault);
      if (!picked.empty()) {
        for (const auto& inst : m_instances) {
          if (!inst->connectorName.empty()) {
            m_config->setWallpaperPath(inst->connectorName, picked);
          }
        }
        m_config->setWallpaperPath(std::nullopt, picked);
        kLog.info("automation set all outputs → {}", picked);
      }
    }
  }
  m_lastAutomationSwitchMinute = minuteStamp;
}

bool Wallpaper::switchToRandomWallpaper(std::optional<std::string_view> connector) {
  if (m_config == nullptr || !m_config->config().wallpaper.enabled || m_instances.empty()) {
    return false;
  }

  const auto& wallpaper = m_config->config().wallpaper;
  const ThemeMode mode = wallpaper.perMonitorDirectories
                             ? (m_config->config().theme.mode == ThemeMode::Light ? ThemeMode::Light : ThemeMode::Dark)
                             : ThemeMode::Dark;

  if (connector.has_value()) {
    if (m_wayland != nullptr) {
      const auto& outputs = m_wayland->outputs();
      const bool found = std::any_of(outputs.begin(), outputs.end(), [&](const WaylandOutput& out) {
        return !out.connectorName.empty() && out.connectorName == *connector;
      });
      if (!found) {
        return false;
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
      return false;
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
    const std::string dir =
        output != nullptr ? resolveWallpaperDirectory(wallpaper, *output, mode) : wallpaper.directory;
    collectWallpaperCandidates(dir, wallpaper.automation.recursive, candidates);
    if (candidates.empty()) {
      return false;
    }
    const std::string currentPath = m_config->getWallpaperPath(std::string(*connector));
    const std::string picked = pickRandomWallpaperPath(candidates, currentPath);
    if (picked.empty() || picked == currentPath) {
      return false;
    }
    m_config->setWallpaperPath(std::string(*connector), picked);
    kLog.info("ipc set {} → {}", *connector, picked);
    return true;
  }

  ConfigService::WallpaperBatch batch(*m_config);
  bool anyChanged = false;

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
      const std::string dir =
          output != nullptr ? resolveWallpaperDirectory(wallpaper, *output, mode) : wallpaper.directory;
      collectWallpaperCandidates(dir, wallpaper.automation.recursive, candidates);
      if (candidates.empty()) {
        continue;
      }
      const std::string currentPath = m_config->getWallpaperPath(inst->connectorName);
      const std::string picked = pickRandomWallpaperPath(candidates, currentPath);
      if (picked.empty() || picked == currentPath) {
        continue;
      }
      m_config->setWallpaperPath(inst->connectorName, picked);
      kLog.info("ipc set {} → {}", inst->connectorName, picked);
      anyChanged = true;
    }
  } else {
    std::vector<std::string> candidates;
    collectWallpaperCandidates(wallpaper.directory, wallpaper.automation.recursive, candidates);
    if (!candidates.empty()) {
      const std::string currentDefault = m_config->getDefaultWallpaperPath();
      const std::string picked = pickRandomWallpaperPath(candidates, currentDefault);
      if (!picked.empty()) {
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

  return anyChanged;
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

  instance->sceneRoot = std::make_unique<Node>();
  instance->sceneRoot->setAnimationManager(&instance->animations);
  auto fillNode = std::make_unique<Box>();
  instance->fillNode = static_cast<Box*>(instance->sceneRoot->addChild(std::move(fillNode)));
  auto wallpaperNode = std::make_unique<WallpaperNode>();
  instance->wallpaperNode = static_cast<WallpaperNode*>(instance->sceneRoot->addChild(std::move(wallpaperNode)));
  instance->surface->setSceneRoot(instance->sceneRoot.get());

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst, wallpaperPath](std::uint32_t width, std::uint32_t height) {
    const float sw = static_cast<float>(width);
    const float sh = static_cast<float>(height);
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
  m_textureCache->release(inst.currentTexture, inst.currentPath);
  m_textureCache->release(inst.nextTexture, inst.pendingPath);
}

// ── Wallpaper loading & transitions ──────────────────────────────────────────

void Wallpaper::loadWallpaper(WallpaperInstance& instance, const std::string& path) {
  // Nothing to do if we're already at (or heading toward) this wallpaper.
  if (!instance.transitioning && path == instance.currentPath) {
    return;
  }
  if (instance.transitioning && path == instance.pendingPath) {
    return;
  }

  if (instance.transitioning) {
    instance.queuedPath = path;
    return;
  }

  TextureHandle newTex;
  Color newColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  WallpaperSourceKind newSourceKind = WallpaperSourceKind::Image;
  if (parseColorWallpaperPath(path, newColor)) {
    newSourceKind = WallpaperSourceKind::Color;
  } else {
    newTex = m_textureCache->acquire(path);
    if (newTex.id == 0) {
      kLog.warn("failed to load {}", path);
      return;
    }
  }

  if (instance.currentPath.empty()) {
    // First wallpaper — display immediately, no transition
    instance.currentSourceKind = newSourceKind;
    instance.currentTexture = newTex;
    instance.currentColor = newColor;
    instance.currentPath = path;
    instance.pendingPath.clear();
    instance.queuedPath.clear();
    updateRendererState(instance);
    instance.surface->requestRedraw();
    return;
  }

  instance.nextSourceKind = newSourceKind;
  instance.nextTexture = newTex;
  instance.nextColor = newColor;
  instance.pendingPath = path;
  startTransition(instance);
}

void Wallpaper::startTransition(WallpaperInstance& instance) {
  const auto& wpConfig = m_config->config().wallpaper;

  float aspectRatio = 1.777f;
  if (instance.surface->height() > 0) {
    aspectRatio = static_cast<float>(instance.surface->width()) / static_cast<float>(instance.surface->height());
  }

  const auto& transitions = wpConfig.transitions;
  const auto picked =
      transitions[static_cast<std::size_t>(std::floor(randomFloat(0.0f, static_cast<float>(transitions.size()))))];
  instance.activeTransition = picked;
  instance.transitionParams = randomizeParams(picked, wpConfig.edgeSmoothness, aspectRatio);
  instance.transitioning = true;
  instance.transitionProgress = 0.0f;

  auto* inst = &instance;
  instance.transitionAnimId = instance.animations.animateUnscaled(
      0.0f, 1.0f, wpConfig.transitionDurationMs, Easing::EaseInOutCubic,
      [inst](float v) { inst->transitionProgress = v; },
      [this, inst]() {
        // Transition complete — release old current, promote next to current
        m_textureCache->release(inst->currentTexture, inst->currentPath);
        inst->currentSourceKind = inst->nextSourceKind;
        inst->currentTexture = inst->nextTexture;
        inst->currentColor = inst->nextColor;
        inst->nextTexture = {};
        inst->nextSourceKind = WallpaperSourceKind::Image;
        inst->nextColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
        inst->currentPath = inst->pendingPath;
        inst->pendingPath.clear();
        inst->transitionProgress = 0.0f;
        inst->transitioning = false;
        updateRendererState(*inst);
        // The frame loop stops once there are no active animations, so the
        // promoted final wallpaper needs one explicit redraw.
        inst->surface->requestRedraw();

        if (!inst->queuedPath.empty() && inst->queuedPath != inst->currentPath) {
          const std::string queuedPath = inst->queuedPath;
          inst->queuedPath.clear();
          loadWallpaper(*inst, queuedPath);
        } else {
          inst->queuedPath.clear();
        }
      }
  );

  updateRendererState(instance);
  instance.surface->requestRedraw();
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
  wallpaperNode->setTransition(instance.activeTransition, instance.transitionProgress, instance.transitionParams);
  wallpaperNode->setFillMode(wpConfig.fillMode);
  wallpaperNode->setFillColor(fillColor);
}
