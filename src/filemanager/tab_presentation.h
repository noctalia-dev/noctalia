#pragma once

#include "core/files/directory_scanner.h"
#include "core/files/display_text.h"
#include "filemanager/file_breadcrumbs.h"
#include "ui/palette.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace filemanager {

  using TabId = std::uint64_t;
  inline constexpr TabId kInvalidTabId = 0;
  inline constexpr ColorRole kAdminWarningColorRole = ColorRole::Error;

  struct TabViewState {
    bool gridMode = false;
    bool showHidden = false;
    bool foldersFirst = true;
    bool hiddenLast = true;
    FileDialogSortField sortField = FileDialogSortField::Name;
    FileDialogSortOrder sortOrder = FileDialogSortOrder::Ascending;
    float viewScale = 1.0F;

    [[nodiscard]] bool operator==(const TabViewState&) const = default;
  };

  struct TabCapabilities {
    bool canCreate = true;
    bool canCreateBasic = true;
    bool canRename = true;
    bool canMutate = true;
    bool canTrash = true;
    bool canLaunchApps = true;
    bool canRunExternalTools = true;
    bool canGenerateThumbnails = true;
    bool canChecksum = true;
    bool canPinToPlaces = true;
    bool canUndo = true;
    bool canDragSource = true;
    bool canAcceptDrop = true;
    bool canOpenProperties = true;
    bool canCopyItems = true;

    [[nodiscard]] static constexpr TabCapabilities forLocal(bool writable) noexcept {
      return TabCapabilities{
          .canCreate = writable,
          .canCreateBasic = writable,
          .canRename = writable,
          .canMutate = writable,
          .canTrash = writable,
          .canLaunchApps = true,
          .canRunExternalTools = true,
          .canGenerateThumbnails = true,
          .canChecksum = true,
          .canPinToPlaces = true,
          .canUndo = true,
          .canDragSource = true,
          .canAcceptDrop = writable,
          .canOpenProperties = true,
          .canCopyItems = true,
      };
    }

    [[nodiscard]] static constexpr TabCapabilities forWastebin() noexcept {
      return TabCapabilities{
          .canCreate = false,
          .canCreateBasic = false,
          .canRename = false,
          .canMutate = true,
          .canTrash = false,
          .canLaunchApps = false,
          .canRunExternalTools = false,
          .canGenerateThumbnails = false,
          .canChecksum = false,
          .canPinToPlaces = true,
          .canUndo = false,
          .canDragSource = false,
          .canAcceptDrop = false,
          .canOpenProperties = false,
          .canCopyItems = false,
      };
    }

    [[nodiscard]] static constexpr TabCapabilities forAdministrator(bool writable) noexcept {
      return TabCapabilities{
          .canCreate = writable,
          .canCreateBasic = writable,
          .canRename = writable,
          .canMutate = writable,
          .canTrash = false,
          .canLaunchApps = false,
          .canRunExternalTools = false,
          .canGenerateThumbnails = false,
          .canChecksum = false,
          .canPinToPlaces = false,
          .canUndo = false,
          .canDragSource = false,
          .canAcceptDrop = false,
          .canOpenProperties = false,
          .canCopyItems = true,
      };
    }

    [[nodiscard]] static constexpr TabCapabilities forAdministratorReadOnly() noexcept {
      return forAdministrator(false);
    }
  };

  [[nodiscard]] inline bool isTrashTabPath(const std::filesystem::path& path) {
    return path == "trash:" || path.string().starts_with("trash:/");
  }

  [[nodiscard]] inline std::filesystem::path normalizeTabPath(const std::filesystem::path& path) {
    if (path.empty() || isTrashTabPath(path))
      return path == "trash:" ? std::filesystem::path("trash:/") : path.lexically_normal();
    auto normalized = path.lexically_normal();
    if (normalized.filename().empty() && normalized.has_parent_path() && normalized != normalized.root_path())
      normalized = normalized.parent_path();
    return normalized;
  }

  [[nodiscard]] inline std::string
  tabDisplayLabel(const std::filesystem::path& path, const std::filesystem::path& homePath) {
    if (isTrashTabPath(path))
      return "Wastebin";
    const auto normalized = normalizeTabPath(path);
    const auto normalizedHome = normalizeTabPath(homePath);
    if (!normalizedHome.empty() && normalized == normalizedHome)
      return "Home";
    const auto crumbs = fileBreadcrumbs(normalized);
    if (!crumbs.empty())
      return sanitizeFilenameForDisplay(crumbs.back().label);
    const auto raw = normalized.string();
    return raw.empty() ? "Nythera Files" : sanitizeFilenameForDisplay(raw);
  }

  [[nodiscard]] inline std::string windowTitleForPath(
      const std::filesystem::path& path, const std::filesystem::path& homePath, bool administrator = false
  ) {
    std::string title = tabDisplayLabel(path, homePath) + " \xE2\x80\x94 Nythera Files";
    if (administrator)
      title += " [Administrator]";
    return title;
  }

  [[nodiscard]] inline std::string tabTooltipForPath(const std::filesystem::path& path) {
    if (isTrashTabPath(path))
      return "Wastebin";
    return sanitizeFilenameForDisplay(normalizeTabPath(path).string());
  }

  [[nodiscard]] inline std::vector<std::string>
  tabIconNamesForPath(const std::filesystem::path& path, const std::filesystem::path& homePath) {
    if (isTrashTabPath(path))
      return {"user-trash", "user-trash-symbolic"};
    const auto normalized = normalizeTabPath(path);
    const auto normalizedHome = normalizeTabPath(homePath);
    if (!normalizedHome.empty() && normalized == normalizedHome)
      return {"user-home", "folder-home", "folder"};
    return {};
  }

} // namespace filemanager
