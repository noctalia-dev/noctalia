#include "shell/greeter/greeter_appearance_sync.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/process.h"
#include "dbus/polkit/polkit_session_support.h"
#include "ipc/ipc_service.h"
#include "render/core/color.h"
#include "shell/session/session_action_meta.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <toml.hpp>
#include <vector>

namespace {

  constexpr Logger kLog("greeter-sync");

  constexpr std::string_view kApplyHelperName = "noctalia-greeter-apply-appearance";
  constexpr std::string_view kGreeterName = "noctalia-greeter";
  constexpr std::string_view kGreeterTomlFileName = "greeter.toml";
  constexpr std::string_view kDefaultGreeterStateDir = "/var/lib/noctalia-greeter";
  constexpr std::string_view kGreeterStateDirEnv = "NOCTALIA_GREETER_STATE_DIR";
  constexpr std::string_view kStagedOutputLayoutFileName = "output_layout";

  [[nodiscard]] std::string
  resolveProgramPath(std::string_view name, std::initializer_list<const char*> fallbackPaths) {
    if (process::commandExists(std::string(name).c_str())) {
      return std::string(name);
    }
    for (const char* candidate : fallbackPaths) {
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec) && !ec) {
        return candidate;
      }
    }
    return {};
  }

  [[nodiscard]] bool programExists(std::string_view name, std::initializer_list<const char*> fallbackPaths) {
    return !resolveProgramPath(name, fallbackPaths).empty();
  }

  Color resolveWallpaperFillColor(const WallpaperConfig& config) {
    if (!config.fillColor) {
      return rgba(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return resolveColorSpec(*config.fillColor);
  }

  void putPaletteColor(nlohmann::json& palette, std::string_view key, const Color& color) {
    palette[std::string(key)] = formatRgbHex(color);
  }

  [[nodiscard]] std::filesystem::path greeterTomlPath() {
    const char* stateDir = std::getenv(kGreeterStateDirEnv.data());
    if (stateDir != nullptr && stateDir[0] != '\0') {
      return std::filesystem::path(stateDir) / kGreeterTomlFileName;
    }
    return std::filesystem::path(kDefaultGreeterStateDir) / kGreeterTomlFileName;
  }

  [[nodiscard]] std::optional<std::string> readGreeterConfiguredOutput() {
    const auto path = greeterTomlPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::nullopt;
    }

    try {
      const toml::table table = toml::parse_file(path.string());
      if (const toml::table* output = table["output"].as_table()) {
        const auto name = (*output)["name"].value<std::string>();
        if (name.has_value() && !name->empty()) {
          return name;
        }
      }
      return std::nullopt;
    } catch (const toml::parse_error& e) {
      kLog.warn("failed to parse {}: {}", path.string(), e.description());
      return std::nullopt;
    }
  }

  [[nodiscard]] std::string resolveSyncWallpaperPath(const ConfigService& configService) {
    const Config& config = configService.config();
    if (config.theme.source != PaletteSource::Wallpaper) {
      if (const auto output = readGreeterConfiguredOutput(); output.has_value() && !output->empty()) {
        const std::string path = configService.getWallpaperPath(*output);
        if (!path.empty()) {
          return path;
        }
      }
    }
    return configService.getGreeterSyncWallpaperPath();
  }

  [[nodiscard]] std::string findApplyHelper() {
    return resolveProgramPath(
        kApplyHelperName,
        {"/usr/bin/noctalia-greeter-apply-appearance", "/usr/local/bin/noctalia-greeter-apply-appearance"}
    );
  }

  [[nodiscard]] std::filesystem::path makeStagingDirectory() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path base = runtimeDir != nullptr && runtimeDir[0] != '\0'
        ? std::filesystem::path(runtimeDir)
        : std::filesystem::temp_directory_path();
    const auto staging = base / "noctalia-greeter-sync";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
      return {};
    }
    return staging;
  }

  void putOptionalString(nlohmann::json& object, std::string_view key, const std::optional<std::string>& value) {
    if (!value.has_value() || value->empty()) {
      return;
    }
    object[std::string(key)] = *value;
  }

  void appendSessionManifest(nlohmann::json& root, const ShellSessionConfig& session) {
    nlohmann::json sessionJson;
    nlohmann::json power;
    putOptionalString(power, "suspend", session.power.suspend);
    putOptionalString(power, "reboot", session.power.reboot);
    putOptionalString(power, "shutdown", session.power.shutdown);
    if (!power.empty()) {
      sessionJson["power"] = std::move(power);
    }

    nlohmann::json actions = nlohmann::json::array();
    const auto& source = session.actions.empty() ? defaultSessionPanelActions() : session.actions;
    for (const SessionPanelActionConfig& row : source) {
      if (!row.enabled || !session_action::isKnown(row.action)) {
        continue;
      }
      if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
        continue;
      }

      nlohmann::json item;
      item["action"] = row.action;
      putOptionalString(item, "command", row.command);
      putOptionalString(item, "label", row.label);
      putOptionalString(item, "glyph", row.glyph);
      if (row.variant != SessionActionButtonVariant::Default) {
        item["variant"] = std::string(enumToKey(kSessionActionButtonVariants, row.variant));
      }
      actions.push_back(std::move(item));
    }
    sessionJson["actions"] = std::move(actions);
    root["session"] = std::move(sessionJson);
  }

  [[nodiscard]] bool writeManifest(
      const std::filesystem::path& staging, const Config& config, std::string_view resolvedMode,
      const std::string& wallpaperPath, const std::string& installedWallpaperName
  ) {
    nlohmann::json root;
    root["version"] = 1;
    root["theme_mode"] = resolvedMode;

    nlohmann::json palette;
    putPaletteColor(palette, "primary", ::palette.primary);
    putPaletteColor(palette, "on_primary", ::palette.onPrimary);
    putPaletteColor(palette, "secondary", ::palette.secondary);
    putPaletteColor(palette, "on_secondary", ::palette.onSecondary);
    putPaletteColor(palette, "tertiary", ::palette.tertiary);
    putPaletteColor(palette, "on_tertiary", ::palette.onTertiary);
    putPaletteColor(palette, "error", ::palette.error);
    putPaletteColor(palette, "on_error", ::palette.onError);
    putPaletteColor(palette, "surface", ::palette.surface);
    putPaletteColor(palette, "on_surface", ::palette.onSurface);
    putPaletteColor(palette, "surface_variant", ::palette.surfaceVariant);
    putPaletteColor(palette, "on_surface_variant", ::palette.onSurfaceVariant);
    putPaletteColor(palette, "outline", ::palette.outline);
    putPaletteColor(palette, "shadow", ::palette.shadow);
    putPaletteColor(palette, "hover", ::palette.hover);
    putPaletteColor(palette, "on_hover", ::palette.onHover);
    root["palette"] = std::move(palette);

    nlohmann::json wallpaper;
    if (!installedWallpaperName.empty()) {
      wallpaper["path"] = (std::filesystem::path("/var/lib/noctalia-greeter") / installedWallpaperName).string();
    } else if (!wallpaperPath.empty()) {
      wallpaper["path"] = wallpaperPath;
    }
    wallpaper["fill_mode"] = std::string(enumToKey(kWallpaperFillModes, config.wallpaper.fillMode));
    const Color fillColor = resolveWallpaperFillColor(config.wallpaper);
    if (fillColor.a > 0.0f) {
      wallpaper["fill_color"] = formatRgbHex(fillColor);
    }
    root["wallpaper"] = std::move(wallpaper);
    root["corner_radius_scale"] = config.shell.cornerRadiusScale;
    appendSessionManifest(root, config.shell.session);

    const auto manifestPath = staging / "appearance.json";
    std::ofstream out(manifestPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staging manifest '{}'", manifestPath.string());
      return false;
    }
    out << root.dump(2) << '\n';
    return true;
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputLayout(const CompositorPlatform& platform) {
    if (!platform.wayland().hasXdgOutputManager()) {
      kLog.info("greeter sync: xdg-output unavailable; skipping output layout sync");
      return std::nullopt;
    }

    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output layout sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty() || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.size() < 2) {
      kLog.info("greeter sync: {} ready output(s); skipping output layout sync", ready.size());
      return std::nullopt;
    }

    const int originX = ready.front()->logicalX;
    const int originY = ready.front()->logicalY;
    const bool allShareOrigin = std::ranges::all_of(ready, [originX, originY](const WaylandOutput* output) {
      return output->logicalX == originX && output->logicalY == originY;
    });
    if (allShareOrigin) {
      kLog.info("greeter sync: ready outputs share the same origin; skipping output layout sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      if (lhs->logicalX != rhs->logicalX) {
        return lhs->logicalX < rhs->logicalX;
      }
      if (lhs->logicalY != rhs->logicalY) {
        return lhs->logicalY < rhs->logicalY;
      }
      return lhs->connectorName < rhs->connectorName;
    });

    std::string layout;
    for (const WaylandOutput* output : ready) {
      if (!layout.empty()) {
        layout += "; ";
      }
      layout += output->connectorName + ':' + std::to_string(output->logicalX) + ',' + std::to_string(output->logicalY);
    }
    return layout;
  }

  void logOutputLayoutForGreeter(const CompositorPlatform& platform) {
    const auto& outputs = platform.outputs();
    if (outputs.empty()) {
      kLog.info("greeter sync: no Wayland outputs available for layout logging");
      return;
    }

    kLog.info("greeter sync: {} Wayland output(s) (logical layout from compositor):", outputs.size());
    for (const auto& output : outputs) {
      kLog.info(
          "  output connector='{}' description='{}' done={} logical=({}, {}) {}x{} physical={}x{} scale={} "
          "transform={}",
          output.connectorName.empty() ? "?" : output.connectorName,
          output.description.empty() ? "?" : output.description, output.done, output.logicalX, output.logicalY,
          output.logicalWidth, output.logicalHeight, output.width, output.height, output.scale, output.transform
      );
    }

    if (const auto layout = buildGreeterOutputLayout(platform)) {
      kLog.info("greeter sync: staging output_layout \"{}\"", *layout);
    }
  }

  [[nodiscard]] bool stageOutputLayout(const std::filesystem::path& staging, std::string_view layout) {
    const auto layoutPath = staging / kStagedOutputLayoutFileName;
    std::ofstream out(layoutPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output layout '{}'", layoutPath.string());
      return false;
    }
    out << layout << '\n';
    return true;
  }

  [[nodiscard]] std::string stageWallpaper(const std::filesystem::path& staging, std::string_view sourcePath) {
    if (sourcePath.empty()) {
      return {};
    }
    if (sourcePath.starts_with("color:")) {
      return {};
    }

    std::error_code ec;
    const std::filesystem::path source(sourcePath);
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
      return {};
    }

    const std::string extension = source.extension().string();
    const std::string installedName = extension.empty() ? "wallpaper" : "wallpaper" + extension;
    const auto destination = staging / installedName;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      kLog.warn("failed to stage wallpaper '{}': {}", source.string(), ec.message());
      return {};
    }
    return installedName;
  }

  [[nodiscard]] bool hasPrivilegeCommandOverride(const ShellGreeterSyncConfig& greeterSync) {
    return !StringUtils::trim(greeterSync.privilegeCommand).empty();
  }

  [[nodiscard]] std::string privilegeCommandPrefix(const ShellGreeterSyncConfig& greeterSync) {
    return StringUtils::trim(greeterSync.privilegeCommand);
  }

  [[nodiscard]] std::string
  buildPrivilegedApplyCommand(std::string_view privilegePrefix, std::string_view helper, std::string_view staging) {
    return std::string(privilegePrefix)
        + " "
        + StringUtils::shellQuote(helper)
        + " "
        + StringUtils::shellQuote(staging);
  }

  [[nodiscard]] bool launchPrivilegedApplyHelper(
      const ShellGreeterSyncConfig& greeterSync, std::string_view helper, const std::filesystem::path& staging,
      process::RunCallbacks callbacks
  ) {
    if (hasPrivilegeCommandOverride(greeterSync)) {
      const std::string command =
          buildPrivilegedApplyCommand(privilegeCommandPrefix(greeterSync), helper, staging.string());
      return process::runAsync(command, std::move(callbacks));
    }

    const std::string escalator = process::resolvePrivilegeEscalator().value_or(std::string{});
    if (escalator.empty()) {
      return false;
    }
    return process::runAsync(
        std::vector<std::string>{escalator, std::string(helper), staging.string()}, std::move(callbacks)
    );
  }

} // namespace

