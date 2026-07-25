#pragma once

#include "shell/bar/widget_definition.h"

#include <utility>
#include <vector>

namespace noctalia::bar {

  // Shared field set for bar buttons that draw a glyph with an optional custom image override.
  // `Options` must declare `glyph`, `customImage` and `customImageColorize` as its own members:
  // a common base struct would make `field<&Options::glyph>` deduce the base as the options type.
  // Any `extra` fields are appended after the shared ones.
  template <typename Options, typename... Extra>
  [[nodiscard]] std::vector<WidgetDefinitionField<Options>> glyphButtonFields(Extra&&... extra) {
    std::vector<WidgetDefinitionField<Options>> fields;
    fields.reserve(3 + sizeof...(Extra));
    fields.push_back(
        field<&Options::glyph>({
            .key = "glyph",
            .control = settings::WidgetControlKind::Glyph,
        })
    );
    fields.push_back(
        field<&Options::customImage>({
            .key = "custom_image",
        })
    );
    fields.push_back(
        field<&Options::customImageColorize>({
            .key = "custom_image_colorize",
        })
    );
    (fields.push_back(std::forward<Extra>(extra)), ...);
    return fields;
  }

} // namespace noctalia::bar
