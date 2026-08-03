#include "shell/greeter/greeter_appearance_sync.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/polkit/polkit_session_support.h"
#include "ipc/ipc_service.h"
#include "render/core/color.h"
#include "shell/session/session_action_meta.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("greeter-sync");

  constexpr std::string_view kApplyHelperName = "noctalia-greeter-apply-appearance";
  constexpr std::string_view kGreeterName = "noctalia-greeter";
  constexpr std::string_view kGreeterTomlFileName = "greeter.toml";
  constexpr std::string_view kDefaultGreeterStateDir = "/var/lib/noctalia-greeter";
  constexpr std::string_view kGreeterStateDirEnv = "NOCTALIA_GREETER_STATE_DIR";
  constexpr std::string_view kStagedOutputLayoutFileName = "output_layout";
  constexpr std::string_view kStagedOutputTransformsFileName = "output_transforms";
  constexpr std::string_view kStagedOutputScalesFileName = "output_scales";
  // Staged sync.toml fragment (appearance + session); apply helper merges into live sync.toml.
  constexpr std::string_view kStagedSyncTomlFileName = "sync.toml";

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

  void putPaletteColor(toml::table& palette, std::string_view key, const Color& color) {
    palette.insert_or_assign(std::string(key), formatRgbHex(color));
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

  [[nodiscard]] std::string sanitizeConnectorFileToken(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const unsigned char c : name) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('_');
      }
    }
    return out.empty() ? std::string("unknown") : out;
  }

  // Fallback single wallpaper when no per-output map is usable.
  [[nodiscard]] std::string resolveSyncWallpaperPath(const ConfigService& configService) {
    if (const auto output = readGreeterConfiguredOutput(); output.has_value() && !output->empty()) {
      const std::string path = configService.getWallpaperPath(*output);
      if (!path.empty()) {
        return path;
      }
    }
    const std::string defaultPath = configService.getDefaultWallpaperPath();
    if (!defaultPath.empty()) {
      return defaultPath;
    }
    return configService.getGreeterSyncWallpaperPath();
  }

  struct StagedOutputWallpaper {
    std::string connector;
    std::string installedName;
    std::string sourcePath;
  };

  // Stage every per-monitor wallpaper so the greeter can pick by connector name.
  // File wallpapers are copied as wallpaper-<connector>.*; solid color: specs are
  // kept as sourcePath only (no file) so the wallpapers map can still reference them.
  [[nodiscard]] std::vector<StagedOutputWallpaper>
  stageAllOutputWallpapers(const std::filesystem::path& staging, const ConfigService& configService) {
    std::vector<StagedOutputWallpaper> staged;
    for (const auto& [connector, sourcePath] : configService.monitorWallpaperPaths()) {
      if (connector.empty() || sourcePath.empty()) {
        continue;
      }
      if (sourcePath.starts_with("color:")) {
        staged.push_back(StagedOutputWallpaper{connector, /*installedName=*/{}, sourcePath});
        kLog.info("greeter sync: color wallpaper for '{}' -> {}", connector, sourcePath);
        continue;
      }
      std::error_code ec;
      const std::filesystem::path source(sourcePath);
      if (!std::filesystem::is_regular_file(source, ec) || ec) {
        kLog.warn("greeter sync: skip missing wallpaper for '{}': {}", connector, sourcePath);
        continue;
      }
      const std::string extension = source.extension().string();
      const std::string installedName =
          "wallpaper-" + sanitizeConnectorFileToken(connector) + (extension.empty() ? "" : extension);
      const auto destination = staging / installedName;
      std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        kLog.warn("greeter sync: failed to stage wallpaper for '{}': {}", connector, ec.message());
        continue;
      }
      kLog.info("greeter sync: staged wallpaper for '{}' -> {}", connector, installedName);
      staged.push_back(StagedOutputWallpaper{connector, installedName, sourcePath});
    }
    // unordered_map iteration order is unspecified; keep map/fallback deterministic.
    std::ranges::sort(staged, [](const StagedOutputWallpaper& a, const StagedOutputWallpaper& b) {
      return a.connector < b.connector;
    });
    return staged;
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

  void putOptionalString(toml::table& table, std::string_view key, const std::optional<std::string>& value) {
    if (!value.has_value() || value->empty()) {
      return;
    }
    table.insert_or_assign(std::string(key), *value);
  }

  void appendSessionToSyncToml(toml::table& root, const ShellSessionConfig& session) {
    toml::table sessionTable;
    toml::table power;
    putOptionalString(power, "suspend", session.power.suspend);
    putOptionalString(power, "reboot", session.power.reboot);
    putOptionalString(power, "shutdown", session.power.shutdown);
    if (!power.empty()) {
      sessionTable.insert("power", std::move(power));
    }

    toml::array actions;
    const auto& source = session.actions.empty() ? defaultSessionPanelActions() : session.actions;
    for (const SessionPanelActionConfig& row : source) {
      if (!row.enabled || !session_action::isKnown(row.action)) {
        continue;
      }
      if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
        continue;
      }

      toml::table item;
      item.insert_or_assign("action", row.action);
      putOptionalString(item, "command", row.command);
      putOptionalString(item, "label", row.label);
      putOptionalString(item, "glyph", row.glyph);
      if (row.variant != SessionActionButtonVariant::Default) {
        item.insert_or_assign("variant", std::string(enumToKey(kSessionActionButtonVariants, row.variant)));
      }
      actions.push_back(std::move(item));
    }
    if (!actions.empty()) {
      sessionTable.insert("actions", std::move(actions));
    }
    if (!sessionTable.empty()) {
      root.insert("session", std::move(sessionTable));
    }
  }

  [[nodiscard]] std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }

  // Stages a sync.toml fragment (appearance + session) for noctalia-greeter-apply-appearance.
  [[nodiscard]] bool writeStagedSyncToml(
      const std::filesystem::path& staging, const Config& config, std::string_view resolvedMode,
      const std::string& wallpaperPath, const std::string& installedWallpaperName,
      const std::vector<StagedOutputWallpaper>& outputWallpapers
  ) {
    toml::table root;
    toml::table appearance;
    appearance.insert_or_assign("scheme", "Synced");
    appearance.insert_or_assign("theme_mode", std::string(resolvedMode));
    appearance.insert_or_assign("corner_radius_scale", static_cast<double>(config.shell.cornerRadiusScale));
    if (!config.shell.fontFamily.empty()) {
      appearance.insert_or_assign("font_family", config.shell.fontFamily);
    }

    toml::table palette;
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
    appearance.insert("palette", std::move(palette));

    toml::table wallpaper;
    if (!installedWallpaperName.empty()) {
      wallpaper.insert_or_assign(
          "path", (std::filesystem::path(kDefaultGreeterStateDir) / installedWallpaperName).string()
      );
    } else if (!wallpaperPath.empty()) {
      wallpaper.insert_or_assign("path", wallpaperPath);
    }
    const std::string fillMode = std::string(enumToKey(kWallpaperFillModes, config.wallpaper.fillMode));
    wallpaper.insert_or_assign("fill_mode", fillMode);
    const Color fillColor = resolveWallpaperFillColor(config.wallpaper);
    if (fillColor.a > 0.0f) {
      wallpaper.insert_or_assign("fill_color", formatRgbHex(fillColor));
    }
    if (!wallpaper.empty()) {
      appearance.insert("wallpaper", std::move(wallpaper));
    }

    if (!outputWallpapers.empty()) {
      toml::table byOutput;
      for (const auto& entry : outputWallpapers) {
        toml::table item;
        if (!entry.installedName.empty()) {
          item.insert_or_assign(
              "path", (std::filesystem::path(kDefaultGreeterStateDir) / entry.installedName).string()
          );
        } else if (!entry.sourcePath.empty()) {
          item.insert_or_assign("path", entry.sourcePath);
        } else {
          continue;
        }
        item.insert_or_assign("fill_mode", fillMode);
        if (fillColor.a > 0.0f) {
          item.insert_or_assign("fill_color", formatRgbHex(fillColor));
        }
        byOutput.insert(entry.connector, std::move(item));
      }
      if (!byOutput.empty()) {
        appearance.insert("wallpapers", std::move(byOutput));
      }
    }

    root.insert("appearance", std::move(appearance));
    appendSessionToSyncToml(root, config.shell.session);

    const auto syncPath = staging / kStagedSyncTomlFileName;
    std::ofstream out(syncPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged sync.toml '{}'", syncPath.string());
      return false;
    }
    out << "# noctalia-greeter staged sync.toml (merged into live sync.toml by apply-appearance)\n\n";
    out << formatToml(root);
    if (!out.good()) {
      kLog.warn("failed to write staged sync.toml '{}'", syncPath.string());
      return false;
    }
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

  [[nodiscard]] std::optional<std::string> greeterTransformToken(std::int32_t transform) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      return std::string("normal");
    case WL_OUTPUT_TRANSFORM_90:
      return std::string("90");
    case WL_OUTPUT_TRANSFORM_180:
      return std::string("180");
    case WL_OUTPUT_TRANSFORM_270:
      return std::string("270");
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      return std::string("flipped");
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      return std::string("flipped-90");
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      return std::string("flipped-180");
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      return std::string("flipped-270");
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputTransforms(const CompositorPlatform& platform) {
    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output transforms sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty()) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.empty()) {
      kLog.info("greeter sync: no ready outputs; skipping output transforms sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      return lhs->connectorName < rhs->connectorName;
    });

    std::string transforms;
    for (const WaylandOutput* output : ready) {
      const auto token = greeterTransformToken(output->transform);
      if (!token.has_value()) {
        kLog.warn(
            "greeter sync: output '{}' has unknown transform {}; skipping output transforms sync",
            output->connectorName, output->transform
        );
        return std::nullopt;
      }
      if (!transforms.empty()) {
        transforms += "; ";
      }
      transforms += output->connectorName + ':' + *token;
    }
    return transforms;
  }

  [[nodiscard]] std::optional<float> detectedLayoutScale(const WaylandOutput& output) {
    if (output.width <= 0 || output.height <= 0 || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      return std::nullopt;
    }

    const auto physicalW = static_cast<double>(output.width);
    const auto physicalH = static_cast<double>(output.height);
    const auto logicalW = static_cast<double>(output.logicalWidth);
    const auto logicalH = static_cast<double>(output.logicalHeight);

    const auto candidate = [](double xScale, double yScale) {
      return std::pair{(xScale + yScale) * 0.5, std::abs(xScale - yScale)};
    };
    const auto normal = candidate(physicalW / logicalW, physicalH / logicalH);
    const auto rotated = candidate(physicalW / logicalH, physicalH / logicalW);
    const double scale = rotated.second < normal.second ? rotated.first : normal.first;
    if (scale < 1.0) {
      return std::nullopt;
    }
    return static_cast<float>(scale);
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputScales(const CompositorPlatform& platform) {
    if (!platform.wayland().hasXdgOutputManager()) {
      kLog.info("greeter sync: xdg-output unavailable; skipping output scales sync");
      return std::nullopt;
    }

    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output scales sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty()) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.empty()) {
      kLog.info("greeter sync: no ready outputs; skipping output scales sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      return lhs->connectorName < rhs->connectorName;
    });

    std::string scales;
    for (const WaylandOutput* output : ready) {
      const auto scale = detectedLayoutScale(*output);
      if (!scale.has_value()) {
        kLog.warn(
            "greeter sync: output '{}' has unknown scale (logical={}x{} mode={}x{}); skipping output scales sync",
            output->connectorName, output->logicalWidth, output->logicalHeight, output->width, output->height
        );
        return std::nullopt;
      }
      if (!scales.empty()) {
        scales += "; ";
      }
      scales += output->connectorName + ':' + std::format("{:.3f}", *scale);
    }
    return scales;
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
    if (const auto transforms = buildGreeterOutputTransforms(platform)) {
      kLog.info("greeter sync: staging output_transforms \"{}\"", *transforms);
    }
    if (const auto scales = buildGreeterOutputScales(platform)) {
      kLog.info("greeter sync: staging output_scales \"{}\"", *scales);
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

  [[nodiscard]] bool stageOutputTransforms(const std::filesystem::path& staging, std::string_view transforms) {
    const auto transformsPath = staging / kStagedOutputTransformsFileName;
    std::ofstream out(transformsPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output transforms '{}'", transformsPath.string());
      return false;
    }
    out << transforms << '\n';
    return true;
  }

  [[nodiscard]] bool stageOutputScales(const std::filesystem::path& staging, std::string_view scales) {
    const auto scalesPath = staging / kStagedOutputScalesFileName;
    std::ofstream out(scalesPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output scales '{}'", scalesPath.string());
      return false;
    }
    out << scales << '\n';
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
      if (const auto transforms = buildGreeterOutputTransforms(*platform)) {
        if (!stageOutputTransforms(staging, *transforms)) {
          finish(false);
          return GreeterSyncLaunch::Failed;
        }
      }
      if (const auto scales = buildGreeterOutputScales(*platform)) {
        if (!stageOutputScales(staging, *scales)) {
          finish(false);
          return GreeterSyncLaunch::Failed;
        }
      }
    } else {
      kLog.info("greeter sync: no compositor platform provided; skipping output layout/transforms/scales sync");
    }

    const auto outputWallpapers = stageAllOutputWallpapers(staging, configService);
    std::string wallpaperPath = resolveSyncWallpaperPath(configService);
    std::string installedWallpaperName = stageWallpaper(staging, wallpaperPath);
    // Prefer a staged per-output entry for the legacy single wallpaper when needed.
    if (installedWallpaperName.empty() && !outputWallpapers.empty()) {
      auto preferEntry = [&](const StagedOutputWallpaper& entry) {
        wallpaperPath = entry.sourcePath;
        installedWallpaperName = entry.installedName;
      };
      bool pinResolved = false;
      if (const auto pin = readGreeterConfiguredOutput(); pin.has_value() && !pin->empty()) {
        for (const auto& entry : outputWallpapers) {
          if (entry.connector == *pin) {
            preferEntry(entry);
            pinResolved = true;
            break;
          }
        }
      }
      // Pin hit (file or color:): keep it. Otherwise pick first staged file, else first entry.
      if (!pinResolved) {
        for (const auto& entry : outputWallpapers) {
          if (!entry.installedName.empty()) {
            preferEntry(entry);
            break;
          }
        }
        if (installedWallpaperName.empty() && wallpaperPath.empty()) {
          preferEntry(outputWallpapers.front());
        }
      }
    }
    if (!writeStagedSyncToml(
            staging, config, resolvedThemeMode, wallpaperPath, installedWallpaperName, outputWallpapers
        )) {
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
    if (!appearanceSyncAvailable(config.config().shell.greeterSync)) {
      return;
    }
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
        "", "Sync wallpaper, colors, and monitor layout to Noctalia Greeter",
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
  }

} // namespace greeter
