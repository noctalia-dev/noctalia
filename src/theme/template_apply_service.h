#pragma once

#include "config/config_types.h"
#include "core/toml.h" // IWYU pragma: keep
#include "theme/palette.h"

#include <condition_variable>
#include <cstdint>
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
    explicit TemplateApplyService(ConfigService& config);
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
    void registerIpc(IpcService& ipc);

  private:
    struct ApplyRequest {
      GeneratedPalette palette;
      ThemeConfig::TemplatesConfig templates;
      std::shared_ptr<const toml::table> configTable;
      std::string defaultMode;
      std::string imagePath;
      std::string schemeType;
      // Built-in templates Noctalia has applied and that are no longer enabled: their
      // undo hooks run before this request's templates.
      std::vector<std::string> undoBuiltinIds;
      std::uint64_t generation = 0;
    };

    [[nodiscard]] bool reapplyLast() const;
    [[nodiscard]] ApplyRequest makeRequest(const GeneratedPalette& palette, std::string_view defaultMode) const;
    [[nodiscard]] static bool sameInputs(const ApplyRequest& a, const ApplyRequest& b);
    // Diffs the enabled built-in template ids against the ids Noctalia has applied (persisted
    // in state.toml) and returns the ids that still owe an undo. Main thread only.
    [[nodiscard]] std::vector<std::string> syncAppliedBuiltinIds(const ThemeConfig::TemplatesConfig& templates) const;
    // Clears ids whose undo hook has run from the applied/owed sets. Main thread only.
    void forgetAppliedBuiltinIds(const std::vector<std::string>& ids) const;
    void persistAppliedBuiltinIds() const;
    // Runs the undo hooks of request.undoBuiltinIds and returns the ids it resolved.
    [[nodiscard]] std::vector<std::string> undoBuiltinTemplates(const ApplyRequest& request) const;
    void applyRequest(const ApplyRequest& request) const;
    void workerLoop();
    [[nodiscard]] bool requestSuperseded(std::uint64_t generation) const;

    ConfigService& m_config;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    mutable std::optional<ApplyRequest> m_pendingRequest;
    mutable std::optional<ApplyRequest> m_lastAppliedRequest;
    // Built-in template ids whose output is on disk: the enabled ones plus the ones still
    // owing an undo. Mirrors [theme_templates].applied_builtin_ids in state.toml, so a
    // disable survives a restart between queueing the request and running its undo hook.
    mutable std::set<std::string> m_appliedBuiltinIds;
    mutable std::set<std::string> m_owedUndoBuiltinIds;
    mutable bool m_appliedBuiltinIdsLoaded = false;
    mutable std::thread m_worker;
    mutable std::uint64_t m_nextGeneration = 0;
    mutable bool m_shutdown = false;
    mutable bool m_inFlight = false;
    mutable std::function<void(std::string_view appliedMode, bool paletteChanged)> m_afterApplyCallback;
    // A palette change has been reported to apply() and not yet passed on to the handler.
    mutable bool m_paletteChangedOwed = false;
    mutable std::unique_ptr<HookRunner> m_hookRunner;
  };

} // namespace noctalia::theme
