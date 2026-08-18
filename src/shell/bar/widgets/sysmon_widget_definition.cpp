#include "shell/bar/widgets/sysmon_widget_definition.h"

const noctalia::bar::WidgetDefinition<SysmonWidget::Options, SysmonWidgetDefinitionContext>& sysmonWidgetDefinition() {
  using noctalia::bar::field;
  using Options = SysmonWidget::Options;

  static const settings::WidgetSettingVisibility diskStat{
      "stat", {"disk_used_pct", "disk_used", "disk_free_pct", "disk_free"}
  };
  static const settings::WidgetSettingVisibility networkStat{"stat", {"net_rx", "net_tx"}};
  static const settings::WidgetSettingVisibility hasVisualization{"visualization", {"gauge", "graph"}};
  static const settings::WidgetSettingVisibility showValue{"show_value", {"true"}};
  static const settings::WidgetSettingVisibility showGlyph{"show_glyph", {"true"}};
  settings::WidgetSettingVisibility glyphPosition;
  glyphPosition.any = {
      hasVisualization.any[0],
      showValue.any[0],
  };
  glyphPosition.all = {
      showGlyph.any[0],
  };

  static const noctalia::bar::WidgetDefinition<Options, SysmonWidgetDefinitionContext> definition{
      .type = "sysmon",
      .fields = {
          field<&Options::stat>({
              .key = "stat",
              .choices =
                  {
                      {
                          .value = SysmonStat::CpuUsage,
                          .configValue = "cpu_usage",
                          .labelKey = "settings.widgets.options.cpu-usage",
                      },
                      {
                          .value = SysmonStat::CpuTemp,
                          .configValue = "cpu_temp",
                          .labelKey = "settings.widgets.options.cpu-temp",
                      },
                      {
                          .value = SysmonStat::CpuFreq,
                          .configValue = "cpu_freq",
                          .labelKey = "settings.widgets.options.cpu-freq",
                      },
                      {
                          .value = SysmonStat::GpuTemp,
                          .configValue = "gpu_temp",
                          .labelKey = "settings.widgets.options.gpu-temp",
                      },
                      {
                          .value = SysmonStat::GpuUsage,
                          .configValue = "gpu_usage",
                          .labelKey = "settings.widgets.options.gpu-usage",
                      },
                      {
                          .value = SysmonStat::GpuVram,
                          .configValue = "gpu_vram",
                          .labelKey = "settings.widgets.options.gpu-vram",
                      },
                      {
                          .value = SysmonStat::GpuVramUsed,
                          .configValue = "gpu_vram_used",
                          .labelKey = "settings.widgets.options.gpu-vram-used",
                      },
                      {
                          .value = SysmonStat::RamUsed,
                          .configValue = "ram_used",
                          .labelKey = "settings.widgets.options.ram-used",
                      },
                      {
                          .value = SysmonStat::RamPct,
                          .configValue = "ram_pct",
                          .labelKey = "settings.widgets.options.ram-percent",
                      },
                      {
                          .value = SysmonStat::SwapPct,
                          .configValue = "swap_pct",
                          .labelKey = "settings.widgets.options.swap-percent",
                      },
                      {
                          .value = SysmonStat::DiskUsedPct,
                          .configValue = "disk_used_pct",
                          .labelKey = "settings.widgets.options.disk-used-percent",
                      },
                      {
                          .value = SysmonStat::DiskUsed,
                          .configValue = "disk_used",
                          .labelKey = "settings.widgets.options.disk-used",
                      },
                      {
                          .value = SysmonStat::DiskFreePct,
                          .configValue = "disk_free_pct",
                          .labelKey = "settings.widgets.options.disk-free-percent",
                      },
                      {
                          .value = SysmonStat::DiskFree,
                          .configValue = "disk_free",
                          .labelKey = "settings.widgets.options.disk-free",
                      },
                      {
                          .value = SysmonStat::NetRx,
                          .configValue = "net_rx",
                          .labelKey = "settings.widgets.options.net-rx",
                      },
                      {
                          .value = SysmonStat::NetTx,
                          .configValue = "net_tx",
                          .labelKey = "settings.widgets.options.net-tx",
                      },
                  },
          }),
          field<&Options::diskPath>({
              .key = "path",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = diskStat,
                  },
          }),
          field<&Options::networkInterface>({
              .key = "interface",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = networkStat,
                  },
          }),
          field<&Options::networkSpeedUnit>({
              .key = "network_speed_unit",
              .choices =
                  {
                      {
                          .value = FormatUnits::DecimalByteRateUnit::Auto,
                          .configValue = "auto",
                          .labelKey = "settings.widgets.options.auto",
                      },
                      {
                          .value = FormatUnits::DecimalByteRateUnit::Kilobytes,
                          .configValue = "kb",
                          .labelKey = "settings.widgets.options.kilobytes",
                      },
                      {
                          .value = FormatUnits::DecimalByteRateUnit::Megabytes,
                          .configValue = "mb",
                          .labelKey = "settings.widgets.options.megabytes",
                      },
                  },
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = networkStat,
                  },
          }),
          field<&Options::networkSpeedCompact>({
              .key = "network_speed_compact",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = networkStat,
                  },
          }),
          field<&Options::visualization>({
              .key = "visualization",
              .choices =
                  {
                      {
                          .value = SysmonVisualization::Gauge,
                          .configValue = "gauge",
                          .labelKey = "settings.widgets.options.gauge",
                      },
                      {
                          .value = SysmonVisualization::Graph,
                          .configValue = "graph",
                          .labelKey = "settings.widgets.options.graph",
                      },
                      {
                          .value = SysmonVisualization::None,
                          .configValue = "none",
                          .labelKey = "settings.widgets.options.none",
                      },
                  },
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .segmented = true,
                  },
          }),
          field<&Options::showValue>({
              .key = "show_value",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                  },
          }),
          field<&Options::showUnits>({
              .key = "label_show_units",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .visibleWhen = showValue,
                  },
          }),
          field<&Options::labelMinWidth>({
              .key = "label_min_width",
              .minValue = 0.0,
              .maxValue = 200.0,
              .step = 1.0,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .visibleWhen = showValue,
                  },
          }),
          field<&Options::showGlyph>({
              .key = "show_glyph",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                  },
          }),
          field<&Options::glyph>({
              .key = "glyph",
              .control = settings::WidgetControlKind::Glyph,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .descriptionKey = "settings.widgets.settings.glyph.sysmon-description",
                      .group = "presentation",
                      .visibleWhen = showGlyph,
                  },
          }),
          field<&Options::customImage>({
              .key = "custom_image",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .visibleWhen = showGlyph,
                  },
          }),
          field<&Options::customImageColorize>({
              .key = "custom_image_colorize",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .visibleWhen = showGlyph,
                  },
          }),
          field<&Options::glyphPosition>({
              .key = "glyph_position",
              .choices =
                  {
                      {
                          .value = SysmonGlyphPosition::Before,
                          .configValue = "before",
                          .labelKey = "settings.widgets.options.before",
                      },
                      {
                          .value = SysmonGlyphPosition::After,
                          .configValue = "after",
                          .labelKey = "settings.widgets.options.after",
                      },
                  },
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                      .segmented = true,
                      .visibleWhen = glyphPosition,
                  },
          }),
          field<&Options::highlightColor>({
              .key = "highlight_color",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .group = "presentation",
                  },
          }),
      },
      .finalize = [](Options& options, const SysmonWidgetDefinitionContext& context) {
        if (context.verticalBar && options.visualization == SysmonVisualization::Graph) {
          options.visualization = SysmonVisualization::Gauge;
        }
      },
      .glyph = [](const Options& options) {
        return options.glyph.empty() ? std::string(SysmonWidget::glyphName(options.stat)) : options.glyph;
      },
      .validateOptions = [](const Options& options) -> std::optional<std::string> {
        if (!options.showGlyph && !options.showValue && options.visualization == SysmonVisualization::None) {
          return "show_glyph, show_value, and visualization cannot all be disabled";
        }
        return std::nullopt;
      },
  };
  return definition;
}
