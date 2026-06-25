#pragma once

#include <functional>
#include <string>
#include <string_view>

struct BarConfig;
struct BarMonitorOverride;
struct Config;
class ConfigService;
class Flex;

namespace settings {

  struct SettingsBarManagementContext {
    const Config& config;
    ConfigService* configService = nullptr;
    float scale = 1.0f;
    std::string_view searchQuery;
    std::string_view selectedSection;
    const BarConfig* selectedBar = nullptr;
    const BarMonitorOverride* selectedMonitorOverride = nullptr;

    std::string& renamingBarName;
    std::string& pendingDeleteBarName;
    std::string& renamingMonitorOverrideBarName;
    std::string& renamingMonitorOverrideMatch;
    std::string& pendingDeleteMonitorOverrideBarName;
    std::string& pendingDeleteMonitorOverrideMatch;

    std::function<void()> requestRebuild;
    std::function<void(std::string, std::string)> renameBar;
    std::function<void(std::string)> deleteBar;
    std::function<void(std::string, int)> moveBar;
    std::function<void(std::string, std::string, std::string)> renameMonitorOverride;
    std::function<void(std::string, std::string)> deleteMonitorOverride;
  };

  void addSettingsBarManagement(Flex& content, SettingsBarManagementContext ctx);

} // namespace settings
