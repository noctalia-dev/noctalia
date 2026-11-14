#include "core/ui_strings.h"

#include <utility>

namespace {

  UiStrings gUiStrings;

} // namespace

void setUiStrings(UiStrings strings) { gUiStrings = std::move(strings); }

const UiStrings& uiStrings() { return gUiStrings; }
