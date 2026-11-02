#pragma once

#include <nlohmann/json.hpp>

namespace calendar::detail {

  inline bool googleCalendarListItemSelected(const nlohmann::json& item) { return item.value("selected", false); }

} // namespace calendar::detail
