#pragma once

#include "compositors/display_backend.h"
#include "core/timer_manager.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class WaylandConnection;

namespace settings::display {

  enum class EdidStatus : std::uint8_t { Idle, Decoded, DecodedEmpty, ReadError, DecodeError };

  struct EdidSummary {
    std::string output;
    EdidStatus status = EdidStatus::Idle;
    std::string monitorName;
    std::string manufacturerId;
    std::string productCode;
    std::string serialText;
    std::string serialNumber;
    std::string version;
    int week = -1;
    int year = -1;
    std::string inputType;
    int sizeWidthCm = -1;
    int sizeHeightCm = -1;
    std::string preferredMode;
  };

  class DisplayService {
  public:
    DisplayService(std::unique_ptr<compositors::display::DisplayBackend> backend, WaylandConnection* wayland);

    [[nodiscard]] const std::vector<compositors::display::OutputState>& outputs() const { return m_outputs; }
    [[nodiscard]] const std::map<std::string, compositors::display::OutputState>& target() const { return m_target; }
    [[nodiscard]] bool writable() const;
    [[nodiscard]] bool supportsHdr() const;
    [[nodiscard]] std::string backendKind() const;
    [[nodiscard]] bool loading() const { return m_loading; }
    [[nodiscard]] const std::string& lastError() const { return m_lastError; }
    [[nodiscard]] bool awaitingConfirmation() const { return m_awaitingConfirmation; }
    [[nodiscard]] int revertCountdown() const { return m_revertCountdown; }
    [[nodiscard]] const EdidSummary& edidSummary() const { return m_edidSummary; }
    [[nodiscard]] const std::string& edidHex() const { return m_edidHex; }

    void setOnStateChanged(std::function<void()> callback) { m_onStateChanged = std::move(callback); }

    void fetchAsync();
    void setMode(const std::string& outputName, const std::string& modeStr);
    void setScale(const std::string& outputName, double scale);
    void setTransform(const std::string& outputName, const std::string& transform);
    void setVrr(const std::string& outputName, bool enabled);
    void setHdr(const std::string& outputName, bool enabled);
    void toggleOutput(const std::string& outputName, bool enabled);
    void setPosition(const std::string& outputName, int x, int y);
    void keepChanges();
    void revertChanges();
    void onSecondTick();
    void readEdid(const std::string& outputName);

  private:
    using TargetMap = std::map<std::string, compositors::display::OutputState>;

    [[nodiscard]] TargetMap currentSnapshot() const;
    [[nodiscard]] bool hasChangesComparedToCurrent() const;
    [[nodiscard]] int enabledOutputCount(const TargetMap& config) const;

    void startConfirmation(const TargetMap& snapshot);
    void enqueue(compositors::display::DisplayCommand command, const std::optional<TargetMap>& snapshot);
    void enqueueAll(
        const std::vector<compositors::display::DisplayCommand>& commands, const std::optional<TargetMap>& snapshot
    );
    void drainNext();
    static void stampHdrSupport(std::vector<compositors::display::OutputState>& outputs);
    void stampOutputMetadata(std::vector<compositors::display::OutputState>& outputs);
    void applyTopologyChange(
        const std::optional<TargetMap>& snapshot, std::vector<compositors::display::DisplayCommand> commands
    );
    void notifyStateChanged();

    std::shared_ptr<compositors::display::DisplayBackend> m_backend;
    WaylandConnection* m_wayland = nullptr;
    std::vector<compositors::display::OutputState> m_outputs;
    TargetMap m_target;
    std::optional<TargetMap> m_pendingSnapshot;
    std::deque<compositors::display::DisplayCommand> m_queue;
    bool m_busy = false;
    bool m_loading = false;
    bool m_awaitingConfirmation = false;
    int m_revertCountdown = 0;
    std::string m_lastError;
    std::function<void()> m_onStateChanged;

    Timer m_sleepTimer;
    Timer m_finishTimer;
    Timer m_refreshTimer;

    EdidSummary m_edidSummary;
    std::string m_edidHex;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };

} // namespace settings::display
