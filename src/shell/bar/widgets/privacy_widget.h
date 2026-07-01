#pragma once

#include "pipewire/privacy_filter.h"
#include "shell/bar/widget.h"
#include "shell/tooltip/tooltip_content.h"
#include "ui/palette.h"

#include <string>
#include <vector>

class Glyph;
class ConfigService;
class InputArea;
class PipeWireService;

struct PrivacyWidgetConfig {
  bool hideInactive = false;
  int iconSpacing = 4;
  ColorSpec activeColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec inactiveColor = colorSpecFromRole(ColorRole::Outline);
};

class PrivacyWidget : public Widget {
public:
  PrivacyWidget(PipeWireService* pipewire, ConfigService* configService, PrivacyWidgetConfig config);

  void create() override;

private:
  struct Snapshot {
    std::vector<std::string> micApps;
    std::vector<std::string> cameraApps;
    std::vector<std::string> screenApps;
    bool vertical = false;

    [[nodiscard]] bool micActive() const { return !micApps.empty(); }
    [[nodiscard]] bool cameraActive() const { return !cameraApps.empty(); }
    [[nodiscard]] bool screenActive() const { return !screenApps.empty(); }
    [[nodiscard]] bool anyActive() const { return micActive() || cameraActive() || screenActive(); }
    [[nodiscard]] bool visible(bool hideInactive) const { return !hideInactive || anyActive(); }

    bool operator==(const Snapshot&) const = default;
  };

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState();
  void refreshFilters() const;
  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] std::vector<TooltipRow> buildTooltipRows() const;

  PipeWireService* m_pipewire = nullptr;
  ConfigService* m_configService = nullptr;
  PrivacyWidgetConfig m_config;
  mutable PrivacyFilter m_micFilter;
  mutable PrivacyFilter m_camFilter;
  mutable PrivacyFilter m_screenFilter;

  InputArea* m_area = nullptr;
  Glyph* m_micGlyph = nullptr;
  Glyph* m_cameraGlyph = nullptr;
  Glyph* m_screenGlyph = nullptr;

  std::optional<Snapshot> m_cachedSnapshot;
  bool m_isVertical = false;
};