namespace greeter {

  bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept {
    return programExists(kGreeterName, {"/usr/bin/noctalia-greeter", "/usr/local/bin/noctalia-greeter"})
        && !findApplyHelper().empty()
        && (process::resolvePrivilegeEscalator().has_value() || hasPrivilegeCommandOverride(greeterSync));
  }

  GreeterSyncLaunch syncAppearanceToGreeterAsync(
      const ConfigService& configService, std::string_view resolvedThemeMode, SyncCompletion onComplete,
      const CompositorPlatform* platform, const bool logindOnSystemBus
  ) {
    const auto completion = std::make_shared<SyncCompletion>(std::move(onComplete));
    const auto finish = [completion](bool success) {
      if (completion && *completion) {
        (*completion)(success);
      }
    };

    const auto helper = findApplyHelper();
    if (helper.empty()) {
      kLog.warn("greeter sync helper is not installed");
      finish(false);
      return GreeterSyncLaunch::Failed;
    }

    const Config& config = configService.config();
    const ShellGreeterSyncConfig& greeterSync = config.shell.greeterSync;
    if (!hasPrivilegeCommandOverride(greeterSync) && !process::resolvePrivilegeEscalator().has_value()) {
      kLog.warn("no privilege escalator available (pkexec or run0)");
      finish(false);
      return GreeterSyncLaunch::Failed;
    }

    const auto staging = makeStagingDirectory();
    if (staging.empty()) {
      kLog.warn("failed to create greeter sync staging directory");
      finish(false);
      return GreeterSyncLaunch::Failed;
    }

    if (platform != nullptr) {
      logOutputLayoutForGreeter(*platform);
      if (const auto layout = buildGreeterOutputLayout(*platform)) {
        if (!stageOutputLayout(staging, *layout)) {
          finish(false);
          return GreeterSyncLaunch::Failed;
        }
      }
    } else {
      kLog.info("greeter sync: no compositor platform provided; skipping output layout sync");
    }

    const std::string wallpaperPath = resolveSyncWallpaperPath(configService);
    const std::string installedWallpaperName = stageWallpaper(staging, wallpaperPath);
    if (!writeManifest(staging, config, resolvedThemeMode, wallpaperPath, installedWallpaperName)) {
      finish(false);
      return GreeterSyncLaunch::Failed;
    }

    if (!hasPrivilegeCommandOverride(greeterSync)
        && !polkit_session::likelySupportsInSessionPolkitAgent(logindOnSystemBus)) {
      kLog.info("greeter sync: staged; skipping background privilege escalation (no session polkit)");
      return GreeterSyncLaunch::StagedOnly;
    }

    process::RunCallbacks callbacks;
    callbacks.onExit = [finish](const process::RunResult& result) {
      if (!result) {
        if (!result.err.empty()) {
          kLog.warn("greeter sync failed: {}", result.err);
        } else {
          kLog.warn("greeter sync failed with exit code {}", result.exitCode);
        }
        finish(false);
        return;
      }
      kLog.info("synced shell appearance and session actions to greeter");
      finish(true);
    };
    if (!launchPrivilegedApplyHelper(greeterSync, helper, staging, std::move(callbacks))) {
      finish(false);
      return GreeterSyncLaunch::Failed;
    }
    return GreeterSyncLaunch::Launched;
  }

  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus
  ) {
    ipc.registerHandler(
        "greeter-sync",
        [&config, resolvedThemeMode = std::move(resolvedThemeMode), platform,
         logindOnSystemBus = std::move(logindOnSystemBus)](const std::string& args) -> std::string {
          if (!StringUtils::trim(args).empty()) {
            return "error: usage: greeter-sync\n";
          }
          if (!appearanceSyncAvailable(config.config().shell.greeterSync)) {
            return "error: noctalia greeter is not installed\n";
          }
          const bool logind = logindOnSystemBus != nullptr && logindOnSystemBus();
          const GreeterSyncLaunch launch =
              syncAppearanceToGreeterAsync(config, resolvedThemeMode(), {}, platform, logind);
          if (launch == GreeterSyncLaunch::Failed) {
            return "error: failed to start greeter appearance sync\n";
          }
          if (launch == GreeterSyncLaunch::StagedOnly) {
            return "ok: staged (run privilege install manually)\n";
          }
          return "ok\n";
        },
        "greeter-sync", "Sync wallpaper, colors, and monitor layout to Noctalia Greeter"
    );
  }

} // namespace greeter
