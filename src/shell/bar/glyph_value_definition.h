#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_definition.h"

#include <initializer_list>
#include <string>
#include <utility>

namespace noctalia::bar {

  // Shared fields for widgets that draw a glyph beside a value (`glyph_position`, `peek`, `peek_delay_ms`).
  // `Options` must declare `glyphPosition`, `peek` and `peekDelayMs` as its own members: a common base struct
  // would make `field<&Options::peek>` deduce the base as the options type.

  // Visible when every listed bool setting is true.
  inline settings::WidgetSettingVisibility whenAllTrue(std::initializer_list<std::string> keys) {
    settings::WidgetSettingVisibility visibility;
    for (const auto& key : keys) {
      visibility.all.push_back({.key = key, .values = {"true"}});
    }
    return visibility;
  }

  template <typename Options>
  WidgetDefinitionField<Options> glyphPositionField(
      settings::WidgetSettingVisibility visibleWhen, std::string group = "widget", std::string descriptionKey = {}
  ) {
    return field<&Options::glyphPosition>({
        .key = "glyph_position",
        .choices =
            {
                {
                    .value = GlyphPosition::Before,
                    .configValue = "before",
                    .labelKey = "settings.widgets.options.before",
                },
                {
                    .value = GlyphPosition::After,
                    .configValue = "after",
                    .labelKey = "settings.widgets.options.after",
                },
            },
        .presentation = settings::WidgetSettingPresentation{
            .descriptionKey = std::move(descriptionKey),
            .group = std::move(group),
            .segmented = true,
            .visibleWhen = std::move(visibleWhen),
        },
    });
  }

  template <typename Options>
  WidgetDefinitionField<Options>
  peekField(settings::WidgetSettingVisibility visibleWhen, std::string group = "widget") {
    return field<&Options::peek>({
        .key = "peek",
        .presentation = settings::WidgetSettingPresentation{
            .group = std::move(group),
            .visibleWhen = std::move(visibleWhen),
        },
    });
  }

  template <typename Options>
  WidgetDefinitionField<Options>
  peekDelayField(settings::WidgetSettingVisibility visibleWhen, std::string group = "widget") {
    visibleWhen.all.push_back({.key = "peek", .values = {"true"}});
    return field<&Options::peekDelayMs>({
        .key = "peek_delay_ms",
        .minValue = 0.0,
        .maxValue = 2000.0,
        .step = 25.0,
        .presentation = settings::WidgetSettingPresentation{
            .group = std::move(group),
            .visibleWhen = std::move(visibleWhen),
        },
    });
  }

} // namespace noctalia::bar
