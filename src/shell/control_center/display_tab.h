#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <string>
#include <vector>

class BrightnessService;
class ConfigService;
class Flex;
class Glyph;
class Label;
class Renderer;
class Slider;

class DisplayTab : public Tab {
public:
  DisplayTab(BrightnessService* brightness, ConfigService* config);

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;
  [[nodiscard]] bool dragging() const noexcept;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildCards(Renderer& renderer);
  void queueBrightness(const std::string& displayId, float value);
  void flushPendingBrightness(bool force = false);

  BrightnessService* m_brightness = nullptr;
  ConfigService* m_configService = nullptr;

  struct DisplayCard {
    std::string displayId;
    Flex* card = nullptr;
    Label* nameLabel = nullptr;
    Label* detailsLabel = nullptr;
    Glyph* icon = nullptr;
    Slider* slider = nullptr;
    Label* valueLabel = nullptr;
    float lastBrightness = -1.0f;
    bool lastControllable = true;
    std::string lastDisplayInfo;
  };

  Flex* m_rootLayout = nullptr;
  Flex* m_emptyState = nullptr;
  std::vector<DisplayCard> m_cards;
  std::string m_lastDisplayListKey;

  std::string m_pendingDisplayId;
  float m_pendingBrightness = -1.0f;
  float m_lastSentBrightness = -1.0f;
  std::chrono::steady_clock::time_point m_lastCommitAt{};
  std::chrono::steady_clock::time_point m_ignoreStateUntil{};
  Timer m_debounceTimer;
  bool m_syncingSlider = false;
  bool m_active = false;
};
