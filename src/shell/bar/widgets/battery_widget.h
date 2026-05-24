#pragma once

#include "dbus/upower/upower_service.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <string>

class Box;
class Glyph;
class Label;

enum class BatteryDisplayMode : std::uint8_t { Graphic, Icon };

class BatteryWidget : public Widget {
public:
  BatteryWidget(
      UPowerService* upower, std::string deviceSelector = "auto", int warningThreshold = 0, ColorSpec warningColor = {},
      BatteryDisplayMode displayMode = BatteryDisplayMode::Icon, bool showLabel = true, bool hideWhenPlugged = false,
      bool hideWhenFull = false
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool needsFrameTick() const override;
  void syncState(Renderer& renderer);
  void updateFillGeometry();

  void createGraphicMode();
  void createIconMode();
  void layoutGraphicMode(Renderer& renderer);
  void layoutIconMode(Renderer& renderer, float containerWidth, float containerHeight);

  UPowerService* m_upower = nullptr;
  std::string m_deviceSelector = "auto";
  int m_warningThreshold = 0;
  ColorSpec m_warningColor;
  BatteryDisplayMode m_displayMode = BatteryDisplayMode::Icon;
  bool m_showLabel = true;
  bool m_hideWhenPlugged = false;
  bool m_hideWhenFull = false;

  // Icon mode nodes
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;

  // Graphic mode nodes
  Box* m_bodyBg = nullptr;
  Box* m_fillRect = nullptr;
  Box* m_terminalNub = nullptr;
  Label* m_overlayLabel = nullptr;
  Glyph* m_overlayGlyph = nullptr;

  // Animated fill
  float m_animatedPct = 0.0f;
  AnimationManager::Id m_fillAnim = 0;

  double m_lastPct = -1.0;
  BatteryState m_lastState = BatteryState::Unknown;
  bool m_lastPresent = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
