#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sdbus {
  class Error;
  class IObject;
  class IProxy;
  class Variant;
} // namespace sdbus

class SessionBus;
class IpcService;

struct MprisPlayerInfo {
  std::string busName;
  std::string identity;
  std::string desktopEntry;
  std::string playbackStatus;
  std::string trackId;
  std::string title;
  std::vector<std::string> artists;
  std::string album;
  std::string sourceUrl;
  std::string artUrl;
  std::string loopStatus{"None"};
  bool shuffle{false};
  double volume{1.0};
  int64_t positionUs{0};
  int64_t lengthUs{0};
  bool canPlay{false};
  bool canPause{false};
  bool canGoNext{false};
  bool canGoPrevious{false};
  bool canSeek{false};

  bool operator==(const MprisPlayerInfo&) const = default;
};

// Joins artist names with ", ".
[[nodiscard]] std::string joinedArtists(const std::vector<std::string>& artists);

class MprisService {
public:
  explicit MprisService(SessionBus& bus);

  [[nodiscard]] const std::unordered_map<std::string, MprisPlayerInfo>& players() const noexcept;
  [[nodiscard]] std::vector<MprisPlayerInfo> listPlayers() const;
  [[nodiscard]] std::optional<MprisPlayerInfo> activePlayer() const;
  void refreshPlayers();
  void registerIpc(IpcService& ipc);

  bool playPause(const std::string& busName);
  bool stop(const std::string& busName);
  bool next(const std::string& busName);
  bool previous(const std::string& busName);
  bool playPauseActive();
  bool stopActive();
  bool nextActive();
  bool previousActive();
  bool seek(const std::string& busName, int64_t offsetUs);
  bool seekActive(int64_t offsetUs);
  bool setPosition(const std::string& busName, int64_t positionUs);
  bool setPositionActive(int64_t positionUs);
  bool setVolume(const std::string& busName, double volume);
  bool setVolumeActive(double volume);
  bool setShuffle(const std::string& busName, bool shuffle);
  bool setShuffleActive(bool shuffle);
  bool setLoopStatus(const std::string& busName, std::string loopStatus);
  bool setLoopStatusActive(std::string loopStatus);
  [[nodiscard]] std::optional<int64_t> position(const std::string& busName) const;
  [[nodiscard]] std::optional<int64_t> positionActive() const;
  [[nodiscard]] std::optional<double> volume(const std::string& busName) const;
  [[nodiscard]] std::optional<double> volumeActive() const;
  [[nodiscard]] std::optional<bool> shuffle(const std::string& busName) const;
  [[nodiscard]] std::optional<bool> shuffleActive() const;
  [[nodiscard]] std::optional<std::string> loopStatus(const std::string& busName) const;
  [[nodiscard]] std::optional<std::string> loopStatusActive() const;

  bool setPinnedPlayerPreference(const std::string& busName);
  void clearPinnedPlayerPreference();
  void setPreferredPlayers(std::vector<std::string> preferredBusNames);
  void setBlacklist(std::vector<std::string> blacklist);
  void setChangeCallback(std::function<void()> callback);
  [[nodiscard]] std::optional<std::string> pinnedPlayerPreference() const;
  [[nodiscard]] const std::vector<std::string>& preferredPlayers() const noexcept;
  [[nodiscard]] const std::vector<std::string>& blacklist() const noexcept;

private:
  void registerControlApi();
  void emitPlayersChanged();
  void emitActivePlayerChanged();
  void emitTrackChanged(const MprisPlayerInfo& player);
  void syncSignals(const std::optional<MprisPlayerInfo>& previousActive);
  void registerBusSignals();
  void discoverPlayers();
  void scheduleDiscoveryDrain();
  void scheduleStartupRediscovery();
  void scheduleRecoveryDiscovery();
  void addOrRefreshPlayer(const std::string& busName);
  void applyPlayerSnapshot(
      const std::string& busName, const MprisPlayerInfo& info, bool hadPositionSignal, bool hadFullRefreshFailure
  );
  [[nodiscard]] bool shouldRetryPropertiesRefresh(const std::string& busName) const;
  [[nodiscard]] std::chrono::milliseconds propertiesRefreshRetryInterval(
      const std::string& busName, std::chrono::milliseconds fallback, bool usePropertiesBackoff
  ) const;
  void schedulePositionRefreshRetry(
      const std::string& busName, std::chrono::milliseconds fallback, bool usePropertiesBackoff
  );
  void refreshPlayerPosition(const std::string& busName, bool notifyChange);
  void applyPositionSample(const std::string& busName, int64_t rawPositionUs, bool notifyChange);
  void removePlayer(const std::string& busName);
  [[nodiscard]] MprisPlayerInfo readPlayerInfoFromProperties(
      const std::string& busName, const std::map<std::string, sdbus::Variant>& rootProps,
      const std::map<std::string, sdbus::Variant>& playerProps
  ) const;
  [[nodiscard]] MprisPlayerInfo projectedPlayerInfo(const MprisPlayerInfo& player) const;
  [[nodiscard]] std::int64_t projectedPositionUs(const MprisPlayerInfo& player) const;
  [[nodiscard]] std::optional<std::string> chooseActivePlayer() const;
  [[nodiscard]] bool isBlacklisted(const MprisPlayerInfo& player) const;
  std::function<void(std::optional<sdbus::Error>)> makeAsyncReplyHandler(std::string op, std::string busName);
  std::function<void(std::optional<sdbus::Error>)>
  makeAsyncReplyHandler(std::string op, std::string busName, std::string_view method);
  [[nodiscard]] bool callPlayerMethod(const std::string& busName, const char* methodName);
  [[nodiscard]] bool canInvoke(const MprisPlayerInfo& player, const char* methodName) const;
  void dismissPlayer(const std::string& busName);

