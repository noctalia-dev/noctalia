#include "filemanager/file_manager_settings.h"

#include "config/config_service.h"
#include "ui/dialogs/file_view_metrics.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace filemanager {

  namespace {
    constexpr std::string_view kNamespace = "files";

    float loadClampedFloat(
        ConfigService& config, std::string_view key, float defaultValue, float minValue, float maxValue
    ) {
      if (const auto stored = config.stateString(std::string(kNamespace), std::string(key))) {
        try {
          const float value = std::stof(*stored);
          if (std::isfinite(value))
            return std::clamp(value, minValue, maxValue);
        } catch (...) {
        }
      }
      return defaultValue;
    }
  } // namespace

  FileViewSettings FileManagerSettings::loadFileView() const {
    FileViewSettings settings;
    settings.gridMode = m_config.stateBool(std::string(kNamespace), "grid_view").value_or(false);
    settings.showHidden = m_config.stateBool(std::string(kNamespace), "show_hidden").value_or(false);
    settings.foldersFirst = m_config.stateBool(std::string(kNamespace), "folders_first").value_or(true);
    settings.hiddenLast = m_config.stateBool(std::string(kNamespace), "hidden_last").value_or(true);
    if (const auto stored = m_config.stateString(std::string(kNamespace), "sort_field")) {
      int value = 0;
      const auto parsed = std::from_chars(stored->data(), stored->data() + stored->size(), value);
      if (parsed.ec == std::errc{} && parsed.ptr == stored->data() + stored->size() && value >= 0 && value <= 5)
        settings.sortField = static_cast<FileDialogSortField>(value);
    }
    settings.sortOrder = m_config.stateBool(std::string(kNamespace), "sort_descending").value_or(false)
        ? FileDialogSortOrder::Descending
        : FileDialogSortOrder::Ascending;
    return settings;
  }

  bool FileManagerSettings::saveFileView(const FileViewSettings& settings) {
    bool saved = m_config.setStateBool(std::string(kNamespace), "show_hidden", settings.showHidden);
    saved = m_config.setStateBool(std::string(kNamespace), "folders_first", settings.foldersFirst) && saved;
    saved = m_config.setStateBool(std::string(kNamespace), "hidden_last", settings.hiddenLast) && saved;
    saved = m_config.setStateString(
                std::string(kNamespace), "sort_field", std::to_string(static_cast<int>(settings.sortField))
            )
        && saved;
    saved = m_config.setStateBool(
                std::string(kNamespace), "sort_descending", settings.sortOrder == FileDialogSortOrder::Descending
            )
        && saved;
    saved = m_config.setStateBool(std::string(kNamespace), "grid_view", settings.gridMode) && saved;
    return saved;
  }

  bool FileManagerSettings::showApplicationMenuBar() const {
    return m_config.stateBool(std::string(kNamespace), "show_menu_bar").value_or(true);
  }

  bool FileManagerSettings::setShowApplicationMenuBar(bool show) {
    return m_config.setStateBool(std::string(kNamespace), "show_menu_bar", show);
  }

  bool FileManagerSettings::alwaysShowTabBar() const {
    return m_config.stateBool(std::string(kNamespace), "always_show_tab_bar").value_or(false);
  }

  bool FileManagerSettings::setAlwaysShowTabBar(bool show) {
    return m_config.setStateBool(std::string(kNamespace), "always_show_tab_bar", show);
  }

  bool FileManagerSettings::openTabsInBackground() const {
    return m_config.stateBool(std::string(kNamespace), "open_tabs_in_background").value_or(false);
  }

  bool FileManagerSettings::setOpenTabsInBackground(bool background) {
    return m_config.setStateBool(std::string(kNamespace), "open_tabs_in_background", background);
  }

  int FileManagerSettings::adminInactivityTimeoutSeconds() const {
    constexpr int kDefaultSeconds = 600; // 10 minutes
    constexpr int kMinSeconds = 60;      // 1 minute
    constexpr int kMaxSeconds = 3600;    // 60 minutes
    if (const auto stored = m_config.stateString(std::string(kNamespace), "admin_inactivity_timeout_seconds")) {
      int parsedValue = 0;
      const auto parsed = std::from_chars(stored->data(), stored->data() + stored->size(), parsedValue);
      if (parsed.ec == std::errc{} && parsed.ptr == stored->data() + stored->size() && parsedValue > 0)
        return std::clamp(parsedValue, kMinSeconds, kMaxSeconds);
    }
    return kDefaultSeconds;
  }

  bool FileManagerSettings::setAdminInactivityTimeoutSeconds(int seconds) {
    constexpr int kMinSeconds = 60;
    constexpr int kMaxSeconds = 3600;
    if (seconds < kMinSeconds || seconds > kMaxSeconds)
      return false;
    return m_config.setStateString(
        std::string(kNamespace), "admin_inactivity_timeout_seconds", std::to_string(seconds)
    );
  }

  void FileManagerSettings::loadListColumnWidths(FileColumns& out) const {
    if (const auto widths = m_config.stateString(std::string(kNamespace), "list_column_widths"))
      parseFileColumns(*widths, out);
  }

  bool FileManagerSettings::saveListColumnWidths(const FileColumns& columns) {
    return m_config.setStateString(std::string(kNamespace), "list_column_widths", serializeFileColumns(columns));
  }

  void FileManagerSettings::loadListColumnLayout(FileColumnLayout& out) const {
    if (const auto columns = m_config.stateString(std::string(kNamespace), "list_column_layout"))
      parseFileColumnLayout(*columns, out);
  }

  bool FileManagerSettings::saveListColumnLayout(const FileColumnLayout& layout) {
    return m_config.setStateString(std::string(kNamespace), "list_column_layout", serializeFileColumnLayout(layout));
  }

  float FileManagerSettings::loadViewScale(float defaultScale) const {
    if (const auto stored = m_config.stateString(std::string(kNamespace), "view_scale")) {
      try {
        const float scale = std::stof(*stored);
        if (std::isfinite(scale))
          return clampFileViewScale(scale);
      } catch (...) {
      }
    }
    return defaultScale;
  }

  bool FileManagerSettings::saveViewScale(float scale) {
    return m_config.setStateString(std::string(kNamespace), "view_scale", std::to_string(scale));
  }

  float FileManagerSettings::loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const {
    return loadClampedFloat(m_config, "sidebar_width", defaultWidth, minWidth, maxWidth);
  }

  bool FileManagerSettings::saveSidebarWidth(float width) {
    return m_config.setStateString(std::string(kNamespace), "sidebar_width", std::to_string(width));
  }

  float FileManagerSettings::loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const {
    return loadClampedFloat(m_config, "status_height", defaultHeight, minHeight, maxHeight);
  }

  bool FileManagerSettings::saveStatusHeight(float height) {
    return m_config.setStateString(std::string(kNamespace), "status_height", std::to_string(height));
  }

  std::vector<SavedPlace> FileManagerSettings::loadSavedPlaces() const {
    std::vector<SavedPlace> result;
    if (const auto stored = m_config.stateStringArray(std::string(kNamespace), "saved_places")) {
      for (std::size_t i = 0; i + 2 < stored->size(); i += 3) {
        const auto& path = (*stored)[i + 1];
        if (!path.empty() && std::filesystem::path(path).is_absolute())
          result.push_back({(*stored)[i], path, (*stored)[i + 2]});
      }
    }
    return result;
  }

  bool FileManagerSettings::saveSavedPlaces(const std::vector<SavedPlace>& places) {
    std::vector<std::string> records;
    records.reserve(places.size() * 3);
    for (const auto& saved : places) {
      records.push_back(saved.id);
      records.push_back(saved.path.string());
      records.push_back(saved.label);
    }
    return m_config.setStateStringArray(std::string(kNamespace), "saved_places", records);
  }

  std::vector<std::string> FileManagerSettings::loadPlaceOrder() const {
    return m_config.stateStringArray(std::string(kNamespace), "place_order").value_or(std::vector<std::string>{});
  }

  bool FileManagerSettings::savePlaceOrder(const std::vector<std::string>& order) {
    return m_config.setStateStringArray(std::string(kNamespace), "place_order", order);
  }

  std::vector<std::string> FileManagerSettings::loadHiddenMounts() const {
    return m_config.stateStringArray(std::string(kNamespace), "hidden_mounts").value_or(std::vector<std::string>{});
  }

  bool FileManagerSettings::saveHiddenMounts(const std::vector<std::string>& identities) {
    return m_config.setStateStringArray(std::string(kNamespace), "hidden_mounts", identities);
  }

  bool FileManagerSettings::placesSectionExpanded(std::string_view key) const {
    return m_config.stateBool(std::string(kNamespace), std::string(key)).value_or(true);
  }

  bool FileManagerSettings::setPlacesSectionExpanded(std::string_view key, bool expanded) {
    return m_config.setStateBool(std::string(kNamespace), std::string(key), expanded);
  }

  std::string FileManagerSettings::dialogSizeStateKey(std::string_view title, bool conflictDialog) {
    if (conflictDialog)
      return "conflict_dialog_size";
    std::string key = "dialog_size_";
    bool separator = false;
    for (const unsigned char character : title) {
      if ((character >= 'A' && character <= 'Z')
          || (character >= 'a' && character <= 'z')
          || (character >= '0' && character <= '9')) {
        const auto normalized = character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        key.push_back(static_cast<char>(normalized));
        separator = false;
      } else if (!separator && key.back() != '_') {
        key.push_back('_');
        separator = true;
      }
    }
    while (!key.empty() && key.back() == '_')
      key.pop_back();
    return key;
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>>
  FileManagerSettings::loadDialogSize(std::string_view key) const {
    const auto saved = m_config.stateString(std::string(kNamespace), std::string(key));
    if (!saved)
      return std::nullopt;
    const auto separator = saved->find('x');
    if (separator == std::string::npos)
      return std::nullopt;
    std::uint32_t savedWidth = 0;
    std::uint32_t savedHeight = 0;
    const auto [widthEnd, widthError] = std::from_chars(saved->data(), saved->data() + separator, savedWidth);
    const auto [heightEnd, heightError] =
        std::from_chars(saved->data() + separator + 1, saved->data() + saved->size(), savedHeight);
    if (widthError == std::errc{}
        && widthEnd == saved->data() + separator
        && heightError == std::errc{}
        && heightEnd == saved->data() + saved->size()
        && savedWidth >= 360
        && savedHeight >= 180) {
      return std::pair{savedWidth, savedHeight};
    }
    return std::nullopt;
  }

  bool FileManagerSettings::saveDialogSize(std::string_view key, std::uint32_t width, std::uint32_t height) {
    const auto size = std::to_string(width) + "x" + std::to_string(height);
    return m_config.setStateString(std::string(kNamespace), std::string(key), size);
  }

} // namespace filemanager
