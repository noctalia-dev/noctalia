#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <array>
#include <string>
#include <vector>

class Box;
class Button;
class Flex;
class InputArea;
class Glyph;
class Image;
class Label;
class Renderer;
class ScreenTimeService;
class ScrollView;
class Segmented;

class ScreenTimeTab : public Tab {
public:
  explicit ScreenTimeTab(ScreenTimeService* screenTime);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncContent(Renderer& renderer);
  void bindAppNameMaxWidths(Renderer& renderer, float gridWidth);
  void layoutChart(Renderer& renderer);
  void layoutAppRows(Renderer& renderer);
  void beginRangeSlideOut(int nextRangeDays);
  void beginRangeSlideIn();
  void applyRangeSlide(float progress, bool slidingIn);
  void cancelRangeSlide();
  void syncEnabledUi();
  void syncDayLabelHover();
  void openDayDetail(std::size_t bucketIndex);
  void clearDayDetail();
  [[nodiscard]] std::string resolveIconPath(const std::string& appKey) const;

  ScreenTimeService* m_screenTime = nullptr;
  bool m_active = false;
  int m_rangeDays = 1;
  std::string m_detailDayKey;
  std::vector<std::string> m_bucketDayKeys;
  std::string m_lastSnapshotKey;
  int m_rangeSlideDirection = 0;
  int m_pendingRangeDays = 1;
  bool m_startRangeSlideIn = false;
  AnimationManager::Id m_rangeSlideAnimId = 0;
  float m_scrollBaseX = 0.0f;
  float m_scrollBaseY = 0.0f;

  Flex* m_root = nullptr;
  Flex* m_usageCard = nullptr;
  Flex* m_detailBar = nullptr;
  Label* m_disabledLabel = nullptr;
  Label* m_detailTitle = nullptr;
  Button* m_detailBackButton = nullptr;
  Segmented* m_rangePicker = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_chartPlotRow = nullptr;
  Flex* m_chartLabelRow = nullptr;
  Flex* m_mostUsedSection = nullptr;
  Flex* m_appsGrid = nullptr;
  Label* m_totalLabel = nullptr;
  Label* m_emptyLabel = nullptr;

  static constexpr int kMaxBuckets = 24;
  static constexpr int kMaxAppRows = 12;
  static constexpr int kMaxChartSeries = 5;
  static constexpr int kAppsPerRow = 2;
  static constexpr int kMostUsedRows = (kMaxAppRows + kAppsPerRow - 1) / kAppsPerRow;

  struct BucketColumnWidgets {
    Flex* plotColumn = nullptr;
    Box* track = nullptr;
    Label* label = nullptr;
    Flex* labelCell = nullptr;
    std::array<InputArea*, kMaxChartSeries> segmentHits{};
    std::array<Box*, kMaxChartSeries> segments{};
    std::array<float, kMaxChartSeries> segmentHeights{};
  };
  std::array<BucketColumnWidgets, kMaxBuckets> m_bucketColumns{};

  struct AppRowWidgets {
    Flex* cell = nullptr;
    Flex* row = nullptr;
    Box* chartSwatch = nullptr;
    Flex* iconSlot = nullptr;
    Image* icon = nullptr;
    Glyph* iconFallback = nullptr;
    Label* name = nullptr;
    Label* duration = nullptr;
    Flex* barHost = nullptr;
    Box* barTrack = nullptr;
    Box* barFill = nullptr;
    float barFillRatio = 0.0f;
    std::string iconPath;
  };
  std::array<AppRowWidgets, kMaxAppRows> m_appRows{};
  std::array<Flex*, kMostUsedRows> m_appGridRows{};
  Signal<>::ScopedConnection m_paletteConn;

  void updateIconForRow(Renderer& renderer, AppRowWidgets& widgets, const std::string& appKey, float scale);
};
