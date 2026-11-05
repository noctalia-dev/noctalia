#pragma once

#include <string>
#include <string_view>

namespace noctalia::build_info {

  std::string_view version() noexcept;

  std::string_view revision() noexcept;

  std::string displayVersion();

} // namespace noctalia::build_info
