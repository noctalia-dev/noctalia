#pragma once

#include "shell/bar/widget.h"

#include <cstdint>

struct Config;
class Glyph;
class Label;
class PipeWireService;
struct wl_output;

enum class VolumeWidgetTarget {
  Output,
  Input,
};

class VolumeWidget : public Widget {
public:
  VolumeWidget(
      PipeWireService* audio, const Config* config, wl_output* output, bool showLabel, VolumeWidgetTarget target
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  PipeWireService* m_audio = nullptr;
  const Config* m_config = nullptr;
  bool m_showLabel = true;
  VolumeWidgetTarget m_target = VolumeWidgetTarget::Output;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  float m_lastVolume = -1.0f;
  bool m_lastMuted = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
