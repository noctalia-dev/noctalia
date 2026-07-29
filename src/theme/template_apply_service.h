#pragma once

#include "config/config_types.h"
#include "theme/palette.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

class ConfigService;
class IpcService;

namespace noctalia::theme {

  class TemplateApplyService {
  public:
    explicit TemplateApplyService(const ConfigService& config);
    ~TemplateApplyService();

    TemplateApplyService(const TemplateApplyService&) = delete;
    TemplateApplyService& operator=(const TemplateApplyService&) = delete;

    void apply(const GeneratedPalette& palette, std::string_view defaultMode, bool force = false) const;
    void setAfterApplyCallback(std::function<void()> callback) const;
    void registerIpc(IpcService& ipc);

  private:
    struct ApplyRequest {
      GeneratedPalette palette;
      ThemeConfig::TemplatesConfig templates;
      std::string defaultMode;
      std::string imagePath;
      std::string schemeType;
      std::uint64_t generation = 0;
    };

    [[nodiscard]] bool reapplyLast() const;
    [[nodiscard]] ApplyRequest makeRequest(const GeneratedPalette& palette, std::string_view defaultMode) const;
    [[nodiscard]] static bool sameInputs(const ApplyRequest& a, const ApplyRequest& b);
    void applyRequest(const ApplyRequest& request) const;
    void workerLoop();
    [[nodiscard]] bool requestSuperseded(std::uint64_t generation) const;

    const ConfigService& m_config;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    mutable std::optional<ApplyRequest> m_pendingRequest;
    mutable std::optional<ApplyRequest> m_lastAppliedRequest;
    mutable std::thread m_worker;
    mutable std::uint64_t m_nextGeneration = 0;
    mutable bool m_shutdown = false;
    mutable bool m_inFlight = false;
    mutable std::function<void()> m_afterApplyCallback;
  };

} // namespace noctalia::theme
