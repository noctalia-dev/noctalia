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
    float scale = 1.0F;
    bool globalSearchActive = false;

    ScrollViewState& sidebarScrollState;
    ScrollViewState& contentScrollState;
    std::string& selectedSection;
    std::string& selectedBarName;
    std::string& selectedMonitorOverride;
    std::string& creatingBarName;

    std::function<void()> clearTransientState;
    std::function<void()> clearSearchQuery;
    std::function<void()> requestRebuild;
    std::function<void(std::string)> createBar;
    // Opens the "new monitor override" dialog for the given bar. The create flow lives in a modal
    // (wide enough for the output picker) instead of the narrow sidebar.
    std::function<void(std::string)> openMonitorOverrideCreate;
    std::function<void(const Node*)> scrollSidebarNodeIntoView;
    RovingListNavHost** outNav = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx);

} // namespace settings
