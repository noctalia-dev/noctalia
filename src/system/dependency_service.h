#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

class DependencyService {
public:
  DependencyService();

  bool has(std::string_view name) const;
  bool hasDdcutil() const { return has("ddcutil"); }

  void rescan();

private:
  std::unordered_map<std::string, bool> m_present;
};