  bool onPlayPausePlayer(const std::string& busName);
  bool onStopPlayer(const std::string& busName);
  bool onNextPlayer(const std::string& busName);
  bool onPreviousPlayer(const std::string& busName);
  bool onPlayPauseActive();
  bool onStopActive();
  bool onNextActive();
  bool onPreviousActive();
  bool onSeekPlayer(const std::string& busName, int64_t offsetUs);
  bool onSeekActive(int64_t offsetUs);
  bool onSetPositionPlayer(const std::string& busName, int64_t positionUs);
  bool onSetPositionActive(int64_t positionUs);
  bool onSetVolumePlayer(const std::string& busName, double volume);
  bool onSetVolumeActive(double volume);
  bool onSetShufflePlayer(const std::string& busName, bool shuffle);
  bool onSetShuffleActive(bool shuffle);
  bool onSetLoopStatusPlayer(const std::string& busName, const std::string& loopStatus);
  bool onSetLoopStatusActive(const std::string& loopStatus);
  int64_t onGetPositionPlayer(const std::string& busName) const;
  int64_t onGetPositionActive() const;
  double onGetVolumePlayer(const std::string& busName) const;
  double onGetVolumeActive() const;
  bool onGetShufflePlayer(const std::string& busName) const;
  bool onGetShuffleActive() const;
  std::string onGetLoopStatusPlayer(const std::string& busName) const;
  std::string onGetLoopStatusActive() const;
  bool onSetActivePlayerPreference(const std::string& busName);
  bool onClearActivePlayerPreference();
  bool onSetPreferredPlayers(const std::vector<std::string>& preferredBusNames);
  [[nodiscard]] std::tuple<bool, std::string, std::vector<std::string>> onGetPlayerPreferences() const;

  SessionBus& m_bus;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::unique_ptr<sdbus::IObject> m_controlObject;
  std::unique_ptr<sdbus::IProxy> m_dbusProxy;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_playerProxies;
  std::unordered_map<std::string, MprisPlayerInfo> m_players;
  std::unordered_map<std::string, std::string> m_logicalTrackSignatures;
  std::unordered_map<std::string, std::int64_t> m_positionOffsetsUs;
  std::unordered_map<std::string, std::uint64_t> m_positionResyncTimers;
  std::unordered_map<std::string, bool> m_pendingPositionSignalRefresh;
  std::unordered_map<std::string, bool> m_hasAuthoritativePositionSample;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastLogicalTrackChangeAt;
  std::unordered_map<std::string, std::int64_t> m_previousTrackRawPositionUs;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastSeekCommandAt;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_recentNoSignalPauseAt;
  std::unordered_map<std::string, std::int64_t> m_pendingPositionCandidateUs;
  std::unordered_map<std::string, int> m_pendingPositionCandidateMatches;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_pendingPositionCandidateAt;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastPositionSampleAt;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastPropertiesUpdate;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastPlayingUpdate;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastStrongMetadataUpdate;
  std::unordered_map<std::string, int> m_playerPropertiesFailures;
  std::unordered_map<std::string, std::chrono::milliseconds> m_playerPropertiesRefreshBackoffMs;
  std::deque<std::string> m_pendingDiscoveryBusNames;
  std::unordered_set<std::string> m_stoppedPlayers;
  std::string m_lastActivePlayer;
  std::string m_lastEmittedActivePlayer;
  std::optional<std::string> m_pinnedPlayerPreference;
  std::vector<std::string> m_preferredPlayers;
  std::vector<std::string> m_blacklist;
  std::function<void()> m_changeCallback;
  int m_startupRediscoveryPassesRemaining = 4;
  Timer m_recoveryTimer;
  std::chrono::milliseconds m_recoveryBackoffMs{500};
  bool m_discoveryDrainScheduled = false;
};
