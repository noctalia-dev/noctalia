#include "theme/template_apply_service.h"

#include "config/config_export.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/process/process.h"
#include "ipc/ipc_service.h"
#include "theme/community_templates.h"
#include "theme/hook_runner.h"
#include "theme/template_engine.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("theme_templates");

    // Request coalescing window: a burst of requests applies once, after the stream has
    // been quiet for kRequestQuietWindow, and at the latest kMaxRequestDeferral after the
    // first request of the burst.
    constexpr auto kRequestQuietWindow = std::chrono::milliseconds(100);
    constexpr auto kMaxRequestDeferral = std::chrono::milliseconds(500);

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
          if (!color.color_dark.empty()) {
            colorTable.insert_or_assign("color_dark", color.color_dark);
          }
          if (!color.color_light.empty()) {
            colorTable.insert_or_assign("color_light", color.color_light);
          }
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
        if (!userTemplate.postAction.empty()) {
          templateTable.insert_or_assign("post_action", userTemplate.postAction);
        }
        if (userTemplate.index != 0) {
          templateTable.insert_or_assign("index", static_cast<std::int64_t>(userTemplate.index));
        }
        if (!userTemplate.hookAsync) {
          templateTable.insert_or_assign("hook_async", false);
        }
        userTemplates.insert_or_assign(userTemplate.id, std::move(templateTable));
      }

      if (!userTemplates.empty()) {
        root.insert_or_assign("templates", std::move(userTemplates));
      }
      return root;
    }

    std::vector<std::string>
    disabledBuiltinIds(const ThemeConfig::TemplatesConfig& previous, const ThemeConfig::TemplatesConfig& current) {
      if (!previous.enableBuiltinTemplates) {
        return {};
      }

      std::unordered_set<std::string> enabled;
      if (current.enableBuiltinTemplates) {
        enabled.insert(current.builtinIds.begin(), current.builtinIds.end());
      }

      std::vector<std::string> disabled;
      for (const auto& id : previous.builtinIds) {
        if (!enabled.contains(id)) {
          disabled.push_back(id);
        }
      }
      return disabled;
    }

  } // namespace

  TemplateApplyService::TemplateApplyService(const ConfigService& config)
      : m_config(config), m_hookRunner(std::make_unique<HookRunner>()) {
    m_worker = std::thread([this]() { workerLoop(); });
  }

  TemplateApplyService::~TemplateApplyService() {
    {
      std::scoped_lock lock(m_mutex);
      m_shutdown = true;
      m_pendingRequest.reset();
    }
    m_cv.notify_one();
    // The worker may be draining hooks; drop the backlog so shutdown waits only for
    // the hooks already running.
    m_hookRunner->requestShutdown();
    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

  void TemplateApplyService::setAfterApplyCallback(std::function<void()> callback) const {
    std::scoped_lock lock(m_mutex);
    m_afterApplyCallback = std::move(callback);
  }

  void TemplateApplyService::apply(const GeneratedPalette& palette, std::string_view defaultMode, bool force) const {
    ApplyRequest request = makeRequest(palette, defaultMode);
    std::function<void()> afterApplyCallback;
    {
      std::scoped_lock lock(m_mutex);
      // Skip rendering and hooks when palette and template inputs are unchanged. Config values
      // are captured only when an application is queued; forced IPC re-application bypasses
      // this deduplication.
      if (!force && m_lastAppliedRequest.has_value() && sameInputs(request, *m_lastAppliedRequest)) {
        // A queued request has not been applied yet; it fires the callback when it lands.
        if (m_afterApplyCallback && !m_inFlight && !m_pendingRequest.has_value()) {
          afterApplyCallback = m_afterApplyCallback;
        }
        if (!afterApplyCallback) {
          return;
        }
      } else {
        if (m_lastAppliedRequest.has_value()) {
          request.disabledBuiltinIds = disabledBuiltinIds(m_lastAppliedRequest->templates, request.templates);
        } else {
          request.reconcileDisabledBuiltinIds = true;
        }
        request.generation = ++m_nextGeneration;
        m_lastAppliedRequest = request;
        m_pendingRequest = std::move(request);
      }
    }

    if (afterApplyCallback) {
      DeferredCall::callLater(std::move(afterApplyCallback));
      return;
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
    ipc.bind(noctalia::cli::msg::templatesApply, [this](const std::string& args) -> std::string {
      if (!StringUtils::trim(args).empty()) {
        return "error: usage: templates-apply\n";
      }
      if (!reapplyLast()) {
        return "error: theme palette has not been resolved yet\n";
      }
      return "ok\n";
    });
  }

  TemplateApplyService::ApplyRequest
  TemplateApplyService::makeRequest(const GeneratedPalette& palette, std::string_view defaultMode) const {
    const Config& config = m_config.config();
    const ThemeConfig& theme = config.theme;
    return ApplyRequest{
        .palette = palette,
        .templates = theme.templates,
        .configTable = std::make_shared<const toml::table>(config_export::serialize(config)),
        .defaultMode = std::string(defaultMode),
        .imagePath = m_config.getPaletteWallpaperPath(),
        .schemeType = schemeTypeFromConfig(theme),
    };
  }

  void TemplateApplyService::applyRequest(const ApplyRequest& request) const {
    HookRunner& hookRunner = *m_hookRunner;

    // Hooks from superseded generations must not outlive them: drop the queued ones and
    // wait out the ones already running. A started hook cannot be cancelled safely, so
    // without this wait a slow hook from an older generation could finish last and win
    // the final state (and undo hooks below could be reverted by it).
    hookRunner.invalidateBefore(request.generation);
    hookRunner.waitIdle();

    TemplateEngine::Options options;
    options.defaultMode = request.defaultMode;
    options.imagePath = request.imagePath;
    options.schemeType = request.schemeType;
    options.verbose = true;
    options.cancelRequested = [this, generation = request.generation]() { return requestSuperseded(generation); };
    options.configTable = request.configTable;
    options.hookRunner = &hookRunner;
    options.generation = request.generation;

    TemplateEngine engine(TemplateEngine::makeThemeData(request.palette), options);

    undoDisabledBuiltinTemplates(request);

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

    if ((request.templates.userTemplates.empty() && request.templates.customColors.empty())
        || requestSuperseded(request.generation)) {
      return;
    }

    const toml::table userTemplateRoot = buildUserTemplateRoot(request.templates);
    const std::filesystem::path configPath = userTemplateConfigPath();
    if (!engine.processConfigTable(userTemplateRoot, configPath)) {
      kLog.warn("failed to apply user templates from main config");
    }
  }

  void TemplateApplyService::undoDisabledBuiltinTemplates(const ApplyRequest& request) const {
    if ((!request.reconcileDisabledBuiltinIds && request.disabledBuiltinIds.empty())
        || requestSuperseded(request.generation)) {
      return;
    }

    const std::filesystem::path configPath = builtinTemplateConfigPath();
    toml::table root;
    try {
      root = toml::parse_file(configPath.string());
    } catch (const toml::parse_error&) {
      kLog.warn("failed to parse built-in templates for undo hooks from {}", configPath.string());
      return;
    }

    const toml::table* templates = root["templates"].as_table();
    if (templates == nullptr) {
      return;
    }

    std::vector<std::string> ids = request.disabledBuiltinIds;
    if (request.reconcileDisabledBuiltinIds) {
      std::unordered_set<std::string> enabled;
      if (request.templates.enableBuiltinTemplates) {
        enabled.insert(request.templates.builtinIds.begin(), request.templates.builtinIds.end());
      }
      for (const auto& [id, entry] : *templates) {
        if (entry.is_table() && !enabled.contains(std::string(id.str()))) {
          ids.emplace_back(id.str());
        }
      }
    }

    TemplateEngine::Options hookOptions;
    hookOptions.defaultMode = request.defaultMode;
    hookOptions.imagePath = request.imagePath;
    hookOptions.schemeType = request.schemeType;
    hookOptions.verbose = true;
    hookOptions.configTable = request.configTable;
    hookOptions.configDir = configPath.parent_path().string();
    hookOptions.configFile = configPath.string();
    TemplateEngine hookEngine(TemplateEngine::makeThemeData(request.palette), std::move(hookOptions));

    for (const auto& id : ids) {
      if (requestSuperseded(request.generation)) {
        return;
      }
      const toml::table* entry = (*templates)[id].as_table();
      if (entry == nullptr) {
        kLog.warn("cannot undo unknown built-in template '{}'", id);
        continue;
      }
      const auto hook = entry->get_as<std::string>("undo_hook");
      if (hook == nullptr) {
        continue;
      }

      const RenderResult rendered = hookEngine.render(hook->get());
      if (rendered.errorCount != 0 || rendered.text.empty()) {
        kLog.warn("failed to render undo hook for built-in template '{}'", id);
        continue;
      }
      const process::RunResult result = process::runSync(rendered.text);
      if (!result) {
        kLog.warn("undo hook for built-in template '{}' failed with exit code {}: {}", id, result.exitCode, result.err);
      }
    }
  }

  void TemplateApplyService::workerLoop() {
    while (true) {
      ApplyRequest request;
      {
        std::unique_lock lock(m_mutex);

        // Wait for initial request
        m_cv.wait(lock, [this]() { return m_shutdown || m_pendingRequest.has_value(); });

        if (m_shutdown) {
          return;
        }

        // Coalesce bursts of requests (theme scrubbing, wallpaper cycling) into one
        // application: wait for a quiet window, but never defer past kMaxRequestDeferral
        // from the first request of the burst, so a steady request stream still applies.
        const auto burstStart = std::chrono::steady_clock::now();
        const auto deferralLimit = burstStart + kMaxRequestDeferral;
        auto quietUntil = burstStart + kRequestQuietWindow;
        auto lastGeneration = m_nextGeneration;

        while (!m_shutdown) {
          const auto wakeAt = std::min(quietUntil, deferralLimit);
          if (std::chrono::steady_clock::now() >= wakeAt) {
            break;
          }
          m_cv.wait_until(lock, wakeAt);
          if (m_nextGeneration != lastGeneration) {
            lastGeneration = m_nextGeneration;
            quietUntil = std::chrono::steady_clock::now() + kRequestQuietWindow;
          }
        }

        if (m_shutdown) {
          return;
        }

        request = std::move(*m_pendingRequest);
        m_pendingRequest.reset();
        m_inFlight = true;
      }

      applyRequest(request);

      // Hooks of the current generation must finish before the after-apply callback
      // reports the theme as applied. A superseded generation skips the drain; the next
      // applyRequest() waits its hooks out before touching anything.
      if (!requestSuperseded(request.generation)) {
        m_hookRunner->waitIdle();
      }

      std::function<void()> afterApplyCallback;
      {
        std::scoped_lock lock(m_mutex);
        m_inFlight = false;
        if (!m_shutdown && request.generation == m_nextGeneration) {
          afterApplyCallback = m_afterApplyCallback;
        }
      }
      if (afterApplyCallback) {
        DeferredCall::callLater(std::move(afterApplyCallback));
      }
    }
  }

  bool TemplateApplyService::requestSuperseded(std::uint64_t generation) const {
    std::scoped_lock lock(m_mutex);
    return m_shutdown || generation != m_nextGeneration;
  }

} // namespace noctalia::theme
