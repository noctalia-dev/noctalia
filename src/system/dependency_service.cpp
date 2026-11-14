#include "system/dependency_service.h"

#include "system/process.h"

#include <array>

namespace {

  // Optional CLI tools the shell knows about. Adding a new tracked tool is one line.
  constexpr std::array<const char*, 1> kTrackedTools = {
      "ddcutil",
  };

} // namespace

DependencyService::DependencyService() { rescan(); }

bool DependencyService::has(std::string_view name) const {
  const auto it = m_present.find(std::string(name));
  return it != m_present.end() && it->second;
}

void DependencyService::rescan() {
  m_present.clear();

  for (const char* name : kTrackedTools) {
    const bool present = process::commandExists(name);
    m_present.emplace(name, present);
  }
}
