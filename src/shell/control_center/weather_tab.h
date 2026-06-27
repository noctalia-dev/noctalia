#pragma once

#include "render/core/render_styles.h"
#include "shell/control_center/tab.h"

#include <array>
#include <cstdint>

class EffectNode;
class Flex;
class Glyph;
class Label;
class Separator;
class WeatherService;
class ConfigService;

class WeatherTab : public Tab {
public:
  WeatherTab(WeatherService* weather, ConfigService* config);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);
  void setForecastVisibleDayCount(std::size_t count);
  void showLocationPrompt(bool show);
  void hideEffect();
  [[nodiscard]] static std::string todayIso(std::int32_t utcOffsetSeconds);
  [[nodiscard]] static std::string weekdayLabel(const std::string& isoDate);
  [[nodiscard]] static EffectType effectForWeatherCode(std::int32_t code, bool isDay);

  static constexpr std::size_t kDayCount = 7;
  static constexpr std::size_t kDetailRowCount = 6;

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
  std::array<Flex*, kDetailRowCount> m_detailRows{};
  std::array<Flex*, kDayCount> m_dayRows{};
  std::array<Separator*, kDayCount - 1> m_daySeparators{};
  std::array<Flex*, kDayCount> m_dayIconSlots{};
  std::array<Glyph*, kDayCount> m_dayGlyphs{};
  std::array<Label*, kDayCount> m_dayMetas{};
  std::array<Label*, kDayCount> m_dayDescs{};
  std::array<Label*, kDayCount> m_dayTemps{};
  EffectNode* m_effectNode = nullptr;
  EffectType m_activeEffect = EffectType::None;
  float m_shaderTime = 0.0f;
};
