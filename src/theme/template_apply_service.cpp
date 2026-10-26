#include "theme/template_apply_service.h"

#include "config/config_export.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/process/process.h"
#include "ipc/ipc_service.h"
#include "theme/community_templates.h"
#include "theme/gtk_extensions.h"
#include "theme/hook_runner.h"
#include "theme/template_engine.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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

    // state.toml owner holding the template ids Noctalia has applied, one key per source.
    constexpr std::string_view kStateOwner = "theme_templates";

    std::set<std::string> enabledBuiltinIds(const ThemeConfig::TemplatesConfig& templates) {
      if (!templates.enableBuiltinTemplates) {
        return {};
      }
      return {templates.builtinIds.begin(), templates.builtinIds.end()};
    }

    std::set<std::string> enabledCommunityIds(const ThemeConfig::TemplatesConfig& templates) {
      if (!templates.enableCommunityTemplates) {
        return {};
      }
      return {templates.communityIds.begin(), templates.communityIds.end()};
    }

  } // namespace

  TemplateApplyService::TemplateApplyService(ConfigService& config, std::chrono::milliseconds hookShutdownGrace)
      : m_config(config), m_hookShutdownGrace(hookShutdownGrace),
        m_hookRunner(std::make_unique<HookRunner>(HookRunner::kDefaultMaxConcurrent, m_hookCancel)) {
    m_worker = std::thread([this]() { workerLoop(); });
  }

  TemplateApplyService::~TemplateApplyService() {
    {
      std::scoped_lock lock(m_mutex);
      m_shutdown = true;
      m_warningGeneration->store(0);
      m_pendingRequest.reset();
    }
    m_cv.notify_one();
    // The worker may be draining hooks; drop the backlog so shutdown waits only for
    // the hooks already running.
    m_hookRunner->requestShutdown();
    // The worker can be blocked inside a synchronous hook, and the runner waits for its
    // asynchronous ones. Neither may hold teardown forever: both get one grace period,
    // then m_hookCancel terminates the process group of every hook still running.
    std::mutex graceMutex;
    std::condition_variable graceCv;
    bool hooksDone = false;
    std::thread graceThread([this, &graceMutex, &graceCv, &hooksDone]() {
      std::unique_lock lock(graceMutex);
      if (graceCv.wait_for(lock, m_hookShutdownGrace, [&hooksDone]() { return hooksDone; })) {
        return;
      }
      m_hookCancel->store(true);
      kLog.warn(
          "a template hook is still running after {}s; terminating it",
          std::chrono::duration_cast<std::chrono::duration<double>>(m_hookShutdownGrace).count()
      );
    });
    if (m_worker.joinable()) {
      m_worker.join();
    }
    // Destroyed inside the grace window: its destructor waits for the running hooks, and
    // the same deadline terminates them.
    m_hookRunner.reset();
    {
      std::scoped_lock lock(graceMutex);
      hooksDone = true;
    }
    graceCv.notify_one();
    graceThread.join();
  }

  void TemplateApplyService::setAfterApplyCallback(
      std::function<void(std::string_view appliedMode, bool paletteChanged)> callback
  ) const {
    std::scoped_lock lock(m_mutex);
    m_afterApplyCallback = std::move(callback);
  }

  void TemplateApplyService::setWarningCallback(std::function<void(std::string_view)> callback) const {
    std::scoped_lock lock(m_mutex);
    m_warningCallback = std::move(callback);
  }

  void TemplateApplyService::apply(
      const GeneratedPalette& palette, std::string_view defaultMode, bool force, bool paletteChanged
  ) const {
    ApplyRequest request = makeRequest(palette, defaultMode);
    bool queued = false;
    std::function<void()> undeliverable;
    {
      std::scoped_lock lock(m_mutex);
      m_paletteChangedOwed = m_paletteChangedOwed || paletteChanged;

      // Skip rendering and hooks when palette and template inputs are unchanged. Config values
      // are captured only when an application is queued; forced IPC re-application bypasses
      // this deduplication.
      if (!force && m_lastAppliedRequest.has_value() && sameInputs(request, *m_lastAppliedRequest)) {
        // The applied mode already matches, so only an owed palette change is still worth
        // reporting. It rides on the application queued or in flight; with neither, no worker
        // pass is left to carry it.
        if (m_paletteChangedOwed && !m_inFlight && !m_pendingRequest.has_value()) {
          m_paletteChangedOwed = false;
          if (m_afterApplyCallback) {
            undeliverable = [callback = m_afterApplyCallback, mode = request.defaultMode]() {
              callback(mode, /*paletteChanged=*/true);
            };
          }
        }
      } else {
        request.undoBuiltinIds = syncAppliedIds(m_builtinIds, enabledBuiltinIds(request.templates));
        request.undoCommunityIds = syncAppliedIds(m_communityIds, enabledCommunityIds(request.templates));
        request.generation = ++m_nextGeneration;
        m_warningGeneration->store(request.generation);
        m_lastAppliedRequest = request;
        m_pendingRequest = std::move(request);
        queued = true;
      }
    }

    if (undeliverable) {
      DeferredCall::callLater(std::move(undeliverable));
    }
    if (queued) {
      m_cv.notify_one();
    }
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

    // A forced re-render rewrites every template, so consumers hear about it as a colour change.
    apply(palette, defaultMode, /*force=*/true, /*paletteChanged=*/true);
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
    options.hookCancel = m_hookCancel;
    options.hookWarningCallback = [this, generation = request.generation](std::string warning) {
      recordHookWarning(generation, std::move(warning));
    };

    TemplateEngine engine(TemplateEngine::makeThemeData(request.palette), options);

    // The undo hooks ran synchronously; drop their ids from state.toml so the next
    // application does not repeat them. State belongs to the main thread.
    std::vector<std::string> undoneBuiltin = undoBuiltinTemplates(request);
    std::vector<std::string> undoneCommunity = undoCommunityTemplates(request);
    if (!undoneBuiltin.empty() || !undoneCommunity.empty()) {
      DeferredCall::callLater([this, undoneBuiltin = std::move(undoneBuiltin),
                               undoneCommunity = std::move(undoneCommunity)]() {
        forgetAppliedIds(m_builtinIds, undoneBuiltin);
        forgetAppliedIds(m_communityIds, undoneCommunity);
      });
    }

    // GTK extensions of community templates load through the built-in GTK templates, so they
    // follow those toggles.
    GtkToolkits gtkToolkits;
    if (request.templates.enableBuiltinTemplates) {
      gtkToolkits.gtk3 = std::ranges::contains(request.templates.builtinIds, "gtk3");
      gtkToolkits.gtk4 = std::ranges::contains(request.templates.builtinIds, "gtk4");
    }
    const std::filesystem::path gtk3Dir = gtkConfigDir(GtkToolkit::Gtk3);
    const std::filesystem::path gtk4Dir = gtkConfigDir(GtkToolkit::Gtk4);
    // noctalia.css imports the extension index; it has to exist before that import is written.
    if (gtkToolkits.gtk3)
      (void)ensureGtkExtensionIndex(gtk3Dir);
    if (gtkToolkits.gtk4)
      (void)ensureGtkExtensionIndex(gtk4Dir);

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

    std::vector<std::string> gtk3Extensions;
    std::vector<std::string> gtk4Extensions;
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
        toml::table communityRoot;
        try {
          communityRoot = toml::parse_file(communityConfig.string());
        } catch (const toml::parse_error&) {
          kLog.warn("failed to parse community template '{}' from {}", id, communityConfig.string());
          continue;
        }
        for (GtkExtensionFile& file : prepareGtkExtensions(communityRoot, id, gtkToolkits, gtk3Dir, gtk4Dir)) {
          (file.toolkit == GtkToolkit::Gtk3 ? gtk3Extensions : gtk4Extensions).push_back(std::move(file.fileName));
        }
        TemplateEngine communityEngine(TemplateEngine::makeThemeData(request.palette), options);
        if (!communityEngine.processConfigTable(communityRoot, communityConfig)) {
          kLog.warn("failed to apply community template '{}' from {}", id, communityConfig.string());
        }
      }
    }

    // Rewrite the index from what is enabled now, then drop extension files nothing imports.
    if (!requestSuperseded(request.generation)) {
      if (gtkToolkits.gtk3)
        (void)syncGtkExtensions(gtk3Dir, std::move(gtk3Extensions));
      if (gtkToolkits.gtk4)
        (void)syncGtkExtensions(gtk4Dir, std::move(gtk4Extensions));
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

  std::vector<std::string>
  TemplateApplyService::syncAppliedIds(AppliedTemplateIds& ids, const std::set<std::string>& enabled) const {
    if (!ids.loaded) {
      if (const auto encoded = m_config.stateString(kStateOwner, ids.stateKey)) {
        for (std::string& id : StringUtils::splitWhitespace(*encoded)) {
          ids.applied.emplace(std::move(id));
        }
      }
      ids.loaded = true;
    }

    // Everything applied or still owing an undo, minus what is enabled again now. Owed ids
    // survive a request that never ran (coalesced away, superseded) and a restart.
    std::set<std::string> owed;
    ids.owed.insert(ids.applied.begin(), ids.applied.end());
    std::ranges::set_difference(ids.owed, enabled, std::inserter(owed, owed.end()));
    ids.owed = std::move(owed);

    ids.applied = enabled;
    ids.applied.insert(ids.owed.begin(), ids.owed.end());
    persistAppliedIds(ids);

    return {ids.owed.begin(), ids.owed.end()};
  }

  void TemplateApplyService::forgetAppliedIds(AppliedTemplateIds& ids, const std::vector<std::string>& resolved) const {
    if (resolved.empty()) {
      return;
    }
    for (const auto& id : resolved) {
      ids.owed.erase(id);
      ids.applied.erase(id);
    }
    persistAppliedIds(ids);
  }

  void TemplateApplyService::persistAppliedIds(const AppliedTemplateIds& ids) const {
    (void)m_config.setStateString(
        kStateOwner, ids.stateKey, StringUtils::join({ids.applied.begin(), ids.applied.end()}, " ")
    );
  }

  bool TemplateApplyService::runUndoHook(
      const ApplyRequest& request, const std::filesystem::path& configPath, std::string_view hook,
      std::string_view label
  ) const {
    TemplateEngine::Options hookOptions;
    hookOptions.defaultMode = request.defaultMode;
    hookOptions.imagePath = request.imagePath;
    hookOptions.schemeType = request.schemeType;
    hookOptions.verbose = true;
    hookOptions.configTable = request.configTable;
    hookOptions.configDir = configPath.parent_path().string();
    hookOptions.configFile = configPath.string();
    TemplateEngine hookEngine(TemplateEngine::makeThemeData(request.palette), std::move(hookOptions));

    const RenderResult rendered = hookEngine.render(std::string(hook));
    if (rendered.errorCount != 0 || rendered.text.empty()) {
      // Retrying cannot fix a hook that does not render.
      kLog.warn("failed to render undo hook for {}", label);
      return true;
    }
    // A failed hook stays owed: the next application retries it. That includes one
    // terminated by the shutdown grace, so cancelling it here is safe.
    process::RunOptions runOptions;
    runOptions.cancel = m_hookCancel;
    const process::RunResult result = process::runSync(rendered.text, runOptions);
    if (!result) {
      kLog.warn("undo hook for {} failed with exit code {}: {}", label, result.exitCode, result.err);
      return false;
    }
    return true;
  }

  std::vector<std::string> TemplateApplyService::undoBuiltinTemplates(const ApplyRequest& request) const {
    if (request.undoBuiltinIds.empty() || requestSuperseded(request.generation)) {
      return {};
    }

    const std::filesystem::path configPath = builtinTemplateConfigPath();
    toml::table root;
    try {
      root = toml::parse_file(configPath.string());
    } catch (const toml::parse_error&) {
      kLog.warn("failed to parse built-in templates for undo hooks from {}", configPath.string());
      return {};
    }

    const toml::table* templates = root["templates"].as_table();
    if (templates == nullptr) {
      return {};
    }

    std::vector<std::string> resolved;
    for (const auto& id : request.undoBuiltinIds) {
      if (requestSuperseded(request.generation)) {
        return resolved;
      }
      const toml::table* entry = (*templates)[id].as_table();
      if (entry == nullptr) {
        kLog.warn("cannot undo unknown built-in template '{}'", id);
        resolved.push_back(id);
        continue;
      }
      const auto hook = entry->get_as<std::string>("undo_hook");
      if (hook == nullptr || runUndoHook(request, configPath, hook->get(), std::format("built-in template '{}'", id))) {
        resolved.push_back(id);
      }
    }
    return resolved;
  }

  std::vector<std::string> TemplateApplyService::undoCommunityTemplates(const ApplyRequest& request) const {
    std::vector<std::string> resolved;
    for (const auto& id : request.undoCommunityIds) {
      if (requestSuperseded(request.generation)) {
        return resolved;
      }
      if (!isSafeCommunityTemplateId(id)) {
        resolved.push_back(id);
        continue;
      }

      // The cache keeps a disabled template's files, so its undo hook is still on disk.
      const std::filesystem::path configPath = communityTemplateConfigPath(id);
      if (!std::filesystem::exists(configPath)) {
        kLog.warn("cannot undo community template '{}': {} is not cached", id, configPath.string());
        resolved.push_back(id);
        continue;
      }
      toml::table root;
      try {
        root = toml::parse_file(configPath.string());
      } catch (const toml::parse_error&) {
        kLog.warn("failed to parse community template '{}' for undo hooks from {}", id, configPath.string());
        resolved.push_back(id);
        continue;
      }
      const toml::table* templates = root["templates"].as_table();
      if (templates == nullptr) {
        resolved.push_back(id);
        continue;
      }

      // One community template can define several entries; it stays owed until every undo
      // hook succeeds, so those hooks must be safe to run again.
      bool done = true;
      for (const auto& [entryId, node] : *templates) {
        const toml::table* entry = node.as_table();
        const auto* hook = entry != nullptr ? entry->get_as<std::string>("undo_hook") : nullptr;
        if (hook != nullptr
            && !runUndoHook(
                request, configPath, hook->get(), std::format("community template '{}.{}'", id, entryId.str())
            )) {
          done = false;
        }
      }
      if (done) {
        resolved.push_back(id);
      }
    }
    return resolved;
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
        m_pendingWarnings.clear();
      }

      applyRequest(request);

      // Hooks of the current generation must finish before the after-apply callback
      // reports the theme as applied. A superseded generation skips the drain; the next
      // applyRequest() waits its hooks out before touching anything.
      if (!requestSuperseded(request.generation)) {
        m_hookRunner->waitIdle();
      }

      std::function<void()> afterApplyCallback;
      std::function<void()> warningCallback;
      {
        std::scoped_lock lock(m_mutex);
        m_inFlight = false;
        // A superseded generation reports nothing and leaves an owed palette change to the
        // generation that replaced it.
        if (!m_shutdown && request.generation == m_nextGeneration) {
          if (m_afterApplyCallback) {
            const bool paletteChanged = std::exchange(m_paletteChangedOwed, false);
            afterApplyCallback = [callback = m_afterApplyCallback, mode = request.defaultMode, paletteChanged]() {
              callback(mode, paletteChanged);
            };
          }
          if (m_warningCallback && !m_pendingWarnings.empty()) {
            warningCallback = [callback = m_warningCallback, warnings = std::move(m_pendingWarnings),
                               warningGeneration = m_warningGeneration, generation = request.generation]() {
              if (warningGeneration->load() != generation) {
                return;
              }
              std::string message;
              for (const std::string& warning : warnings) {
                if (!message.empty()) {
                  message += "\n\n";
                }
                message += warning;
              }
              callback(message);
            };
          }
        }
      }
      if (warningCallback) {
        DeferredCall::callLater(std::move(warningCallback));
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

  void TemplateApplyService::recordHookWarning(std::uint64_t generation, std::string warning) const {
    std::scoped_lock lock(m_mutex);
    if (!m_shutdown && generation == m_nextGeneration) {
      m_pendingWarnings.insert(std::move(warning));
    }
  }

} // namespace noctalia::theme
