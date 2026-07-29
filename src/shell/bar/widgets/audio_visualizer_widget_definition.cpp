#include "shell/bar/widgets/audio_visualizer_widget_definition.h"

const noctalia::bar::WidgetDefinition<AudioVisualizerWidget::Options>& audioVisualizerWidgetDefinition() {
  using noctalia::bar::field;
  using Options = AudioVisualizerWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "audio_visualizer",
      .fields = {
          field<&Options::width>({
              .key = "width",
              .minValue = 8.0,
              .maxValue = 400.0,
              .step = 1.0,
          }),
          field<&Options::bands>({
              .key = "bands",
              .minValue = 2.0,
              .maxValue = 128.0,
              .step = 1.0,
          }),
          field<&Options::mirrored>({
              .key = "mirrored",
          }),
          field<&Options::centered>({
              .key = "centered",
          }),
          field<&Options::showWhenIdle>({
              .key = "show_when_idle",
          }),
          field<&Options::color1>({
              .key = "color_1",
          }),
          field<&Options::color2>({
              .key = "color_2",
          }),
      },
  };
  return definition;
}
