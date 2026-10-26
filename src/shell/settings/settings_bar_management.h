#pragma once

#include <functional>
#include <string>
#include <string_view>

struct BarConfig;
struct BarMonitorOverride;
struct DockMonitorOverride;
struct Config;
class ConfigService;
class Flex;

namespace settings {

  struct SettingsBarManagementContext {
    const Config& config;
    ConfigService* configService = nullptr;
    float scale = 1.0F;
    std::string_view searchQuery;
    std::string_view selectedSection;
    const BarConfig* selectedBar = nullptr;
    const BarMonitorOverride* selectedMonitorOverride = nullptr;
    const DockMonitorOverride* selectedDockMonitorOverride = nullptr;

    std::string& renamingBarName;
    std::string& pendingDeleteBarName;
    std::string& renamingMonitorOverrideBarName;
    std::string& renamingMonitorOverrideMatch;
    std::string& pendingDeleteMonitorOverrideBarName;
    std::string& pendingDeleteMonitorOverrideMatch;
    std::string& renamingDockMonitorOverride;
    std::string& pendingDeleteDockMonitorOverride;

    std::function<void()> requestRebuild;
    std::function<void(std::string, std::string)> renameBar;
    std::function<void(std::string)> deleteBar;
    std::function<void(std::string, int)> moveBar;
    std::function<void(std::string, std::string, std::string)> renameMonitorOverride;
    std::function<void(std::string, std::string)> deleteMonitorOverride;
    std::function<void(std::string, std::string)> renameDockMonitorOverride;
    std::function<void(std::string)> deleteDockMonitorOverride;
  };

  void addSettingsBarManagement(Flex& content, SettingsBarManagementContext ctx);

} // namespace settings
