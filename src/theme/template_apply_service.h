#pragma once

#include "config/config_types.h"
#include "core/toml.h" // IWYU pragma: keep
#include "theme/palette.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class ConfigService;
class IpcService;

namespace noctalia::theme {

  class HookRunner;

  class TemplateApplyService {
  public:
    // How long teardown lets running template hooks finish before terminating them.
    static constexpr std::chrono::milliseconds kDefaultHookShutdownGrace{5000};

    explicit TemplateApplyService(
        ConfigService& config, std::chrono::milliseconds hookShutdownGrace = kDefaultHookShutdownGrace
    );
    ~TemplateApplyService();

    TemplateApplyService(const TemplateApplyService&) = delete;
    TemplateApplyService& operator=(const TemplateApplyService&) = delete;

    // Every completed application notifies, carrying the mode it applied. `paletteChanged`
    // marks this apply as one whose palette differs from the last; the flag is owed until a
    // non-superseded application reports it, so a later apply() that coalesces with,
    // supersedes, or deduplicates against this one still passes it on.
    void apply(
        const GeneratedPalette& palette, std::string_view defaultMode, bool force = false, bool paletteChanged = false
    ) const;
    // The notification handler. Sticky: set once.
    void setAfterApplyCallback(std::function<void(std::string_view appliedMode, bool paletteChanged)> callback) const;
    // Hook warnings are delivered on the main thread after an application finishes.
    void setWarningCallback(std::function<void(std::string_view)> callback) const;
    void registerIpc(IpcService& ipc);

  private:
    // Template ids of one source (built-in or community) whose output Noctalia has put on
    // disk: the enabled ones plus the ones still owing an undo. Mirrors one key of
    // [theme_templates] in state.toml, so a disable survives a restart between queueing the
    // request and running its undo hook. Main thread only.
    struct AppliedTemplateIds {
      std::string_view stateKey;
      std::set<std::string> applied;
      std::set<std::string> owed;
      bool loaded = false;
    };

    struct ApplyRequest {
      GeneratedPalette palette;
      ThemeConfig::TemplatesConfig templates;
      std::shared_ptr<const toml::table> configTable;
      std::string defaultMode;
      std::string imagePath;
      std::string schemeType;
      // Templates Noctalia has applied and that are no longer enabled: their undo hooks
      // run before this request's templates.
      std::vector<std::string> undoBuiltinIds;
      std::vector<std::string> undoCommunityIds;
      std::uint64_t generation = 0;
    };

    [[nodiscard]] bool reapplyLast() const;
    [[nodiscard]] ApplyRequest makeRequest(const GeneratedPalette& palette, std::string_view defaultMode) const;
    [[nodiscard]] static bool sameInputs(const ApplyRequest& a, const ApplyRequest& b);
    // Diffs `enabled` against the ids Noctalia has applied and returns the ids that still
    // owe an undo.
    [[nodiscard]] std::vector<std::string>
    syncAppliedIds(AppliedTemplateIds& ids, const std::set<std::string>& enabled) const;
    // Clears ids whose undo hook has run from the applied/owed sets.
    void forgetAppliedIds(AppliedTemplateIds& ids, const std::vector<std::string>& resolved) const;
    void persistAppliedIds(const AppliedTemplateIds& ids) const;
    // Run the undo hooks of request.undoBuiltinIds / request.undoCommunityIds and return
    // the ids they resolved.
    [[nodiscard]] std::vector<std::string> undoBuiltinTemplates(const ApplyRequest& request) const;
    [[nodiscard]] std::vector<std::string> undoCommunityTemplates(const ApplyRequest& request) const;
    // Renders and runs one undo hook synchronously. False when it must be retried later.
    [[nodiscard]] bool runUndoHook(
        const ApplyRequest& request, const std::filesystem::path& configPath, std::string_view hook,
        std::string_view label
    ) const;
    void applyRequest(const ApplyRequest& request) const;
    void workerLoop();
    [[nodiscard]] bool requestSuperseded(std::uint64_t generation) const;
    void recordHookWarning(std::uint64_t generation, std::string warning) const;

    ConfigService& m_config;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    mutable std::optional<ApplyRequest> m_pendingRequest;
    mutable std::optional<ApplyRequest> m_lastAppliedRequest;
    mutable AppliedTemplateIds m_builtinIds{.stateKey = "applied_builtin_ids"};
    mutable AppliedTemplateIds m_communityIds{.stateKey = "applied_community_ids"};
    mutable std::thread m_worker;
    mutable std::uint64_t m_nextGeneration = 0;
    mutable bool m_shutdown = false;
    mutable bool m_inFlight = false;
    mutable std::function<void(std::string_view appliedMode, bool paletteChanged)> m_afterApplyCallback;
    mutable std::function<void(std::string_view)> m_warningCallback;
    mutable std::set<std::string> m_pendingWarnings;
    std::shared_ptr<std::atomic<std::uint64_t>> m_warningGeneration = std::make_shared<std::atomic<std::uint64_t>>(0);
    // A palette change has been reported to apply() and not yet passed on to the handler.
    mutable bool m_paletteChangedOwed = false;
    // Raised once the shutdown grace elapses. Synchronous hooks and m_hookRunner share it, so
    // one deadline terminates every hook still running and teardown cannot hang on any of them.
    // Declared before m_hookRunner, which is constructed with it.
    std::shared_ptr<std::atomic<bool>> m_hookCancel = std::make_shared<std::atomic<bool>>(false);
    std::chrono::milliseconds m_hookShutdownGrace;
    mutable std::unique_ptr<HookRunner> m_hookRunner;
  };

} // namespace noctalia::theme
