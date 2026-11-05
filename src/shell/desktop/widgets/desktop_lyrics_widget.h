#pragma once

#include "core/timer_manager.h"
#include "shell/desktop/desktop_widget.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class KaraokeLabel;
class LyricsService;
class MprisService;
struct LyricsDocument;
struct LyricsResult;

class DesktopLyricsWidget : public DesktopWidget {
public:
  struct Options {
    int lineCount = 3;
    int syncOffsetMs = 0;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    ColorSpec activeColor = colorSpecFromRole(ColorRole::Primary);
    bool shadow = true;
  };

  DesktopLyricsWidget(MprisService& mpris, LyricsService& lyrics, Options options);
  void create() override;
  bool wantsSecondTicks() const override { return true; }
  bool recoversStalledFrames() const override { return true; }
  bool needsFrameTick() const override { return m_animateHighlight || m_scrollAnimId != 0; }
  void onFrameTick(float deltaMs, Renderer& renderer) override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doUpdate(Renderer& renderer) override;
  void doLayout(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void rebuildLabels();
  void syncHighlight();
  void startScroll(std::chrono::milliseconds duration);
  void applyScroll();
  void stopScroll();
  void triggerLineUpdate();
  std::int64_t lineLeadMs() const;
  std::int64_t lineHoldMs() const;
  std::optional<std::size_t> selectLine(const LyricsDocument& document, std::int64_t positionMs) const;
  std::size_t labelCount() const;
  void refreshWidthCache(Renderer& renderer);
  float songWidthAtScale(Renderer& renderer, float scale);

  struct WidthCacheKey {
    std::shared_ptr<const LyricsResult> result;
    std::vector<std::string> statusRows;
    std::string fontFamily;
    float baseScale = 0.0F;
    float renderScale = 0.0F;
    std::uint64_t fontGeneration = 0;
    int lineCount = 0;

    bool operator==(const WidthCacheKey&) const = default;
  };

  struct WidthMeasurement {
    float scale = 0.0F;
    float width = 0.0F;
  };

  struct FittedMeasurement {
    float requestedScale = 0.0F;
    float availableWidth = 0.0F;
    WidthMeasurement measured;
  };

  MprisService& m_mpris;
  LyricsService& m_lyrics;
  Options m_options;
  std::vector<KaraokeLabel*> m_labels;
  std::vector<std::string> m_rows;
  std::optional<std::size_t> m_activeRow;
  std::optional<std::size_t> m_activeLine;
  std::optional<std::size_t> m_laidOutLine;
  std::optional<std::size_t> m_laidOutRow;
  std::uint64_t m_laidOutGeneration = 0;
  bool m_animateHighlight = false;
  std::uint32_t m_scrollAnimId = 0;
  // Rows the labels still sit below their resting place while scrolling to a new line.
  float m_scrollRows = 0.0F;
  float m_rowHeight = 0.0F;
  std::vector<float> m_labelBaseY;
  // Slow-decaying peak of the time from a line becoming due until it is laid out and presented, and the frame interval.
  float m_lineLatencyMs = 0.0F;
  float m_frameMs = 0.0F;
  bool m_latencyMeasured = false;
  std::optional<std::chrono::steady_clock::time_point> m_lineTriggerAt;
  std::optional<std::chrono::steady_clock::time_point> m_lineMeasureStart;
  std::string m_requestKey;
  std::uint64_t m_requestGeneration = 0;
  bool m_offline = false;
  std::shared_ptr<const LyricsResult> m_result;
  std::chrono::steady_clock::time_point m_retryAt;
  std::string m_lookupAlbum;
  std::string m_lookupTitle;
  std::string m_lookupBus;
  std::string m_lookupTrackId;
  // Where playback was at the previous update, to tell a continuing track from a new one.
  struct LastSeen {
    std::string busName;
    std::int64_t positionUs = 0;
    double rate = 1.0;
    bool playing = false;
    std::chrono::steady_clock::time_point at;
  };
  std::optional<LastSeen> m_lastSeen;
  bool m_holding = false;
  std::int64_t m_lookupDurationUs = 0;
  Timer m_updateTimer;
  std::optional<WidthCacheKey> m_widthCacheKey;
  std::vector<WidthMeasurement> m_widthMeasurements;
  std::optional<FittedMeasurement> m_fittedMeasurement;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
