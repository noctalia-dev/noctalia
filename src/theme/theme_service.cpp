#include "theme/theme_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"
#include "util/string_utils.h"

#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace noctalia::theme {

  namespace {

    constexpr auto kLog = Logger("theme");

    constexpr float kTransitionDurationMs = 400.0f;
    constexpr std::chrono::milliseconds kTransitionTick{8};

    struct ResolvedTheme {
      GeneratedPalette generated;
      Palette palette;
      std::string mode;
    };

    // Returns "light" or "dark" from org.gnome.desktop.interface color-scheme.
    std::string_view readSystemColorScheme() {
      static GSettings* settings = []() -> GSettings* {
        GSettingsSchemaSource* source = g_settings_schema_source_get_default();
        if (source == nullptr)
          return nullptr;
        GSettingsSchema* schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE);
        if (schema == nullptr)
          return nullptr;
        const bool hasKey = g_settings_schema_has_key(schema, "color-scheme") != FALSE;
        g_settings_schema_unref(schema);
        if (!hasKey)
          return nullptr;
        return g_settings_new("org.gnome.desktop.interface");
      }();

      if (settings == nullptr)
        return "dark";
      gchar* raw = g_settings_get_string(settings, "color-scheme");
      if (raw == nullptr)
        return "dark";
      const bool isLight = (std::string_view(raw) == "prefer-light");
      g_free(raw);
      return isLight ? "light" : "dark";
    }

    std::string resolvedModeName(const ThemeConfig& cfg) {
      if (cfg.mode == ThemeMode::Auto)
        return std::string(readSystemColorScheme());
      return cfg.mode == ThemeMode::Light ? "light" : "dark";
    }

    ResolvedTheme resolveBuiltin(const ThemeConfig& cfg) {
      const auto* palette = findBuiltinPalette(cfg.builtinPalette);
      if (palette == nullptr) {
        kLog.warn("unknown builtin palette '{}', falling back to Noctalia", cfg.builtinPalette);
        palette = findBuiltinPalette("Noctalia");
      }
      const std::string mode = resolvedModeName(cfg);
      const GeneratedPalette generated = expandBuiltinPalette(*palette);
      return {
          .generated = generated,
          .palette = mapGeneratedPaletteMode(mode == "light" ? generated.light : generated.dark),
          .mode = mode,
      };
    }

    std::optional<ResolvedTheme> resolveWallpaper(const ThemeConfig& cfg, const std::string& wallpaperPath) {
      if (wallpaperPath.empty()) {
        kLog.warn("wallpaper theme requested but no wallpaper path set");
        return std::nullopt;
      }
      auto scheme = schemeFromString(cfg.wallpaperScheme);
      if (!scheme.has_value()) {
        kLog.warn("unknown wallpaper scheme '{}', falling back to m3-content", cfg.wallpaperScheme);
        scheme = Scheme::Content;
      }
      std::string err;
      auto image = loadAndResize(wallpaperPath, *scheme, &err);
      if (!image.has_value()) {
        kLog.warn("failed to load wallpaper '{}': {}", wallpaperPath, err);
        return std::nullopt;
      }
      auto generated = generate(image->rgb, *scheme, &err);
      if (generated.dark.empty()) {
        kLog.warn("failed to generate palette from wallpaper: {}", err);
        return std::nullopt;
      }
      const std::string mode = resolvedModeName(cfg);
      return ResolvedTheme{
          .generated = generated,
          .palette = mapGeneratedPaletteMode(mode == "light" ? generated.light : generated.dark),
          .mode = mode,
      };
    }

    // Reads a color key from a JSON object, looking first for the `m`-prefixed form
    // (e.g. `mPrimary`) and falling back to the unprefixed name. Returns fallback
    // (transparent black) if the key is missing or the value is not a hex string.
    Color readColorField(const nlohmann::json& obj, std::string_view camelField) {
      std::string prefixed = "m";
      prefixed.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(camelField[0]))));
      prefixed.append(camelField.substr(1));
      auto tryRead = [&](const std::string& key) -> std::optional<Color> {
        auto it = obj.find(key);
        if (it == obj.end() || !it->is_string()) {
          return std::nullopt;
        }
        try {
          return hex(it->get<std::string>());
        } catch (const std::exception&) {
          return std::nullopt;
        }
      };
      if (auto c = tryRead(prefixed))
        return *c;
      if (auto c = tryRead(std::string(camelField)))
        return *c;
      return Color{};
    }

    Palette readPaletteJson(const nlohmann::json& obj) {
      return Palette{
          .primary = readColorField(obj, "primary"),
          .onPrimary = readColorField(obj, "onPrimary"),
          .secondary = readColorField(obj, "secondary"),
          .onSecondary = readColorField(obj, "onSecondary"),
          .tertiary = readColorField(obj, "tertiary"),
          .onTertiary = readColorField(obj, "onTertiary"),
          .error = readColorField(obj, "error"),
          .onError = readColorField(obj, "onError"),
          .surface = readColorField(obj, "surface"),
          .onSurface = readColorField(obj, "onSurface"),
          .surfaceVariant = readColorField(obj, "surfaceVariant"),
          .onSurfaceVariant = readColorField(obj, "onSurfaceVariant"),
          .outline = readColorField(obj, "outline"),
          .shadow = readColorField(obj, "shadow"),
          .hover = readColorField(obj, "hover"),
          .onHover = readColorField(obj, "onHover"),
      };
    }

    TerminalAnsiColors readAnsiJson(const nlohmann::json& obj) {
      return TerminalAnsiColors{
          .black = readColorField(obj, kTerminalBlackJsonKey),
          .red = readColorField(obj, kTerminalRedJsonKey),
          .green = readColorField(obj, kTerminalGreenJsonKey),
          .yellow = readColorField(obj, kTerminalYellowJsonKey),
          .blue = readColorField(obj, kTerminalBlueJsonKey),
          .magenta = readColorField(obj, kTerminalMagentaJsonKey),
          .cyan = readColorField(obj, kTerminalCyanJsonKey),
          .white = readColorField(obj, kTerminalWhiteJsonKey),
      };
    }

    TerminalPalette readTerminalJson(const nlohmann::json& obj) {
      TerminalPalette tp{};
      if (auto it = obj.find(kTerminalNormalJsonKey); it != obj.end() && it->is_object()) {
        tp.normal = readAnsiJson(*it);
      }
      if (auto it = obj.find(kTerminalBrightJsonKey); it != obj.end() && it->is_object()) {
        tp.bright = readAnsiJson(*it);
      }
      tp.foreground = readColorField(obj, kTerminalForegroundJsonKey);
      tp.background = readColorField(obj, kTerminalBackgroundJsonKey);
      tp.selectionFg = readColorField(obj, kTerminalSelectionFgJsonKey);
      tp.selectionBg = readColorField(obj, kTerminalSelectionBgJsonKey);
      tp.cursorText = readColorField(obj, kTerminalCursorTextJsonKey);
      tp.cursor = readColorField(obj, kTerminalCursorJsonKey);
      return tp;
    }

    std::optional<TerminalPalette> readModeTerminalJson(const nlohmann::json& obj) {
      auto it = obj.find(kTerminalJsonKey);
      if (it == obj.end() || !it->is_object())
        return std::nullopt;
      return readTerminalJson(*it);
    }

    struct ParsedCommunityPalette {
      FixedPaletteMode dark;
      FixedPaletteMode light;
    };

    std::optional<ParsedCommunityPalette> parseCommunityPaletteJson(const std::filesystem::path& path) {
      try {
        std::ifstream in(path);
        if (!in)
          return std::nullopt;
        std::stringstream buf;
        buf << in.rdbuf();
        auto root = nlohmann::json::parse(buf.str());
        if (!root.is_object())
          return std::nullopt;
        ParsedCommunityPalette out{};
        if (auto it = root.find("dark"); it != root.end() && it->is_object()) {
          out.dark.palette = readPaletteJson(*it);
          if (auto terminal = readModeTerminalJson(*it)) {
            out.dark.terminal = *terminal;
          } else {
            return std::nullopt;
          }
        } else {
          return std::nullopt;
        }
        if (auto it = root.find("light"); it != root.end() && it->is_object()) {
          out.light.palette = readPaletteJson(*it);
          if (auto terminal = readModeTerminalJson(*it)) {
            out.light.terminal = *terminal;
          } else {
            return std::nullopt;
          }
        } else {
          out.light = out.dark;
        }
        return out;
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community palette '{}': {}", path.string(), e.what());
        return std::nullopt;
      }
    }

    ResolvedTheme makeResolvedFromParsed(const ParsedCommunityPalette& parsed, const ThemeConfig& cfg) {
      BuiltinPalette bp{
          .name = "community",
          .dark = parsed.dark,
          .light = parsed.light,
      };
      const std::string mode = resolvedModeName(cfg);
      const GeneratedPalette generated = expandBuiltinPalette(bp);
      return {
          .generated = generated,
          .palette = mapGeneratedPaletteMode(mode == "light" ? generated.light : generated.dark),
          .mode = mode,
      };
    }

  } // namespace

  ThemeService::ThemeService(ConfigService& config, HttpClient& httpClient)
      : m_config(config), m_httpClient(httpClient) {}

  void ThemeService::apply() { resolveAndSet(/*animate=*/false); }

  void ThemeService::onConfigReload() { resolveAndSet(/*animate=*/true); }

  void ThemeService::onWallpaperChange() {
    if (m_config.config().theme.source == PaletteSource::Wallpaper) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::onAutoSchemeChanged() {
    if (m_config.config().theme.mode == ThemeMode::Auto) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::toggleLightDark() {
    const auto next = m_isLightMode ? ThemeMode::Dark : ThemeMode::Light;
    // Persist via ConfigService → StateService. The resulting overrides-change
    // callback rebuilds the Config and fires the reload callbacks, which call
    // ThemeService::onConfigReload() to transition to the new palette.
    m_config.setThemeMode(next);
  }

  void ThemeService::cycleMode() {
    ThemeMode next = ThemeMode::Dark;
    switch (configuredMode()) {
    case ThemeMode::Dark:
      next = ThemeMode::Light;
      break;
    case ThemeMode::Light:
      next = ThemeMode::Auto;
      break;
    case ThemeMode::Auto:
      next = ThemeMode::Dark;
      break;
    }
    m_config.setThemeMode(next);
  }

  ThemeMode ThemeService::configuredMode() const noexcept { return m_config.config().theme.mode; }

  bool ThemeService::isLightMode() const noexcept { return m_isLightMode; }

  std::string_view ThemeService::resolvedMode() const noexcept { return m_isLightMode ? "light" : "dark"; }

  void ThemeService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  void ThemeService::setResolvedCallback(ResolvedCallback callback) { m_resolvedCallback = std::move(callback); }

  void ThemeService::queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode) {
    if (!m_resolvedCallback) {
      return;
    }
    m_pendingResolvedPalette = generated;
    m_pendingResolvedMode = std::string(mode);
    ++m_resolvedCallbackGeneration;
  }

  void ThemeService::flushResolvedCallback(bool defer) {
    if (!m_resolvedCallback || !m_pendingResolvedPalette.has_value()) {
      return;
    }

    GeneratedPalette generated = std::move(*m_pendingResolvedPalette);
    std::string mode = std::move(m_pendingResolvedMode);
    const std::uint64_t generation = m_resolvedCallbackGeneration;
    m_pendingResolvedPalette.reset();
    m_pendingResolvedMode.clear();

    if (defer) {
      DeferredCall::callLater([this, generation, generated = std::move(generated), mode = std::move(mode)]() mutable {
        if (generation == m_resolvedCallbackGeneration && m_resolvedCallback) {
          m_resolvedCallback(generated, mode);
        }
      });
      return;
    }

    m_resolvedCallback(generated, mode);
  }

  void ThemeService::startCommunityDownload(const std::string& name) {
    if (m_inflightCommunityName == name) {
      return;
    }
    m_inflightCommunityName = name;
    const auto cachePath = communityPaletteCachePath(name);
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    const std::string url = communityPaletteDownloadUrl(name);
    kLog.info("fetching community palette '{}' from {}", name, url);
    m_httpClient.download(url, cachePath, [this, name, cachePath](bool success) {
      if (m_inflightCommunityName == name) {
        m_inflightCommunityName.clear();
      }
      if (!success) {
        kLog.warn("community palette download failed for '{}'", name);
        return;
      }
      // Validate the just-downloaded file. If it parses, trigger a re-resolve.
      if (!parseCommunityPaletteJson(cachePath).has_value()) {
        kLog.warn("community palette '{}' downloaded but failed to parse; removing cache", name);
        std::error_code rmEc;
        std::filesystem::remove(cachePath, rmEc);
        return;
      }
      resolveAndSet(/*animate=*/true);
    });
  }

  void ThemeService::resolveAndSet(bool animate) {
    const auto& cfg = m_config.config().theme;
    std::optional<ResolvedTheme> resolved;
    if (cfg.source == PaletteSource::Custom && !cfg.customPalette.empty()) {
      const auto path = customPalettePath(cfg.customPalette);
      if (std::filesystem::exists(path)) {
        if (auto parsed = parseCommunityPaletteJson(path)) {
          resolved = makeResolvedFromParsed(*parsed, cfg);
        }
      }
      if (!resolved.has_value()) {
        kLog.warn("custom palette '{}' not found or invalid; falling back to builtin", cfg.customPalette);
      }
    } else if (cfg.source == PaletteSource::Wallpaper) {
      resolved = resolveWallpaper(cfg, m_config.getPaletteWallpaperPath());
    } else if (cfg.source == PaletteSource::Community && !cfg.communityPalette.empty()) {
      const auto cachePath = communityPaletteCachePath(cfg.communityPalette);
      if (std::filesystem::exists(cachePath)) {
        if (auto parsed = parseCommunityPaletteJson(cachePath)) {
          resolved = makeResolvedFromParsed(*parsed, cfg);
        } else {
          std::error_code rmEc;
          std::filesystem::remove(cachePath, rmEc);
        }
      }
      if (!resolved.has_value()) {
        startCommunityDownload(cfg.communityPalette);
      }
    }
    if (!resolved.has_value()) {
      resolved = resolveBuiltin(cfg);
    }

    queueResolvedCallback(resolved->generated, resolved->mode);
    m_isLightMode = resolved->mode == "light";

    if (animate) {
      startTransition(resolved->palette);
    } else {
      if (m_transitionAnimId == 0 && palette == resolved->palette) {
        flushResolvedCallback(/*defer=*/false);
        return;
      }
      if (m_transitionAnimId != 0) {
        m_animations.cancel(m_transitionAnimId);
        m_transitionAnimId = 0;
      }
      m_transitionTimer.stop();
      setPalette(resolved->palette);
      if (m_changeCallback) {
        m_changeCallback();
      }
      flushResolvedCallback(/*defer=*/false);
    }
  }

  void ThemeService::startTransition(const Palette& target) {
    if (m_transitionAnimId == 0 && palette == target) {
      flushResolvedCallback(/*defer=*/false);
      return;
    }
    if (m_transitionAnimId != 0 && m_targetPalette == target) {
      flushResolvedCallback(/*defer=*/true);
      return;
    }
    // Capture the currently-displayed palette (possibly mid-fade) so a new
    // transition starts from wherever the previous one had reached.
    if (m_transitionAnimId != 0) {
      m_animations.cancel(m_transitionAnimId);
      m_transitionAnimId = 0;
    }
    m_fromPalette = palette;
    m_targetPalette = target;
    m_transitionResolvedCallbackFlushed = false;
    m_transitionAnimId = m_animations.animate(
        0.0f, 1.0f, kTransitionDurationMs, Easing::EaseOutCubic,
        [this](float t) {
          setPalette(lerpPalette(m_fromPalette, m_targetPalette, t));
          if (m_changeCallback) {
            m_changeCallback();
          }
          if (!m_transitionResolvedCallbackFlushed) {
            m_transitionResolvedCallbackFlushed = true;
            flushResolvedCallback(/*defer=*/true);
          }
        },
        [this]() { finishTransition(/*deferResolvedCallback=*/false); }
    );
    if (m_transitionAnimId == 0) {
      m_transitionTimer.stop();
      return;
    }
    m_transitionTimer.startRepeating(kTransitionTick, [this] { tickTransition(); });
  }

  void ThemeService::finishTransition(bool deferResolvedCallback) {
    m_transitionAnimId = 0;
    m_transitionTimer.stop();
    setPalette(m_targetPalette);
    if (m_changeCallback) {
      m_changeCallback();
    }
    flushResolvedCallback(deferResolvedCallback);
  }

  void ThemeService::tickTransition() {
    if (!m_animations.hasActive()) {
      m_transitionTimer.stop();
      return;
    }
    m_animations.tick(static_cast<float>(kTransitionTick.count()));
  }

  void ThemeService::registerIpc(IpcService& ipc) {
    ipc.registerHandler(
        "theme-mode-toggle",
        [this](const std::string&) -> std::string {
          toggleLightDark();
          return "ok\n";
        },
        "theme-mode-toggle", "Toggle theme mode between dark and light"
    );
    ipc.registerHandler(
        "theme-mode-get",
        [this](const std::string&) -> std::string {
          std::string out(resolvedMode());
          out.push_back('\n');
          return out;
        },
        "theme-mode-get", "Print the current resolved theme mode"
    );
    ipc.registerHandler(
        "theme-mode-set",
        [this](const std::string& args) -> std::string {
          const std::string token = StringUtils::trim(args);
          const auto mode = enumFromKey(kThemeModes, token);
          if (!mode.has_value()) {
            return "error: expected dark, light, or auto\n";
          }
          m_config.setThemeMode(*mode);
          return "ok\n";
        },
        "theme-mode-set <dark|light|auto>", "Set theme mode and persist to settings.toml"
    );
    ipc.registerHandler(
        "theme-wallpaper-scheme-set",
        [this](const std::string& args) -> std::string {
          const std::string scheme = StringUtils::trim(args);
          if (scheme.empty()) {
            return "error: scheme name required\n";
          }
          if (!m_config.setThemeWallpaperScheme(scheme)) {
            return "error: unknown scheme or settings not writable (see docs for valid names)\n";
          }
          return "ok\n";
        },
        "theme-wallpaper-scheme-set <scheme>",
        "Set wallpaper palette generation scheme ([theme].wallpaper_scheme), e.g. m3-content or vibrant"
    );
  }

} // namespace noctalia::theme
