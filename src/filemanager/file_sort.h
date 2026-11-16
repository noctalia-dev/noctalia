#pragma once

#include "core/files/directory_scanner.h"

#include <cstring>
#include <gio/gio.h>
#include <string>

namespace filemanager {

  // Invalid native filename bytes remain sortable without passing malformed UTF-8
  // into GLib. Keep them in their own group so mixed input remains transitive.
  [[nodiscard]] inline std::string fileSortKey(const std::string& value) {
    if (!g_utf8_validate(value.c_str(), -1, nullptr))
      return std::string(1, '\1') + value;
    gchar* folded = g_utf8_casefold(value.c_str(), -1);
    gchar* key = g_utf8_collate_key_for_filename(folded, -1);
    const auto result = std::string(1, '\0') + key;
    g_free(folded);
    g_free(key);
    return result;
  }

  // A strict weak ordering for Files rows. Hidden and folder priorities are outer
  // groups, so changing sort direction never changes either preference.
  [[nodiscard]] inline bool fileEntryLess(
      const FileEntry& lhs, const FileEntry& rhs, FileDialogSortField field, FileDialogSortOrder order,
      bool foldersFirst, bool hiddenLast
  ) {
    const bool lhsHidden = !lhs.name.empty() && lhs.name.front() == '.';
    const bool rhsHidden = !rhs.name.empty() && rhs.name.front() == '.';
    if (hiddenLast && lhsHidden != rhsHidden)
      return !lhsHidden;
    if (foldersFirst && lhs.isDir != rhs.isDir)
      return lhs.isDir;

    int comparison = 0;
    bool lhsHasValue = true;
    bool rhsHasValue = true;
    switch (field) {
    case FileDialogSortField::Name: {
      comparison = fileSortKey(lhs.name).compare(fileSortKey(rhs.name));
      break;
    }
    case FileDialogSortField::Size:
      comparison = lhs.size < rhs.size ? -1 : (lhs.size > rhs.size ? 1 : 0);
      break;
    case FileDialogSortField::Modified:
      comparison = lhs.mtime < rhs.mtime ? -1 : (lhs.mtime > rhs.mtime ? 1 : 0);
      break;
    case FileDialogSortField::Created:
      lhsHasValue = lhs.hasCreated;
      rhsHasValue = rhs.hasCreated;
      comparison = lhs.created < rhs.created ? -1 : (lhs.created > rhs.created ? 1 : 0);
      break;
    case FileDialogSortField::Accessed:
      lhsHasValue = lhs.hasAccessed;
      rhsHasValue = rhs.hasAccessed;
      comparison = lhs.accessed < rhs.accessed ? -1 : (lhs.accessed > rhs.accessed ? 1 : 0);
      break;
    case FileDialogSortField::Type: {
      lhsHasValue = !lhs.type.empty();
      rhsHasValue = !rhs.type.empty();
      comparison = fileSortKey(lhs.type).compare(fileSortKey(rhs.type));
      break;
    }
    }
    if (lhsHasValue != rhsHasValue)
      return lhsHasValue; // Missing metadata is always last, in either direction.
    if (!lhsHasValue && !rhsHasValue)
      comparison = 0;
    if (comparison != 0)
      return order == FileDialogSortOrder::Ascending ? comparison < 0 : comparison > 0;

    // Stable deterministic tie break independent of selected sort direction.
    const int nameComparison = fileSortKey(lhs.name).compare(fileSortKey(rhs.name));
    if (nameComparison != 0)
      return nameComparison < 0;
    if (lhs.name != rhs.name)
      return lhs.name < rhs.name;
    return lhs.absPath.string() < rhs.absPath.string();
  }

} // namespace filemanager
