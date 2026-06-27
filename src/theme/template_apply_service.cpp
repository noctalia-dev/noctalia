#include "theme/template_apply_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/resource_paths.h"
#include "ipc/ipc_service.h"
#include "theme/community_templates.h"
#include "theme/template_engine.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("theme_templates");

    std::filesystem::path builtinTemplateConfigPath() { return paths::assetPath("templates/builtin.toml"); }

    std::string schemeTypeFromConfig(const ThemeConfig& theme) {
      if (theme.wallpaperScheme.starts_with("m3-"))
        return theme.wallpaperScheme.substr(3);
      return theme.wallpaperScheme;
    }

    std::filesystem::path userTemplateConfigPath() {
      const std::string configDir = FileUtils::configDir();
      if (configDir.empty()) {
        return "noctalia.toml";
      }
      return std::filesystem::path(configDir) / "config.toml";
    }

    toml::array stringArray(const std::vector<std::string>& values) {
      toml::array out;
      for (const auto& value : values) {
        out.push_back(value);
      }
      return out;
    }

    void insertStringOrArray(toml::table& table, std::string_view key, const std::vector<std::string>& values) {
      if (values.empty()) {
        return;
      }
      if (values.size() == 1) {
        table.insert_or_assign(std::string(key), values.front());
        return;
      }
      table.insert_or_assign(std::string(key), stringArray(values));
    }

    toml::table buildUserTemplateRoot(const ThemeConfig::TemplatesConfig& templatesConfig) {
      toml::table root;

      if (!templatesConfig.customColors.empty()) {
        toml::table customColors;
        for (const auto& color : templatesConfig.customColors) {
          toml::table colorTable;
          colorTable.insert_or_assign("color", color.color);
          colorTable.insert_or_assign("blend", color.blend);
          customColors.insert_or_assign(color.name, std::move(colorTable));
        }

        toml::table config;
        config.insert_or_assign("custom_colors", std::move(customColors));
        root.insert_or_assign("config", std::move(config));
      }

      toml::table userTemplates;
      for (const auto& userTemplate : templatesConfig.userTemplates) {
        if (!userTemplate.enabled) {
          continue;
        }

        toml::table templateTable;
        if (userTemplate.inputPathModes.has_value()) {
          toml::table inputPathModes;
          inputPathModes.insert_or_assign("dark", userTemplate.inputPathModes->dark);
          inputPathModes.insert_or_assign("light", userTemplate.inputPathModes->light);
          templateTable.insert_or_assign("input_path_modes", std::move(inputPathModes));
        } else if (!userTemplate.inputPath.empty()) {
          templateTable.insert_or_assign("input_path", userTemplate.inputPath);
        }
        insertStringOrArray(templateTable, "output_path", userTemplate.outputPaths);
        if (!userTemplate.outputPathDynamic.empty()) {
          templateTable.insert_or_assign("output_path_dynamic", userTemplate.outputPathDynamic);
        }
        if (!userTemplate.compareTo.empty()) {
          templateTable.insert_or_assign("compare_to", userTemplate.compareTo);
        }
        if (!userTemplate.colorsToCompare.empty()) {
          toml::array colors;
          for (const auto& color : userTemplate.colorsToCompare) {
            toml::table colorTable;
            colorTable.insert_or_assign("name", color.name);
            colorTable.insert_or_assign("color", color.color);
            colors.push_back(std::move(colorTable));
          }
          templateTable.insert_or_assign("colors_to_compare", std::move(colors));
        }
        if (!userTemplate.preHook.empty()) {
          templateTable.insert_or_assign("pre_hook", userTemplate.preHook);
        }
        if (!userTemplate.postHook.empty()) {
          templateTable.insert_or_assign("post_hook", userTemplate.postHook);
        }
        if (userTemplate.index != 0) {
          templateTable.insert_or_assign("index", static_cast<std::int64_t>(userTemplate.index));
        }
        userTemplates.insert_or_assign(userTemplate.id, std::move(templateTable));
      }

      if (!userTemplates.empty()) {
        root.insert_or_assign("templates", std::move(userTemplates));
      }
      return root;
    }

  } // namespace

  TemplateApplyService::TemplateApplyService(const ConfigService& config) : m_config(config) {
    m_worker = std::thread([this]() { workerLoop(); });
  }

  TemplateApplyService::~TemplateApplyService() {
    {
      std::scoped_lock lock(m_mutex);
      m_shutdown = true;
      m_pendingRequest.reset();
    }
    m_cv.notify_one();
    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

  void TemplateApplyService::apply(const GeneratedPalette& palette, std::string_view defaultMode, bool force) const {
    ApplyRequest request = makeRequest(palette, defaultMode);
    {
      std::scoped_lock lock(m_mutex);
      // Config reloads fire on every settings change, not just theme changes.
      // Skip redundant reprocessing (and its synchronous template hooks) when
      // nothing the templates depend on has changed. Explicit re-application
      // (startup, IPC, template activation) passes force or carries new inputs.
      if (!force && m_lastAppliedRequest.has_value() && sameInputs(request, *m_lastAppliedRequest)) {
        return;
      }
      request.generation = ++m_nextGeneration;
      m_lastAppliedRequest = request;
      m_pendingRequest = std::move(request);
    }
    m_cv.notify_one();
  }

  bool TemplateApplyService::reapplyLast() const {
    GeneratedPalette palette;
    std::string defaultMode;
    {
      std::scoped_lock lock(m_mutex);
      if (!m_lastAppliedRequest.has_value()) {
        return false;
      }
      palette = m_lastAppliedRequest->palette;
      defaultMode = m_lastAppliedRequest->defaultMode;
    }

    apply(palette, defaultMode, /*force=*/true);
    return true;
  }

  bool TemplateApplyService::sameInputs(const ApplyRequest& a, const ApplyRequest& b) {
    return a.palette == b.palette
        && a.templates == b.templates
        && a.defaultMode == b.defaultMode
        && a.imagePath == b.imagePath
        && a.schemeType == b.schemeType;
  }

  void TemplateApplyService::registerIpc(IpcService& ipc) {
    ipc.registerHandler(
        "templates-apply",
        [this](const std::string& args) -> std::string {
          if (!StringUtils::trim(args).empty()) {
            return "error: usage: templates-apply\n";
          }
          if (!reapplyLast()) {
            return "error: theme palette has not been resolved yet\n";
          }
          return "ok\n";
        },
        "templates-apply", "Apply configured theme templates for the current palette"
    );
  }

  TemplateApplyService::ApplyRequest
  TemplateApplyService::makeRequest(const GeneratedPalette& palette, std::string_view defaultMode) const {
    const ThemeConfig& theme = m_config.config().theme;
    return ApplyRequest{
        .palette = palette,
        .templates = theme.templates,
        .defaultMode = std::string(defaultMode),
        .imagePath = m_config.getPaletteWallpaperPath(),
        .schemeType = schemeTypeFromConfig(theme),
    };
  }

  void TemplateApplyService::applyRequest(const ApplyRequest& request) const {
    TemplateEngine::Options options;
    options.defaultMode = request.defaultMode;
    options.imagePath = request.imagePath;
    options.schemeType = request.schemeType;
    options.verbose = true;
    options.cancelRequested = [this, generation = request.generation]() { return requestSuperseded(generation); };

    TemplateEngine engine(TemplateEngine::makeThemeData(request.palette), options);

    if (request.templates.enableBuiltinTemplates
        && !request.templates.builtinIds.empty()
        && !requestSuperseded(request.generation)) {
      TemplateEngine::Options builtinOptions = options;
      builtinOptions.enabledTemplates.insert(request.templates.builtinIds.begin(), request.templates.builtinIds.end());
      TemplateEngine builtinEngine(TemplateEngine::makeThemeData(request.palette), std::move(builtinOptions));
      const std::filesystem::path builtinConfig = builtinTemplateConfigPath();
      if (!builtinEngine.processConfigFile(builtinConfig)) {
        kLog.warn("failed to apply built-in templates from {}", builtinConfig.string());
      }
    }

    if (request.templates.enableCommunityTemplates
        && !request.templates.communityIds.empty()
        && !requestSuperseded(request.generation)) {
      for (const auto& id : request.templates.communityIds) {
        if (requestSuperseded(request.generation))
          return;
        if (!isSafeCommunityTemplateId(id)) {
          kLog.warn("skipping unsafe community template id '{}'", id);
          continue;
        }

        const std::filesystem::path communityConfig = communityTemplateConfigPath(id);
        if (!std::filesystem::exists(communityConfig)) {
          kLog.warn("community template '{}' is not cached yet", id);
          continue;
        }
        TemplateEngine communityEngine(TemplateEngine::makeThemeData(request.palette), options);
        if (!communityEngine.processConfigFile(communityConfig)) {
          kLog.warn("failed to apply community template '{}' from {}", id, communityConfig.string());
        }
      }
    }

    if (request.templates.userTemplates.empty() || requestSuperseded(request.generation))
      return;

    const toml::table userTemplateRoot = buildUserTemplateRoot(request.templates);
    const std::filesystem::path configPath = userTemplateConfigPath();
    if (!engine.processConfigTable(userTemplateRoot, configPath)) {
      kLog.warn("failed to apply user templates from main config");
    }
  }

  void TemplateApplyService::workerLoop() {
    while (true) {
      ApplyRequest request;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_shutdown || m_pendingRequest.has_value(); });
        if (m_shutdown) {
          return;
        }
        request = std::move(*m_pendingRequest);
        m_pendingRequest.reset();
      }

      applyRequest(request);
    }
  }

  bool TemplateApplyService::requestSuperseded(std::uint64_t generation) const {
    std::scoped_lock lock(m_mutex);
    return m_shutdown || generation != m_nextGeneration;
  }

} // namespace noctalia::theme
