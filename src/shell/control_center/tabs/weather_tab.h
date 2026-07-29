#pragma once

#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "shell/control_center/tab.h"

#include <array>
#include <cstdint>

class EffectNode;
class Flex;
class Glyph;
class InputArea;
class Label;
class Separator;
class Segmented;
class WeatherService;
class ConfigService;
struct WeatherSnapshot;

class WeatherTab : public Tab {
public:
  WeatherTab(WeatherService* weather, ConfigService* config);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;

private:
  enum class ForecastView : std::uint8_t {
    Daily = 0,
    Hourly = 1,
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);
  void beginForecastSlideOut(ForecastView nextView);
  void beginForecastSlideIn();
  void applyForecastSlide(float progress, bool slidingIn);
  void cancelForecastSlide();
  void setForecastVisibleRowCount(std::size_t count);
  void syncDailyForecast(Renderer& renderer, const WeatherSnapshot& snapshot);
  void syncHourlyForecast(Renderer& renderer, const WeatherSnapshot& snapshot);
  void showLocationPrompt(bool show);
  void hideEffect();
  [[nodiscard]] static std::string todayIso(std::int32_t utcOffsetSeconds);
  [[nodiscard]] static std::string hourLabel(const std::string& isoTime, const std::string& timeFormat);
  [[nodiscard]] static std::string weekdayLabel(const std::string& isoDate);
  [[nodiscard]] static EffectType effectForWeatherCode(std::int32_t code, bool isDay);

  static constexpr std::size_t kForecastRowCount = 7;
  static constexpr std::size_t kDetailRowCount = 7;

  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_leftColumn = nullptr;
  Flex* m_currentCard = nullptr;
  Flex* m_glyphColumn = nullptr;
  Flex* m_detailsCard = nullptr;
  Flex* m_currentText = nullptr;
  Flex* m_locationPrompt = nullptr;
  Glyph* m_locationPromptGlyph = nullptr;
  Label* m_locationPromptBody = nullptr;
  Flex* m_forecastColumn = nullptr;
  Flex* m_forecastRowsContainer = nullptr;
  Segmented* m_forecastViewPicker = nullptr;
  Label* m_statusLabel = nullptr;
  Glyph* m_currentGlyph = nullptr;
  Label* m_currentTempLabel = nullptr;
  Label* m_currentHiLoLabel = nullptr;
  Label* m_currentDescLabel = nullptr;
  Label* m_updatedLabel = nullptr;
  Label* m_windLabel = nullptr;
  Label* m_sunriseLabel = nullptr;
  Label* m_sunsetLabel = nullptr;
  Label* m_tempMaxLabel = nullptr;
  Label* m_tempMinLabel = nullptr;
  Label* m_elevationLabel = nullptr;
  Label* m_timeZoneLabel = nullptr;
  Flex* m_timeZoneRow = nullptr;
  Label* m_uvIndexLabel = nullptr;
  std::array<Flex*, kDetailRowCount> m_detailRows{};
  std::array<Flex*, kForecastRowCount> m_forecastRows{};
  std::array<Separator*, kForecastRowCount - 1> m_forecastSeparators{};
  std::array<Flex*, kForecastRowCount> m_forecastIconSlots{};
  std::array<Glyph*, kForecastRowCount> m_forecastGlyphs{};
  std::array<Label*, kForecastRowCount> m_forecastMetas{};
  std::array<Label*, kForecastRowCount> m_forecastDescs{};
  std::array<Label*, kForecastRowCount> m_forecastTemps{};
  std::array<InputArea*, kForecastRowCount> m_forecastHitAreas{};
  EffectNode* m_effectNode = nullptr;
  EffectType m_activeEffect = EffectType::None;
  ForecastView m_forecastView = ForecastView::Daily;
  int m_forecastSlideDirection = 0;
  ForecastView m_pendingForecastView = ForecastView::Daily;
  bool m_startForecastSlideIn = false;
  AnimationManager::Id m_forecastSlideAnimId = 0;
  float m_forecastRowsBaseX = 0.0f;
  float m_forecastRowsBaseY = 0.0f;
  float m_shaderTime = 0.0f;
};
