#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Config;
struct ScrollViewState;
class Flex;
class Node;
class RovingListNavHost;

namespace settings {
  enum class SettingsSection : std::uint8_t;

  struct SettingsSidebarContext {
    const Config& config;
    const std::vector<SettingsSection>& sections;
    const std::vector<std::string>& availableBars;
    float scale = 1.0f;
    bool globalSearchActive = false;

    ScrollViewState& sidebarScrollState;
    ScrollViewState& contentScrollState;
    std::string& selectedSection;
    std::string& selectedBarName;
    std::string& selectedMonitorOverride;
    std::string& creatingBarName;
    std::string& creatingMonitorOverrideBarName;
    std::string& creatingMonitorOverrideMatch;

    std::function<void()> clearTransientState;
    std::function<void()> clearSearchQuery;
    std::function<void()> requestRebuild;
    std::function<void(std::string)> createBar;
    std::function<void(std::string, std::string)> createMonitorOverride;
    std::function<void(const Node*)> scrollSidebarNodeIntoView;
    RovingListNavHost** outNav = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx);

} // namespace settings
