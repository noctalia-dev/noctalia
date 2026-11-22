#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/file_columns.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ConfigService;

namespace filemanager {

  struct SavedPlace {
    std::string id;
    std::filesystem::path path;
    std::string label;
  };

  struct FileViewSettings {
    bool gridMode = false;
    bool showHidden = false;
    bool foldersFirst = true;
    bool hiddenLast = true;
    FileDialogSortField sortField = FileDialogSortField::Name;
    FileDialogSortOrder sortOrder = FileDialogSortOrder::Ascending;
  };

  class FileManagerSettings {
  public:
    explicit FileManagerSettings(ConfigService& config) : m_config(config) {}

    [[nodiscard]] FileViewSettings loadFileView() const;
    [[nodiscard]] bool saveFileView(const FileViewSettings& settings);

    [[nodiscard]] bool showApplicationMenuBar() const;
    [[nodiscard]] bool setShowApplicationMenuBar(bool show);

    [[nodiscard]] bool alwaysShowTabBar() const;
    [[nodiscard]] bool setAlwaysShowTabBar(bool show);

    [[nodiscard]] bool openTabsInBackground() const;
    [[nodiscard]] bool setOpenTabsInBackground(bool background);

    [[nodiscard]] int adminInactivityTimeoutSeconds() const;
    [[nodiscard]] bool setAdminInactivityTimeoutSeconds(int seconds);

    void loadListColumnWidths(FileColumns& out) const;
    [[nodiscard]] bool saveListColumnWidths(const FileColumns& columns);

    void loadListColumnLayout(FileColumnLayout& out) const;
    [[nodiscard]] bool saveListColumnLayout(const FileColumnLayout& layout);

    [[nodiscard]] float loadViewScale(float defaultScale = 1.0F) const;
    [[nodiscard]] bool saveViewScale(float scale);

    [[nodiscard]] float loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const;
    [[nodiscard]] bool saveSidebarWidth(float width);

    [[nodiscard]] float loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const;
    [[nodiscard]] bool saveStatusHeight(float height);

    [[nodiscard]] std::vector<SavedPlace> loadSavedPlaces() const;
    [[nodiscard]] bool saveSavedPlaces(const std::vector<SavedPlace>& places);

    [[nodiscard]] std::vector<std::string> loadPlaceOrder() const;
    [[nodiscard]] bool savePlaceOrder(const std::vector<std::string>& order);

    [[nodiscard]] std::vector<std::string> loadHiddenMounts() const;
    [[nodiscard]] bool saveHiddenMounts(const std::vector<std::string>& identities);

    [[nodiscard]] bool placesSectionExpanded(std::string_view key) const;
    [[nodiscard]] bool setPlacesSectionExpanded(std::string_view key, bool expanded);

    [[nodiscard]] static std::string dialogSizeStateKey(std::string_view title, bool conflictDialog);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadDialogSize(std::string_view key) const;
    [[nodiscard]] bool saveDialogSize(std::string_view key, std::uint32_t width, std::uint32_t height);

  private:
    ConfigService& m_config;
  };

} // namespace filemanager
