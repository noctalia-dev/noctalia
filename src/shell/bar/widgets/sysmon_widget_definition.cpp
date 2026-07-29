#include "shell/bar/widgets/sysmon_widget_definition.h"

namespace {

  settings::WidgetSettingVisibility labelMinWidthVisibility() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {
        {"display", {"gauge", "graph", "text"}},
        {"show_label", {"true"}},
    };
    return visibility;
  }

} // namespace

const noctalia::bar::WidgetDefinition<SysmonWidget::Options, SysmonWidgetDefinitionContext>& sysmonWidgetDefinition() {
  using noctalia::bar::field;
  using Options = SysmonWidget::Options;

  static const settings::WidgetSettingVisibility diskStat{
      "stat", {"disk_used_pct", "disk_used", "disk_free_pct", "disk_free"}
  };
  static const settings::WidgetSettingVisibility networkStat{"stat", {"net_rx", "net_tx"}};
  static const settings::WidgetSettingVisibility hasDisplay{"display", {"gauge", "graph", "text"}};
  static const settings::WidgetSettingVisibility showLabel{"show_label", {"true"}};

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
          field<&Options::glyph>({
              .key = "glyph",
              .control = settings::WidgetControlKind::Glyph,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .descriptionKey = "settings.widgets.settings.glyph.sysmon-description",
                  },
          }),
          field<&Options::customImage>({
              .key = "custom_image",
          }),
          field<&Options::customImageColorize>({
              .key = "custom_image_colorize",
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
          field<&Options::displayMode>({
              .key = "display",
              .choices =
                  {
                      {
                          .value = SysmonDisplayMode::Gauge,
                          .configValue = "gauge",
                          .labelKey = "settings.widgets.options.gauge",
                      },
                      {
                          .value = SysmonDisplayMode::Graph,
                          .configValue = "graph",
                          .labelKey = "settings.widgets.options.graph",
                      },
                      {
                          .value = SysmonDisplayMode::Text,
                          .configValue = "text",
                          .labelKey = "settings.widgets.options.text",
                      },
                      {
                          .value = SysmonDisplayMode::None,
                          .configValue = "none",
                          .labelKey = "settings.widgets.options.none",
                      },
                  },
              .presentation =
                  settings::WidgetSettingPresentation{
                      .segmented = true,
                  },
          }),
          field<&Options::highlightColor>({
              .key = "highlight_color",
          }),
          field<&Options::showLabel>({
              .key = "show_label",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = hasDisplay,
                  },
          }),
          field<&Options::labelMinWidth>({
              .key = "label_min_width",
              .minValue = 0.0,
              .maxValue = 200.0,
              .step = 1.0,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = labelMinWidthVisibility(),
                  },
          }),
          field<&Options::showUnits>({
              .key = "label_show_units",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = showLabel,
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
                      .segmented = true,
                      .visibleWhen = showLabel,
                  },
          }),
      },
      .finalize = [](Options& options, const SysmonWidgetDefinitionContext& context) {
        if (context.verticalBar && options.displayMode == SysmonDisplayMode::Graph) {
          options.displayMode = SysmonDisplayMode::Gauge;
        }
      },
  };
  return definition;
}
