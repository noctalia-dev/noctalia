#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <functional>
#include <string>

class Glyph;
class InputArea;
class Label;
class DesktopVolume : public Node {
public:
  struct Data {
    std::string glyph;
    std::string percent;
    std::string status;
    std::string device;
    float progress = 0.0F;
  };
  DesktopVolume();
  void setData(const Data& data);
  void setAppearance(const ColorSpec& fill, const ColorSpec& track, bool showDevice, bool shadow);
  void setOnScroll(std::function<void(float)> callback);
  void setOnToggleMute(std::function<void()> callback);
  void setFontFamily(const std::string& family);
  void setContentScale(float scale);
  void setViewport(float width, float height, float cornerRadius);

private:
  void doLayout(Renderer& renderer) override;
  void applyColors();
  void applyShadow();
  void updateFillClip();
  ColorSpec m_fillColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_trackColor = colorSpecFromRole(ColorRole::OnSurfaceVariant);
  bool m_showDevice = true;
  bool m_shadow = true;
  float m_contentScale = 1.0F;
  float m_viewWidth = 0.0F;
  float m_viewHeight = 0.0F;
  float m_cornerRadius = 0.0F;
  float m_glyphBox = 0.0F;
  float m_fillProgress = 0.0F;
  std::string m_lastDeviceText;
  InputArea* m_area = nullptr;
  InputArea* m_glyphHit = nullptr;
  Label* m_percentLabel = nullptr;
  Label* m_statusLabel = nullptr;
  Label* m_deviceLabel = nullptr;
  Glyph* m_trackGlyph = nullptr;
  Node* m_fillClip = nullptr;
  Glyph* m_fillGlyph = nullptr;
};
