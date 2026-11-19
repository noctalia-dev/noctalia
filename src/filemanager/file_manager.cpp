#include "filemanager/file_manager.h"

#include "app/timer_poll_source.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/text_clipboard.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_creation.h"
#include "ui/palette.h"
#include "filemanager/file_transfer_payload.h"
#include "filemanager/property_operations.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/folder_size.h"
#include "filemanager/permanent_delete_prompt.h"
#include "i18n/i18n_service.h"
#include "render/core/renderer.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/box.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstring>
#include <future>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <tuple>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>

namespace {

  constexpr int kManualSymlink = 117;
  constexpr int kSelectionSymlink = 118;
  constexpr int kDuplicate = 119;
  constexpr int kSelectAll = 120;
  constexpr int kDeselectAll = 121;
  constexpr int kInvertSelection = 122;
  constexpr int kUndo = 123;
  constexpr int kRedo = 124;
  constexpr int kCompactEditMenu = 125;
  constexpr int kCompactViewMenu = 126;
  constexpr int kListView = 127;
  constexpr int kGridView = 128;
  constexpr int kShowColumns = 129;
  constexpr int kZoomIn = 130;
  constexpr int kZoomOut = 131;
  constexpr int kResetZoom = 132;
  constexpr int kFirstColumnAction = 133;
  constexpr int kShowMenuBar = 30008;

  using filemanager::FileColumn;
  using filemanager::columnIndex;
  constexpr std::array kColumnSortFields{
      FileDialogSortField::Name, FileDialogSortField::Size, FileDialogSortField::Modified, FileDialogSortField::Type,
      FileDialogSortField::Created, FileDialogSortField::Accessed
  };

  // SVG alpha masks use the existing Image tint and scale handling.
  constexpr char kDeleteWarning[] = R"SVG(data:image/svg+xml,
<svg xmlns="http://www.w3.org/2000/svg" width="72" height="64" viewBox="0 0 72 64">
  <path fill="none" stroke="white" stroke-width="3.6" stroke-linejoin="bevel"
        d="M34.3 5 H33 L13.3 41.4 M11.5 44.5 L4.9 56.6 7 59 H32
           M35.5 59 H38 M41.5 59 H65 L67.1 56.6 54.8 34
           M53.2 31 L51.8 28.4 M50.2 25.4 L48.8 22.8 M47.2 19.8 L39 5 H37.7"/>
  <path fill="white" d="M32 20 H40 L38 42 H34 Z M33 46 H39 V52 H33 Z"/>
</svg>)SVG";

} // namespace

class FileManager::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(FileManager& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.m_entries.size(); }
  std::optional<bool> selectedState(std::size_t index) const override {
    if (index >= m_owner.m_entries.size())
      return false;
    return m_owner.m_selectedPaths.contains(m_owner.m_entries[index].absPath.string());
  }
  std::unique_ptr<Node> createTile() override {
    if (m_owner.m_gridMode) {
      auto tile = std::make_unique<FileEntryTile>(
          1.0F, &m_owner.m_pickerThumbnails, filemanager::fileViewMetrics(m_owner.m_viewScale).gridIcon
      );
      tile->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
      return tile;
    }
    auto row = std::make_unique<FileEntryRow>(
        1.0F, filemanager::fileViewMetrics(m_owner.m_viewScale).listIcon, &m_owner.m_pickerThumbnails
    );
    row->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
    return row;
  }
  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (index >= m_owner.m_entries.size() || m_renderer == nullptr)
      return;
    if (m_owner.m_gridMode) {
      static_cast<FileEntryTile&>(tile).bind(
          *m_renderer, m_owner.m_entries[index], index, tile.width(), tile.height(), selected,
          hovered || m_owner.m_dropIndex == index, false
      );
    } else {
      auto& row = static_cast<FileEntryRow&>(tile);
      row.setColumns(m_owner.m_visibleFileColumns, m_owner.m_listColumns);
      row.bind(
          *m_renderer, m_owner.m_entries[index], index, tile.width(), selected, hovered || m_owner.m_dropIndex == index,
          false, m_owner.folderItemCount(m_owner.m_entries[index])
      );
    }
  }
  void onActivate(std::size_t index) override {
    if (index < m_owner.m_entries.size())
      m_owner.activatePointerItem(index);
  }
  std::string itemTooltip(std::size_t index) const override {
    if (index >= m_owner.m_entries.size())
      return {};
    const auto& entry = m_owner.m_entries[index];
    auto found = m_owner.m_trashOriginalPaths.find(entry.absPath.string());
    const auto description = queryFileTypeDescription(entry.absPath);
    const auto tooltip = entry.name + "\n" + description;
    return found == m_owner.m_trashOriginalPaths.end() ? tooltip : tooltip + "\nOriginal location: " + found->second;
  }
  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    m_owner.openContextMenu(index, anchorX, anchorY);
  }
  void onSecondaryEmptyActivate(float anchorX, float anchorY) override {
    m_owner.openEmptyContextMenu(anchorX, anchorY);
  }
  InputArea* beginInlineEdit(
      Node& tile, const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel
  ) override {
    if (m_owner.m_gridMode)
      return static_cast<FileEntryTile&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel));
    return static_cast<FileEntryRow&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel));
  }
  void endInlineEdit(Node& tile, const std::string& path) override {
    if (m_owner.m_gridMode)
      static_cast<FileEntryTile&>(tile).endInlineEdit(path);
    else
      static_cast<FileEntryRow&>(tile).endInlineEdit(path);
  }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }

private:
  FileManager& m_owner;
  Renderer* m_renderer = nullptr;
};

namespace {
  bool sameDirectoryPath(const std::filesystem::path& first, const std::filesystem::path& second) {
    if (first / "" == second / "")
      return true;
    std::error_code error;
    return std::filesystem::equivalent(first, second, error) && !error;
  }

  struct CapacityQuery {
    FileManager* manager;
    std::weak_ptr<int> alive;
    std::string identity;
    std::string requestKey;
    GCancellable* cancellable;
  };

  struct MonitoredFileQuery {
    FileManager* manager;
    std::uint64_t generation;
    std::filesystem::path path;
    std::filesystem::path replacedPath;
  };

  constexpr float kToolbarHeight = 32.0F;
  constexpr float kSidebarMinWidth = 150.0F;
  constexpr float kSidebarMaxWidth = 360.0F;
  constexpr float kStatusMinHeight = 30.0F;
  constexpr float kStatusMaxHeight = 76.0F;

  struct CompactSize {
    double value = 0.0;
    std::string unit;
    std::size_t unitIndex = 0;
  };

  CompactSize compactSize(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 7> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= 1024.0 && index + 1 < units.size()) {
      value /= 1024.0;
      ++index;
    }
    if (value >= 10.0)
      value = std::round(value);
    else
      value = std::round(value * 10.0) / 10.0;
    return {value, std::string(units[index]), index};
  }

  std::string compactCapacity(std::uint64_t free, std::uint64_t total) {
    const auto freeSize = compactSize(free), totalSize = compactSize(total);
    auto number = [](double value) {
      std::ostringstream out;
      out << std::fixed << std::setprecision(value == std::round(value) ? 0 : 1) << value;
      return out.str();
    };
    if (freeSize.unitIndex == totalSize.unitIndex)
      return number(freeSize.value) + " free of " + number(totalSize.value) + " " + totalSize.unit;
    return number(freeSize.value) + " " + freeSize.unit + " free / " + number(totalSize.value) + " " + totalSize.unit;
  }

  void applyNytheraBevel(Flex& panel, float radius) {
    panel.setRadius(radius);
    panel.setCornerShapes(
        CornerShapes{
            .tl = CornerShape::Chamfered,
            .tr = CornerShape::Chamfered,
            .br = CornerShape::Chamfered,
            .bl = CornerShape::Chamfered,
        }
    );
  }

  void useSubtleSelection(Button& button, bool muted = false) {
    auto palette = Button::defaultPalette(button.variant());
    palette.selected = Button::ButtonStateColors{
        .bg = colorSpecFromRole(ColorRole::PrimaryContainer, 0.42F),
        .border = clearColorSpec(),
        .label = colorSpecFromRole(ColorRole::OnPrimaryContainer),
    };
    if (muted) {
      for (auto* state : {&palette.normal, &palette.hover, &palette.pressed})
        state->label = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    }
    button.setCustomPalette(std::move(palette));
  }

  FileEntry makeFileEntry(GFileInfo* info, const std::filesystem::path& path) {
    FileEntry entry;
    entry.name = g_file_info_get_name(info);
    entry.absPath = path;
    entry.isDir = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY;
    entry.size = g_file_info_get_size(info);
    if (const auto* type = g_file_info_get_content_type(info))
      entry.type = type;
    entry.mtime = std::filesystem::file_time_type::clock::from_sys(
        std::chrono::system_clock::from_time_t(
            static_cast<std::time_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        )
    );
    entry.hasCreated = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CREATED);
    entry.created = entry.hasCreated ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED) : 0;
    entry.hasAccessed = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
    entry.accessed = entry.hasAccessed ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS) : 0;
    return entry;
  }

  bool sameEntry(const FileEntry& lhs, const FileEntry& rhs) {
    return lhs.name == rhs.name
        && lhs.absPath == rhs.absPath
        && lhs.isDir == rhs.isDir
        && lhs.size == rhs.size
        && lhs.mtime == rhs.mtime
        && lhs.created == rhs.created
        && lhs.accessed == rhs.accessed
        && lhs.hasCreated == rhs.hasCreated
        && lhs.hasAccessed == rhs.hasAccessed
        && lhs.type == rhs.type;
  }

  std::unique_ptr<Button> makeButton(std::string_view text, std::function<void()> action, Button** out = nullptr) {
    auto button = std::make_unique<Button>();
    button->setText(text);
    button->setVariant(ButtonVariant::Ghost);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick(std::move(action));
    if (out != nullptr)
      *out = button.get();
    return button;
  }

  std::string progressSummary(const std::vector<FileOperationManager::Operation>& operations) {
    std::ostringstream summary;
    std::size_t active = 0;
    for (const auto& operation : operations)
      active += operation.finished ? 0U : 1U;
    summary << active << (active == 1 ? " active operation" : " active operations");
    std::size_t shown = 0;
    for (const auto& operation : operations) {
      if (operation.finished)
        continue;
      if (shown == 4) {
        summary << "\n…and " << (active - shown) << " more active operations";
        break;
      }
      const auto name = std::filesystem::path(operation.source.empty() ? operation.destination : operation.source)
                            .filename()
                            .string();
      summary << "\n" << (name.empty() ? "Working" : name) << " — " << operation.status;
      if (operation.fraction > 0.0)
        summary << " (" << static_cast<int>(operation.fraction * 100.0) << "%)";
      ++shown;
    }
    return summary.str();
  }

  std::string applicationDisplayName(GAppInfo* app) {
    const char* name = app ? g_app_info_get_display_name(app) : nullptr;
    if (!name || !*name)
      name = app ? g_app_info_get_name(app) : nullptr;
    return name ? name : "Unknown application";
  }

  std::vector<std::string> applicationIconNames(GAppInfo* app) {
    std::vector<std::string> names;
    GIcon* icon = app ? g_app_info_get_icon(app) : nullptr;
    if (icon && G_IS_THEMED_ICON(icon)) {
      const char* const* themedNames = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (std::size_t i = 0; themedNames && themedNames[i]; ++i)
        names.emplace_back(themedNames[i]);
    }
    return names;
  }

  ContextMenuControlEntry applicationMenuEntry(std::int32_t id, GAppInfo* app) {
    ContextMenuControlEntry entry{.id = id, .label = applicationDisplayName(app)};
    entry.iconNames = applicationIconNames(app);
    return entry;
  }

  bool directorySupportsCreation(const std::filesystem::path& path) {
    GFile* file = g_file_new_for_path(path.c_str());
    GFileInfo* info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr
    );
    const bool writable = info && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
    if (info)
      g_object_unref(info);
    g_object_unref(file);
    return writable;
  }

  ContextMenuControlEntry createNewMenuEntry(std::optional<bool> symlinkDirectory) {
    std::vector<ContextMenuControlEntry> children{
        {.id = 111, .label = "Folder…", .actionGlyph = "folder-plus"},
        {.id = 112, .label = "Empty File…", .actionGlyph = "file-plus"},
        {.id = 113, .label = "Text File…", .actionGlyph = "file-text"}
    };
    struct OfficeType {
      std::int32_t id;
      const char* label;
      const char* mime;
      const char* asset;
    };
    constexpr std::array officeTypes{
        OfficeType{114, "Document…", "application/vnd.oasis.opendocument.text", "templates/filemanager/office/blank.odt"},
        OfficeType{115, "Spreadsheet…", "application/vnd.oasis.opendocument.spreadsheet", "templates/filemanager/office/blank.ods"},
        OfficeType{116, "Presentation…", "application/vnd.oasis.opendocument.presentation", "templates/filemanager/office/blank.odp"}
    };
    std::vector<ContextMenuControlEntry> officeEntries;
    for (const auto& type : officeTypes) {
      std::error_code assetError;
      if (!std::filesystem::is_regular_file(paths::assetPath(type.asset), assetError) || assetError)
        continue;
      GList* compatible = g_app_info_get_recommended_for_type(type.mime);
      GAppInfo* selected = nullptr;
      for (GList* item = compatible; item; item = item->next) {
        auto* app = G_APP_INFO(item->data);
        if (!applicationIconNames(app).empty()) {
          selected = app;
          break;
        }
      }
      if (selected) {
        ContextMenuControlEntry entry{.id = type.id, .label = type.label};
        entry.gutterIconNames = applicationIconNames(selected);
        officeEntries.push_back(std::move(entry));
      }
      for (GList* item = compatible; item; item = item->next)
        g_object_unref(item->data);
      g_list_free(compatible);
    }
    if (!officeEntries.empty()) {
      children.push_back({.separator = true});
      children.insert(children.end(), std::make_move_iterator(officeEntries.begin()),
                      std::make_move_iterator(officeEntries.end()));
    }
    children.push_back({.separator = true});
    if (symlinkDirectory) {
      children.push_back(
          {.id = kSelectionSymlink, .label = *symlinkDirectory ? "Symlink to Folder" : "Symlink to File",
           .actionGlyph = "link"}
      );
    }
    children.push_back({.id = kManualSymlink, .label = "Symlink…", .actionGlyph = "link"});
    ContextMenuControlEntry createNew{
        .id = 110,
        .label = "Create New",
        .hasSubmenu = true,
        .submenuEntries = std::move(children),
        .actionGlyph = "plus",
        .attachSubmenuContent = true,
    };
    return createNew;
  }

  bool launchApplicationForFile(GAppInfo* app, const std::filesystem::path& path, GError** error) {
    if (!app || path.empty()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No application or file was selected.");
      return false;
    }
    GFile* file = g_file_new_for_path(path.c_str());
    GList files{file, nullptr, nullptr};
    char* uri = g_file_get_uri(file);
    GList uris{uri, nullptr, nullptr};
    const gboolean launched = g_app_info_supports_files(app)
        ? g_app_info_launch(app, &files, nullptr, error)
        : g_app_info_launch_uris(app, &uris, nullptr, error);
    g_free(uri);
    g_object_unref(file);
    return launched == TRUE;
  }
} // namespace

FileManager::FileManager(std::string initialPath)
    : m_operations([this](const FileOperationManager::Operation& operation) {
        if (operation.finished) {
          setStatus(operation.status);
          if (operation.succeeded && operation.kind == FileOperationManager::Kind::Rename) {
            if (m_selectedPaths.erase(operation.source))
              m_selectedPaths.insert(operation.destination);
            if (m_selectedPath && *m_selectedPath == operation.source)
              m_selectedPath = operation.destination;
            if (m_selectionAnchor && *m_selectionAnchor == operation.source)
              m_selectionAnchor = operation.destination;
          }
          if (m_inlineRenameOperation == operation.id) {
            m_inlineRenameOperation.reset();
            if (operation.succeeded && m_renamePath == std::filesystem::path(operation.source)) {
              // The completion below refreshes once, after selection follows the new path.
              m_pendingMonitorChanges.clear();
              m_refreshAfterRename = false;
              cancelRename();
            }
          }
          if (operation.succeeded
              && (operation.kind == FileOperationManager::Kind::CreateFile
                  || operation.kind == FileOperationManager::Kind::CreateFolder)
              && std::filesystem::path(operation.destination).parent_path() == m_path) {
            m_selectedPath = operation.destination;
            m_selectedPaths.clear();
            m_selectedPaths.insert(operation.destination);
            m_pendingInlineRenamePath = operation.destination;
          }
          if (auto drop = m_dropFinishes.find(operation.id); drop != m_dropFinishes.end()) {
            auto finish = std::move(drop->second);
            m_dropFinishes.erase(drop);
            finish(operation.succeeded);
          }
          const bool pastedEntries = recordPasteCompletion(operation);
          const bool wastebinOperation = operation.kind == FileOperationManager::Kind::Restore
              || operation.kind == FileOperationManager::Kind::EmptyWastebin;
          if ((operation.succeeded || wastebinOperation || pastedEntries) && m_windowOpen) {
            if (wastebinOperation || operation.undoUnavailable
                || (operation.kind == FileOperationManager::Kind::Duplicate && !operation.succeeded))
              m_operationResultAfterRefresh = operation.status;
            enumerate(m_path);
          }
          if (auto cut = m_cutOperations.find(operation.id); cut != m_cutOperations.end()) {
            if (operation.succeeded && cut->second == m_transfer.selectionGeneration()) {
              m_transfer.consumeSelection(cut->second);
              m_consumedCutSelection = cut->second;
            }
            m_cutOperations.erase(cut);
          }
          if (operation.succeeded)
            refreshPlaceCapacities();
          if (!m_operations.active() && !m_windowOpen && m_pendingClipboardReads == 0)
            m_running = false;
          if (!m_operations.active() && m_quitting)
            m_running = false;
        } else {
          setStatus(operation.status);
        }
        syncProgressWindow();
      }),
      m_adapter(std::make_unique<Adapter>(*this)),
      m_path(initialPath.empty() ? std::filesystem::current_path() : std::filesystem::path(initialPath)) {
  m_gridMode = m_configService.stateBool("files", "grid_view").value_or(false);
  m_showApplicationMenuBar = m_configService.stateBool("files", "show_menu_bar").value_or(true);
  if (const auto widths = m_configService.stateString("files", "list_column_widths"))
    filemanager::parseFileColumns(*widths, m_preferredListColumns);
  if (const auto columns = m_configService.stateString("files", "list_column_layout"))
    filemanager::parseFileColumnLayout(*columns, m_fileColumnLayout);
  m_listColumns = m_preferredListColumns;
  m_showHidden = m_configService.stateBool("files", "show_hidden").value_or(false);
  m_foldersFirst = m_configService.stateBool("files", "folders_first").value_or(true);
  m_hiddenLast = m_configService.stateBool("files", "hidden_last").value_or(true);
  if (auto stored = m_configService.stateString("files", "sort_field")) {
    int value = 0;
    const auto parsed = std::from_chars(stored->data(), stored->data() + stored->size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == stored->data() + stored->size() && value >= 0 && value <= 5)
      m_sortField = static_cast<FileDialogSortField>(value);
  }
  m_sortOrder = m_configService.stateBool("files", "sort_descending").value_or(false) ? FileDialogSortOrder::Descending
                                                                                      : FileDialogSortOrder::Ascending;
  m_operations.setConflictResolver([this](
                                       const std::string& source, const std::string& destination, bool hasRemaining,
                                       GCancellable* cancellable
                                   ) {
    std::unique_lock lock(m_conflictMutex);
    auto promise = std::make_shared<std::promise<int>>();
    auto answer = promise->get_future();
    auto describe = [cancellable](const std::string& path) {
      GFile* file = path.starts_with("trash:") ? g_file_new_for_uri(path.c_str()) : g_file_new_for_path(path.c_str());
      GError* error = nullptr;
      GFileInfo* info = g_file_query_info(
          file, "standard::type,standard::size,standard::content-type,time::modified",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
      );
      std::string details;
      if (info != nullptr) {
        const bool directory = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY;
        const char* contentType = g_file_info_get_content_type(info);
        gchar* typeName = contentType == nullptr ? nullptr : g_content_type_get_description(contentType);
        details = directory ? "Folder" : typeName == nullptr ? "File" : typeName;
        g_free(typeName);
        if (!directory && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
          gchar* size = g_format_size(g_file_info_get_size(info));
          details += " · ";
          details += size;
          g_free(size);
        }
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
          const auto modified = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          GDateTime* date = g_date_time_new_from_unix_local(static_cast<gint64>(modified));
          if (date != nullptr) {
            gchar* formatted = g_date_time_format(date, "%e %b %Y, %H:%M");
            details += " · Modified ";
            details += formatted;
            g_free(formatted);
            g_date_time_unref(date);
          }
        }
        g_object_unref(info);
      }
      g_clear_error(&error);
      g_object_unref(file);
      return details;
    };
    const auto sourceName = std::filesystem::path(source).filename().string();
    const auto destName = std::filesystem::path(destination).filename().string();
    GFile* sourceFile =
        source.starts_with("trash:") ? g_file_new_for_uri(source.c_str()) : g_file_new_for_path(source.c_str());
    GFile* destinationFile = g_file_new_for_path(destination.c_str());
    const auto sourceType = g_file_query_file_type(sourceFile, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
    const auto destinationType =
        g_file_query_file_type(destinationFile, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
    GFile* sameSource = G_FILE(g_object_ref(sourceFile));
    GFile* sameDestination = g_file_new_for_path(destination.c_str());
    const bool sameItemConflict = g_file_equal(sameSource, sameDestination);
    g_object_unref(sameSource);
    g_object_unref(sameDestination);
    const bool differentTypes =
        sourceType == G_FILE_TYPE_UNKNOWN || destinationType == G_FILE_TYPE_UNKNOWN || sourceType != destinationType;
    const bool directoryConflict = sourceType == G_FILE_TYPE_DIRECTORY && destinationType == G_FILE_TYPE_DIRECTORY;
    std::string comparisonSummary;
    if (differentTypes)
      comparisonSummary = "The item types differ";
    else if (sourceType != G_FILE_TYPE_DIRECTORY) {
      GFileInfo* sourceInfo = g_file_query_info(
          sourceFile, "standard::size,time::modified", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      );
      GFileInfo* destinationInfo = g_file_query_info(
          destinationFile, "standard::size,time::modified", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      );
      if (sourceInfo != nullptr && destinationInfo != nullptr) {
        const auto sourceSize = g_file_info_get_size(sourceInfo);
        const auto destinationSize = g_file_info_get_size(destinationInfo);
        if (sourceSize != destinationSize)
          comparisonSummary = sourceSize > destinationSize ? "Incoming file is larger" : "Incoming file is smaller";
        if (g_file_info_has_attribute(sourceInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED)
            && g_file_info_has_attribute(destinationInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
          const auto sourceModified = g_file_info_get_attribute_uint64(sourceInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          const auto destinationModified =
              g_file_info_get_attribute_uint64(destinationInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          if (sourceModified != destinationModified) {
            if (!comparisonSummary.empty())
              comparisonSummary += " · ";
            comparisonSummary +=
                sourceModified > destinationModified ? "Incoming item is newer" : "Incoming item is older";
          }
        }
      }
      if (sourceInfo != nullptr)
        g_object_unref(sourceInfo);
      if (destinationInfo != nullptr)
        g_object_unref(destinationInfo);
    }
    g_object_unref(sourceFile);
    g_object_unref(destinationFile);
    const std::string message = sourceName
        + "\n"
        + describe(source)
        + "\n"
        + source
        + "\n\n"
        + destName
        + "\n"
        + describe(destination)
        + "\n"
        + destination;
    g_idle_add(
        [](gpointer data) -> gboolean {
          auto* request = static_cast<
              std::tuple<FileManager*, std::shared_ptr<std::promise<int>>, std::string, bool, std::string>*>(data);
          auto* self = std::get<0>(*request);
          self->m_dialogComparisonSummary = std::get<4>(*request);
          if (!self->m_windowOpen) {
            self->m_backgroundConflictPending = true;
            self->syncProgressWindow();
          }
          const auto& detail = std::get<2>(*request);
          const bool selfConflict = detail.starts_with("SELF-CONFLICT\n");
          const bool incompatible = detail.starts_with("TYPE-CONFLICT\n");
          const bool restoreFolders = detail.starts_with("RESTORE-DIRECTORY-CONFLICT\n");
          const bool folders = restoreFolders || detail.starts_with("DIRECTORY-CONFLICT\n");
          const auto body = detail.substr(detail.find('\n') + 1);
          self->showDialog(
              selfConflict       ? "Create a duplicate?"
                  : incompatible ? "Items have different types"
                  : folders      ? "Folder already exists"
                                 : "File already exists",
              body,
              selfConflict ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Cancel", 4}}
                  : restoreFolders
                  ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                  : folders
                  ? std::vector<std::pair<std::string, int>>{{"Merge", 5}, {"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                  : incompatible
                  ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                  : std::vector<
                        std::pair<std::string, int>>{{"Replace", 1}, {"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}},
              [self, promise = std::get<1>(*request)](int choice) {
                try {
                  promise->set_value(choice);
                } catch (...) {
                }
                self->m_backgroundConflictPending = false;
                self->syncProgressWindow();
              },
              self->m_windowOpen ? self->m_surface.get() : self->m_progressSurface.get(), std::get<3>(*request), true
          );
          delete request;
          return G_SOURCE_REMOVE;
        },
        new std::tuple<FileManager*, std::shared_ptr<std::promise<int>>, std::string, bool, std::string>{
            this, promise,
            (sameItemConflict                                        ? "SELF-CONFLICT\n"
                 : differentTypes                                    ? "TYPE-CONFLICT\n"
                 : directoryConflict && source.starts_with("trash:") ? "RESTORE-DIRECTORY-CONFLICT\n"
                 : directoryConflict                                 ? "DIRECTORY-CONFLICT\n"
                                                                     : "FILE-CONFLICT\n")
                + message,
            hasRemaining, comparisonSummary
        }
    );
    while (answer.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
      if (g_cancellable_is_cancelled(cancellable)) {
        g_idle_add(
            [](gpointer data) -> gboolean {
              static_cast<FileManager*>(data)->closeDialog(-1);
              return G_SOURCE_REMOVE;
            },
            this
        );
        return FileOperationManager::ConflictDecision{};
      }
    }
    const int choice = answer.get();
    using Action = FileOperationManager::ConflictAction;
    switch (choice >= 10 ? choice - 10 : choice) {
    case 1:
      return FileOperationManager::ConflictDecision{Action::Replace, choice >= 11, {}};
    case 2:
      return FileOperationManager::ConflictDecision{Action::Rename, choice >= 12, m_dialogAlternativeName};
    case 3:
      return FileOperationManager::ConflictDecision{Action::Skip, choice >= 13, {}};
    case 5:
      return FileOperationManager::ConflictDecision{Action::Merge, choice >= 15, {}};
    default:
      return FileOperationManager::ConflictDecision{Action::Cancel, false};
    }
  });
}

std::vector<std::string> FileManager::selectedPaths() const {
  std::vector<std::string> paths;
  paths.reserve(m_selectedPaths.size());
  for (const auto& entry : m_entries)
    if (m_selectedPaths.contains(entry.absPath.string()))
      paths.push_back(entry.absPath.string());
  return paths;
}

bool FileManager::canChangeSelection() const {
  return m_grid && !m_loading && !m_renamePath && !m_dragPending && !m_draggingFiles && !m_transfer.dragging()
      && !m_reorderId && !m_resizingFileColumn && !m_fileColumnDrag && !m_resizingSidebar && !m_resizingStatus
      && !m_grid->rubberBandActive();
}

void FileManager::changeSelection(SelectionChange change) {
  if (!canChangeSelection())
    return;
  std::unordered_set<std::string> paths;
  std::optional<std::filesystem::path> first;
  if (change != SelectionChange::None) {
    paths.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
      const auto path = entry.absPath.string();
      if (change == SelectionChange::Invert && m_selectedPaths.contains(path))
        continue;
      paths.insert(path);
      if (!first)
        first = entry.absPath;
    }
  }
  if (!m_selectedPath || !paths.contains(m_selectedPath->string()))
    m_selectedPath = std::move(first);
  m_selectedPaths = std::move(paths);
  m_selectionAnchor = m_selectedPath;
  m_chooseFirstAfterLoad = false;
  restoreSelection();
  m_grid->notifyDataChanged();
  updateSelectionSummary();
  if (m_surface)
    m_surface->requestRedraw();
}

std::optional<std::pair<std::filesystem::path, bool>> FileManager::symlinkSelection() const {
  if (m_browsingTrash || m_loading || m_selectedPaths.size() != 1)
    return std::nullopt;
  const auto selected = std::ranges::find_if(m_entries, [this](const auto& entry) {
    return m_selectedPaths.contains(entry.absPath.string());
  });
  if (selected == m_entries.end())
    return std::nullopt;
  std::error_code error;
  const auto type = std::filesystem::symlink_status(selected->absPath, error).type();
  if (error || (type != std::filesystem::file_type::regular && type != std::filesystem::file_type::directory))
    return std::nullopt;
  return std::pair{selected->absPath, type == std::filesystem::file_type::directory};
}

ContextMenuControlEntry FileManager::newItemMenu() const {
  const auto selection = symlinkSelection();
  return createNewMenuEntry(selection ? std::optional(selection->second) : std::nullopt);
}

void FileManager::duplicateSelection() {
  if (m_browsingTrash || m_loading || !directorySupportsCreation(m_path))
    return;
  auto paths = selectedPaths();
  if (paths.empty())
    return;
  if (const auto id = m_operations.duplicate(std::move(paths), m_path.string()))
    m_pasteRequests[id] = {m_path, m_navigationGeneration};
}

void FileManager::createSelectionSymlink() {
  if (m_browsingTrash || !directorySupportsCreation(m_path))
    return;
  const auto selection = symlinkSelection();
  if (!selection) {
    setStatus("Select one regular file or folder to create a symlink");
    return;
  }
  const auto& [path, directory] = *selection;
  auto name = path.filename().string();
  // Treat compound filename extensions as a unit and retain a leading dot.
  const auto extension = directory ? std::string::npos : name.find('.', 1);
  name.insert(extension == std::string::npos ? name.size() : extension, "-link");
  if (const auto id = m_operations.createSymlink(path.string(), (m_path / name).string(), true))
    m_pasteRequests[id] = {m_path, m_navigationGeneration};
}

void FileManager::showSymlinkDialog() {
  if (m_dialogSurface || m_browsingTrash || !directorySupportsCreation(m_path))
    return;
  m_symlinkDialogDestination = PasteRequest{m_path, m_navigationGeneration};
  showDialog(
      "Create Symlink", "Enter a name and target path. Relative targets are resolved from this folder.",
      {{"Cancel", 0}, {"Create Symlink", 1}},
      [this](int) {
        m_symlinkDialogDestination.reset();
        m_symlinkDialogName = nullptr;
        m_symlinkDialogTarget = nullptr;
        m_symlinkDialogError = nullptr;
      }
  );
}

bool FileManager::submitSymlinkDialog() {
  const auto fail = [this](std::string message, Input* field = nullptr) {
    m_symlinkDialogError->setText(message);
    if (field) {
      field->setInvalid(true);
      m_dialogDispatcher.setFocus(field->inputArea());
    }
    for (auto* parent = m_symlinkDialogError->parent(); parent; parent = parent->parent()) {
      if (auto* scroll = dynamic_cast<ScrollView*>(parent)) {
        scroll->requestScrollIntoView(m_symlinkDialogError);
        break;
      }
    }
    m_dialogSurface->requestLayout();
    return false;
  };
  if (!m_deviceOperations.empty() || m_operations.replaying())
    return fail("Wait for the current operation to finish.");
  const auto& request = *m_symlinkDialogDestination;
  if (request.navigation != m_navigationGeneration || m_browsingTrash)
    return fail("The displayed folder changed. Reopen the dialog in the destination folder.");
  const auto& name = m_symlinkDialogName->value();
  const auto& target = m_symlinkDialogTarget->value();
  if (name.empty())
    return fail("Enter a symlink name.", m_symlinkDialogName);
  if (!filemanager::validFileName(name))
    return fail("Enter a single valid filename.", m_symlinkDialogName);
  if (target.empty() || target.find('\0') != std::string::npos)
    return fail("Enter a target path.", m_symlinkDialogTarget);
  const auto destination = request.destination / name;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(destination, error);
  if (std::filesystem::exists(status))
    return fail("An item with that name already exists.", m_symlinkDialogName);
  if (error && error != std::errc::no_such_file_or_directory)
    return fail(error.message(), m_symlinkDialogName);
  if (!directorySupportsCreation(request.destination))
    return fail("The destination folder does not support file creation.");
  const auto id = m_operations.createSymlink(filemanager::expandHomePath(target, g_get_home_dir()), destination.string());
  if (id == 0)
    return fail("The symlink operation could not be started.");
  m_pasteRequests[id] = request;
  return true;
}

void FileManager::pasteClipboard() {
  if (m_browsingTrash || !m_deviceOperations.empty() || m_operations.replaying()) {
    setStatus("This action is currently unavailable");
    return;
  }
  const auto destination = m_path.string();
  const auto navigation = m_navigationGeneration;
  const auto generation = m_transfer.selectionGeneration();
  if (m_consumedCutSelection == generation) {
    setStatus("These cut files have already been moved");
    return;
  }
  ++m_pendingClipboardReads;
  m_transfer.readSelection([this, destination, navigation, generation](std::optional<NativeDataTransfer::Payload> payload) {
    --m_pendingClipboardReads;
    if (!m_quitting && m_windowOpen && payload && m_deviceOperations.empty() && !m_operations.replaying()) {
      try {
        filemanager::FileTransferPayload transfer;
        if (payload->contains("text/uri-list")) {
          bool cut = payload->contains("application/x-kde-cutselection")
              && payload->at("application/x-kde-cutselection") == "1";
          if (!payload->contains("application/x-kde-cutselection") && payload->contains("x-special/gnome-copied-files"))
            cut = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true).cut;
          transfer = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false, cut);
        } else {
          transfer = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true);
        }
        if (transfer.paths.empty())
          setStatus("Clipboard contains no local files");
        else if (transfer.cut) {
          if (std::ranges::any_of(m_cutOperations, [generation](const auto& item) {
                return item.second == generation;
              })) {
            setStatus("These cut files are already being moved");
            return;
          }
          const auto id = m_operations.move(std::move(transfer.paths), destination);
          m_cutOperations[id] = generation;
          m_pasteRequests[id] = {destination, navigation};
        } else {
          const auto id = m_operations.copy(std::move(transfer.paths), destination);
          m_pasteRequests[id] = {destination, navigation};
        }
      } catch (const std::exception& error) {
        setStatus(std::string("Cannot paste files: ") + error.what());
      }
    } else if (!m_quitting && m_windowOpen)
      setStatus("File clipboard unavailable or changed during paste");
    if ((!m_windowOpen || m_quitting) && !m_operations.active() && m_pendingClipboardReads == 0)
      m_running = false;
  });
}

bool FileManager::recordPasteCompletion(const FileOperationManager::Operation& operation) {
  const auto found = m_pasteRequests.find(operation.id);
  if (found == m_pasteRequests.end())
    return false;
  const auto request = found->second;
  m_pasteRequests.erase(found);
  if (!m_windowOpen
      || request.navigation != m_navigationGeneration
      || !sameDirectoryPath(request.destination, m_path)
      || operation.completedEntries.empty())
    return false;
  if (!m_pendingPasteSelection
      || m_pendingPasteSelection->request.navigation != request.navigation
      || !sameDirectoryPath(m_pendingPasteSelection->request.destination, request.destination))
    m_pendingPasteSelection = PasteSelection{request, {}};
  auto& entries = m_pendingPasteSelection->entries;
  entries.insert(entries.end(), operation.completedEntries.begin(), operation.completedEntries.end());
  return true;
}

void FileManager::applyPendingPasteSelection() {
  if (!m_pendingPasteSelection || m_loading)
    return;
  auto pending = std::move(*m_pendingPasteSelection);
  m_pendingPasteSelection.reset();
  if (!m_windowOpen || pending.request.navigation != m_navigationGeneration
      || !sameDirectoryPath(pending.request.destination, m_path))
    return;
  std::unordered_set<std::string> paths;
  std::optional<std::size_t> first;
  for (std::size_t index = 0; index < m_entries.size(); ++index) {
    const auto& path = m_entries[index].absPath;
    // Selection stays in the displayed namespace, even when a drop resolves
    // a symlinked directory to its physical transfer destination.
    if (path.parent_path() / "" != m_path / "")
      continue;
    struct stat state{};
    if (lstat(path.c_str(), &state) != 0)
      continue;
    if (std::ranges::none_of(pending.entries, [&](const auto& entry) {
          if (entry.device != static_cast<std::uint64_t>(state.st_dev)
              || entry.inode != static_cast<std::uint64_t>(state.st_ino))
            return false;
          const std::filesystem::path reported(entry.path);
          return reported == path
              || (reported.filename() == path.filename()
                  && sameDirectoryPath(reported.parent_path(), path.parent_path()));
        }))
      continue;
    paths.insert(path.string());
    if (!first)
      first = index;
  }
  if (!first)
    return;
  m_selectedPaths = std::move(paths);
  m_selectedPath = m_entries[*first].absPath;
  m_pendingDropScrollGeneration = pending.request.dropScrollGeneration;
  const bool reveal = !m_pendingDropScrollGeneration
      || (m_grid && *m_pendingDropScrollGeneration == m_grid->scrollView().userScrollGeneration());
  m_pendingPasteRevealPath = reveal ? m_selectedPath : std::nullopt;
  m_selectionAnchor = m_selectedPath;
  m_chooseFirstAfterLoad = false;
  restoreSelection();
  if (m_grid) {
    m_grid->notifyDataChanged();
    if (reveal && !m_pendingDropScrollGeneration)
      m_grid->scrollToIndex(*first);
  }
  updateSelectionSummary();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::finishPendingTransferReveal(Renderer& renderer) {
  if (m_pendingDropScrollGeneration) {
    if (!m_grid || *m_pendingDropScrollGeneration != m_grid->scrollView().userScrollGeneration()) {
      m_pendingPasteRevealPath.reset();
      m_pendingDropScrollGeneration.reset();
      return;
    }
    // Keep the path through layouts of an in-flight refresh. Resolve its index
    // only after enumeration/sorting, with the final viewport geometry available.
    if (m_loading || m_pendingMonitorQueries != 0 || !m_pendingMonitorChanges.empty())
      return;
    if (m_pendingPasteRevealPath && m_selectedPath == m_pendingPasteRevealPath) {
      const auto entry = std::ranges::find(m_entries, *m_pendingPasteRevealPath, &FileEntry::absPath);
      if (entry != m_entries.end()) {
        m_grid->scrollToIndex(static_cast<std::size_t>(entry - m_entries.begin()));
        m_grid->layout(renderer);
      }
    }
    m_pendingDropScrollGeneration.reset();
  }
  m_pendingPasteRevealPath.reset();
}

void FileManager::copyToDesktopClipboard(bool cut) {
  if (m_browsingTrash) {
    setStatus("This action is unavailable in Wastebin");
    return;
  }
  const auto paths = selectedPaths();
  if (paths.empty())
    return;
  try {
    if (!m_transfer.setSelection(filemanager::encodeFileTransfer(paths, cut), m_wayland.lastInputSerial())) {
      setStatus("Could not publish the file clipboard");
      return;
    }
  } catch (const std::exception& error) {
    setStatus(std::string("Cannot copy files: ") + error.what());
  }
}

void FileManager::writeDesktopClipboard(std::string payload, const char* mime) {
  if (!m_transfer.setSelection({{mime, std::move(payload)}}, m_wayland.lastInputSerial()))
    setStatus("Could not publish the clipboard");
}
void FileManager::beginRename() {
  const auto paths = selectedPaths();
  if (!paths.empty())
    beginRenamePath(paths.front());
}

void FileManager::beginRenamePath(const std::filesystem::path& path) {
  if (m_browsingTrash) {
    setStatus("This action is unavailable in Wastebin");
    return;
  }
  if (!m_grid || !m_surface || m_inlineRenameOperation)
    return;
  if (path.parent_path() != m_path) {
    m_pendingInlineRenamePath.reset();
    return;
  }
  const auto found = std::ranges::find(m_entries, path, &FileEntry::absPath);
  if (found == m_entries.end()) {
    if (m_loading)
      m_pendingInlineRenamePath = path;
    else
      setStatus("The item is no longer available");
    return;
  }
  const auto index = static_cast<std::size_t>(found - m_entries.begin());
  m_grid->setSelectedIndex(index, false);
  m_grid->scrollToIndex(index);
  m_renamePath = path;
  const auto stablePath = path.string();
  InputArea* editor = m_grid->beginInlineEdit(
      index, stablePath, path.filename().string(),
      [this, path](const std::string& newName) {
        if (!filemanager::validFileName(newName)) {
          setStatus("Choose a valid filename");
          return;
        }
        if (m_inlineRenameOperation)
          return;
        const auto destination = path.parent_path() / newName;
        if (destination == path) {
          cancelRename();
          return;
        }
        struct stat state{};
        if (lstat(path.c_str(), &state) != 0) {
          setStatus("Could not inspect the item: " + std::string(g_strerror(errno)));
          return;
        }
        // Keep the editor and the proposed name until success. Checked rename
        // rejects collisions without offering to overwrite another item.
        m_inlineRenameOperation = m_operations.renameChecked(
            path.string(), destination.string(), state.st_dev, state.st_ino, {}
        );
      },
      [this] { cancelRename(); }
  );
  if (editor != nullptr) {
    m_inlineEditArea = editor;
    m_dispatcher.setFocus(editor);
    m_surface->requestLayout();
  } else {
    m_pendingInlineRenamePath = path;
    m_surface->requestLayout();
  }
}

void FileManager::cancelRename() {
  if (m_renamePath && m_grid)
    m_grid->endInlineEdit(m_renamePath->string());
  m_renamePath.reset();
  m_inlineEditArea = nullptr;
  if (m_grid)
    m_dispatcher.setFocus(m_grid->focusArea());
  if (m_surface)
    m_surface->requestLayout();
  if (!m_loading && !m_pendingMonitorChanges.empty())
    applyPendingMonitorChanges();
  if (m_refreshAfterRename) {
    m_refreshAfterRename = false;
    enumerate(m_path);
  }
}

void FileManager::openContextMenu(std::size_t index, float x, float y) {
  if (index >= m_entries.size() || !m_contextMenuPopup || !m_surface)
    return;
  m_contextMenuTarget = m_entries[index].absPath;
  m_openWithApplications.clear();
  const std::string path = m_entries[index].absPath.string();
  if (!m_selectedPaths.contains(path)) {
    m_keyboardModifiers = 0;
    m_selectedPaths.clear();
    m_selectedPaths.insert(path);
    m_selectedPath = m_entries[index].absPath;
    m_grid->setSelectedIndex(index, false);
    m_grid->notifyDataChanged();
  }
  std::vector<ContextMenuControlEntry> entries{
      {.id = 10,
       .label = "Open",
       .actionIconNames = {"folder-open-symbolic", "folder-open"},
       .actionGlyph = "folder-open"},
      {.id = 1,
       .label = "Copy",
       .shortcut = "Ctrl+C",
       .actionIconNames = {"edit-copy-symbolic", "edit-copy"},
       .actionGlyph = "copy"},
      {.id = 2,
       .label = "Cut",
       .shortcut = "Ctrl+X",
       .actionIconNames = {"edit-cut-symbolic", "edit-cut"},
       .actionGlyph = "scissors"},
      {.id = 3,
       .label = "Paste",
       .shortcut = "Ctrl+V",
       .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
       .actionGlyph = "U+EA6F"},
      {.id = 4,
       .label = "Rename",
       .shortcut = "F2",
       .actionIconNames = {"document-edit-symbolic", "document-edit"},
       .actionGlyph = "pencil"},
      {.id = 5,
       .label = "Move to Trash",
       .shortcut = "Delete",
       .actionIconNames = {"user-trash-symbolic", "user-trash"},
       .actionGlyph = "trash"},
      {.id = 6, .label = "Delete permanently", .shortcut = "Shift+Delete"}
  };
  if (m_browsingTrash)
    entries = {
        {.id = 10,
         .label = "Open",
         .actionIconNames = {"folder-open-symbolic", "folder-open"},
         .actionGlyph = "folder-open"},
        {.id = 70, .label = "Restore"},
        {.id = 71, .label = "Restore to…"},
    };
  else {
    if (!m_entries[index].isDir) {
      std::vector<ContextMenuControlEntry> alternatives;
      GFile* file = g_file_new_for_path(m_entries[index].absPath.c_str());
      GFileInfo* info =
          g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
      const char* mime = info ? g_file_info_get_content_type(info) : nullptr;
      if (mime && *mime) {
        std::shared_ptr<GAppInfo> defaultApp(g_app_info_get_default_for_type(mime, FALSE), g_object_unref);
        if (defaultApp) {
          entries.front() = applicationMenuEntry(10, defaultApp.get());
          entries.front().label = "Open with " + applicationDisplayName(defaultApp.get());
          entries.front().gutterIconNames = std::move(entries.front().iconNames);
          entries.front().iconNames.clear();
        }

        GList* compatible = g_app_info_get_all_for_type(mime);
        for (GList* item = compatible; item; item = item->next) {
          auto* app = G_APP_INFO(item->data);
          if (!g_app_info_get_id(app) || (defaultApp && g_app_info_equal(defaultApp.get(), app))) {
            g_object_unref(app);
            continue;
          }
          const auto appId = static_cast<std::int32_t>(20000 + m_openWithApplications.size());
          alternatives.push_back(applicationMenuEntry(appId, app));
          m_openWithApplications.emplace_back(app, g_object_unref);
        }
        g_list_free(compatible);

      }
      ContextMenuControlEntry openWith{.id = 80, .label = "Open With", .hasSubmenu = true};
      openWith.submenuEntries = std::move(alternatives);
      openWith.submenuEntries.push_back({.id = 81, .label = "Other Application…"});
      entries.insert(entries.begin() + 1, std::move(openWith));
      if (info)
        g_object_unref(info);
      g_object_unref(file);
    }
    if (m_entries[index].isDir)
      entries.push_back(
          {.id = 60,
           .label = "Open in New Window",
           .actionIconNames = {"window-new-symbolic", "window-new"},
           .actionGlyph = "app-window"}
      );
    if (m_entries[index].isDir)
      entries.push_back(
          {.id = 20,
           .label = "Add to Places",
           .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
           .actionGlyph = "bookmark-plus"}
      );
    entries.push_back(
        {.id = 59,
         .label = "Copy Location",
         .actionIconNames = {"insert-link-symbolic", "insert-link"},
         .actionGlyph = "link"}
    );
    if (m_selectedPaths.size() == 1)
      entries.push_back(
          {.id = 22,
           .label = "Properties",
           .shortcut = "Alt+Enter",
           .actionIconNames = {"document-properties-symbolic", "document-properties"},
           .actionGlyph = "file-info"}
      );
  }
  const bool canCreate = !m_browsingTrash && directorySupportsCreation(m_path);
  if (canCreate)
    entries.push_back(newItemMenu());
  filemanager::appendFileMenuEntries(
      entries,
      m_browsingTrash              ? filemanager::FileMenuContext::Trash
          : m_entries[index].isDir ? filemanager::FileMenuContext::Folder
                                   : filemanager::FileMenuContext::File,
      m_gridMode, m_showHidden, m_sortField, m_sortOrder, m_foldersFirst, m_hiddenLast
  );
  filemanager::groupFileMenuEntries(
      entries,
      m_browsingTrash              ? filemanager::FileMenuContext::Trash
          : m_entries[index].isDir ? filemanager::FileMenuContext::Folder
                                   : filemanager::FileMenuContext::File
  );
  if (!m_browsingTrash) {
    entries.insert(
        std::ranges::find(entries, 4, &ContextMenuControlEntry::id),
        {.id = kDuplicate, .label = "Duplicate", .enabled = canCreate && !m_loading
             && m_deviceOperations.empty() && !m_operations.replaying(),
         .actionGlyph = "copy"}
    );
  }
  auto* output = m_wayland.lastPointerOutput();
  if (!output)
    output = m_wayland.outputForSurface(m_surface->wlSurface());
  m_dispatcher.pointerLeave();
  TooltipManager::instance().forceDestroy();
  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = entries,
          .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
          .parent =
              PopupSurfaceParent{
                  .xdgSurface = m_surface->xdgSurface(), .output = output, .wlSurface = m_surface->wlSurface()
              },
          .inputSerial = m_lastInputSerial,
          .fitAvailableHeight = !m_browsingTrash,
      }
  );
}

void FileManager::openEmptyContextMenu(float x, float y) {
  if (!m_contextMenuPopup || !m_surface)
    return;
  m_selectedPaths.clear();
  m_selectedPath.reset();
  m_contextMenuTarget.reset();
  m_selectionAnchor.reset();
  if (m_grid)
    m_grid->setSelectedIndex(std::nullopt, false);
  std::vector<ContextMenuControlEntry> entries{
      {.id = 3,
       .label = "Paste here",
       .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
       .actionGlyph = "U+EA6F"}
  };
  if (m_browsingTrash)
    entries = {{.id = 72, .label = "Empty Wastebin…"}};
  else {
    if (directorySupportsCreation(m_path))
      entries.push_back(newItemMenu());
    entries.push_back(
        {.id = 21,
         .label = "Add this folder to Places",
         .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
         .actionGlyph = "bookmark-plus"}
    );
  }
  filemanager::appendFileMenuEntries(
      entries, m_browsingTrash ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty, m_gridMode,
      m_showHidden, m_sortField, m_sortOrder, m_foldersFirst, m_hiddenLast
  );
  filemanager::groupFileMenuEntries(
      entries, m_browsingTrash ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty
  );
  wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
  m_dispatcher.pointerLeave();
  TooltipManager::instance().forceDestroy();
  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
          .parent =
              PopupSurfaceParent{
                  .xdgSurface = m_surface->xdgSurface(), .output = output, .wlSurface = m_surface->wlSurface()
              },
          .inputSerial = m_lastInputSerial
      }
  );
}

FileManager::~FileManager() {
  *m_alive = false;
  clearFolderCounts();
  m_menuAnimations.cancelAll();
  if (m_contextMenuPopup)
    m_contextMenuPopup->close();
  m_transfer.cancelDrag();
  clearNativeDragPreview();
  if (m_applicationPicker)
    m_applicationPicker->close();
  if (m_destinationPicker) {
    m_destinationPicker->closeFileDialogWithoutResult();
    FileDialog::cancelIfPending();
    FileDialog::setPresenter(nullptr);
    m_destinationPicker.reset();
  }
  for (const auto& prompt : m_mountPrompts) {
    g_signal_handlers_disconnect_by_data(prompt.get(), this);
    g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
  }
  m_placesAlive.reset();
  cancelPlaceCapacityQueries();
  m_placesModel.reset();
  closeMonitor();
  m_dispatcher.setSceneRoot(nullptr);
  if (m_surface)
    m_surface->setSceneRoot(nullptr);
  m_selectionTimer.stop();
  TooltipManager::instance().shutdown();
  m_renderContext.cleanup();
  m_sharedContext.cleanup();
}

int FileManager::run() {
  for (const auto action :
       {KeybindAction::Validate, KeybindAction::Cancel, KeybindAction::Up, KeybindAction::Down, KeybindAction::Left,
        KeybindAction::Right}) {
    KeybindMatcher::setMatcher(action, [this, action](std::uint32_t sym, std::uint32_t modifiers) {
      return m_configService.matchesKeybind(action, sym, modifiers);
    });
  }
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");
  i18n::Service::instance().init();
  Style::setRtl(i18n::Service::instance().rtl());
  m_configService.addReloadCallback(
      [this] {
        m_themeService.apply();
        IconResolver::invalidateThemeContent();
        if (m_grid)
          m_grid->notifyDataChanged();
        m_placesDirty = true;
        updateDialogTheme();
        if (m_surface)
          m_surface->requestLayout();
        if (m_dialogSurface)
          m_dialogSurface->requestLayout();
      },
      "file-manager-theme"
  );
  m_themeService.apply();
  m_wayland.setNativeDataTransfer(&m_transfer);
  m_transfer.setDragHandlers(
      [this](wl_surface* surface, float x, float y, std::uint32_t actions) { return updateProtocolDrag(surface, x, y, actions); },
      [this] {
        m_protocolDragInside = false;
        m_protocolDropDestination.reset();
        m_protocolDropSources.clear();
        clearDragFeedback();
        m_pinBefore.reset();
        showPlaceInsertion(std::nullopt);
        m_dragTargetValid = false;
        if (m_dragPreview) m_dragPreview->setVisible(false);
        if (m_surface) m_surface->requestUpdate();
        scheduleNativeDragPreview();
      },
      [this](std::uint32_t action) { return receiveProtocolDrop(action); },
      [this](bool success) {
        cancelFileGesture();
        if (m_windowOpen) setStatus(success ? "Drop completed" : "Drop cancelled");
      },
      [this] {
        if (m_transfer.dragDropped()) clearNativeDragPreview();
        else scheduleNativeDragPreview();
      });
  m_wayland.connect();
  m_transfer.setDragIconResources(m_wayland.compositor(), m_wayland.shm());
  if (!m_wayland.hasXdgShell() || m_wayland.compositor() == nullptr || m_wayland.shm() == nullptr)
    throw std::runtime_error("Wayland compositor, shm, and xdg-shell are required");
  m_sharedContext.initialize(m_wayland.display());
  m_renderContext.initialize(m_sharedContext);
  m_renderContext.setTextBaseDirection(i18n::Service::instance().rtl());
  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
  TooltipManager::instance().setFreshHoverPolicy([this] {
    return (m_contextMenuPopup && m_contextMenuPopup->isOpen())
        || m_draggingFiles
        || m_reorderId.has_value()
        || m_dragPending
        || m_resizingSidebar
        || m_dialogSurface
        || (m_destinationPicker && m_destinationPicker->isOpen())
        || (m_applicationPicker && m_applicationPicker->isOpen())
        || m_dispatcher.pointerCaptured()
        || (m_grid && m_grid->rubberBandActive());
  });
  m_dispatcher.setHoverChangeCallback([this](InputArea*, InputArea* next) {
    if (m_surface)
      TooltipManager::instance().onHoverChange(
          next, m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
      );
  });
  m_surface = std::make_unique<ToplevelSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setAnimationManager(&m_menuAnimations);
  m_surface->setClosedCallback([this] {
    m_windowOpen = false;
    clearFolderCounts();
    closeMonitor();
    ++m_enumerationGeneration;
    if (m_destinationPicker) {
      m_destinationPicker->closeFileDialogWithoutResult();
      FileDialog::cancelIfPending();
    }
    if (m_applicationPicker)
      m_applicationPicker->close();
    if (m_propertiesWindow)
      m_propertiesWindow->close();
    cancelFileGesture();
    TooltipManager::instance().forceDestroy();
    cancelPlaceCapacityQueries();
    if (m_contextMenuPopup)
      m_contextMenuPopup->close();
    g_idle_add(
        [](gpointer context) -> gboolean {
          auto* self = static_cast<FileManager*>(context);
          self->m_dispatcher.setSceneRoot(nullptr);
          if (self->m_surface) {
            self->m_surface->setSceneRoot(nullptr);
            self->m_surface.reset();
          }
          self->m_root.reset();
          self->m_dragPreview = nullptr;
          self->m_dragPreviewIcon = nullptr;
          self->m_dragPreviewLabel = nullptr;
          self->m_dragPreviewAction = nullptr;
          self->m_grid = nullptr;
          self->m_locationInput = nullptr;
          self->m_statusLabel = nullptr;
          if (!self->m_operations.active() && self->m_pendingClipboardReads == 0)
            self->m_running = false;
          return G_SOURCE_REMOVE;
        },
        this
    );
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  ToplevelSurfaceConfig config{
      .width = 1120,
      .height = 760,
      .minWidth = 560,
      .minHeight = 380,
      .maxWidth = 0,
      .maxHeight = 0,
      .title = "Nythera File Manager",
      .appId = "dev.nythera.FileManager"
  };
  wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
  if (!m_surface->initialize(output, config))
    throw std::runtime_error("could not create xdg_toplevel");

  m_thumbnailPendingSub = m_pickerThumbnails.subscribePendingUpload([this] {
    if (m_surface)
      m_surface->requestUpdate();
  });

  m_pickerHosts.registerHost(
      [this](wl_surface* surface) -> std::optional<LayerPopupParentContext> {
        if (!m_surface || surface != m_surface->wlSurface())
          return std::nullopt;
        return LayerPopupParentContext{
            .surface = surface,
            .xdgSurface = m_surface->xdgSurface(),
            .output = m_wayland.outputForSurface(surface),
            .width = m_surface->width(),
            .height = m_surface->height()
        };
      },
      {}, {},
      [this]() -> std::optional<LayerPopupParentContext> {
        return m_surface ? m_pickerHosts.contextForSurface(m_surface->wlSurface()) : std::nullopt;
      }
  );
  m_destinationPicker = std::make_unique<FileDialogPopup>();
  m_destinationPicker->initialize(m_wayland, m_configService, m_renderContext, m_pickerHosts, m_pickerThumbnails);
  FileDialog::setPresenter(m_destinationPicker.get());
  m_applicationPicker = std::make_unique<SearchPickerPopup>();
  m_applicationPicker->initialize(m_wayland, m_configService, m_renderContext, m_pickerHosts);

  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_applicationPicker && m_applicationPicker->onPointerEvent(event))
      return;
    if (m_destinationPicker && m_destinationPicker->onPointerEvent(event))
      return;
    if (m_propertiesWindow && m_propertiesWindow->isOpen() && m_propertiesWindow->ownsSurface(event.surface))
      m_propertiesWindow->handlePointer(event);
    else
      handlePointer(event);
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland.setCursorShape(serial, shape);
  });
  m_progressDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland.setCursorShape(serial, shape);
  });
  m_contextMenuPopup = std::make_unique<ContextMenuPopup>(m_wayland, m_renderContext);
  m_contextMenuPopup->setHighlightOnOpen(false);
  m_contextMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) { activateContextMenuEntry(entry); });
  m_contextMenuPopup->setOnDismissed([this] {
    m_applicationMenuOpen.reset();
    m_applicationMenuHovered.reset();
    for (std::size_t i = 0; i < m_applicationMenuButtons.size(); ++i) {
      if (m_applicationMenuButtons[i] && m_applicationMenuButtons[i]->hovered()) {
        m_applicationMenuHovered = i;
        break;
      }
    }
    updateApplicationMenuUnderline();
  });
  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (m_applicationPicker
        && m_applicationPicker->isOpen()
        && m_wayland.lastKeyboardSurface() == m_applicationPicker->wlSurface())
      m_applicationPicker->onKeyboardEvent(event);
    else if (m_destinationPicker && m_destinationPicker->isOpen())
      m_destinationPicker->onKeyboardEvent(event);
    else if (m_contextMenuPopup && m_contextMenuPopup->isOpen())
      m_contextMenuPopup->onKeyboardEvent(event);
    else if (m_dialogSurface && m_wayland.lastKeyboardSurface() == m_dialogSurface->wlSurface())
      handleDialogKeyboard(event);
    else if (
        m_propertiesWindow
        && m_propertiesWindow->isOpen()
        && m_propertiesWindow->ownsSurface(m_wayland.lastKeyboardSurface())
    )
      m_propertiesWindow->handleKeyboard(event);
    else if (m_dialogSurface)
      handleDialogKeyboard(event);
    else if (m_progressSurface && m_wayland.lastKeyboardSurface() == m_progressSurface->wlSurface())
      handleProgressKeyboard(event);
    else
      handleKeyboard(event);
  });
  m_wayland.setKeyboardModifiersCallback([this](std::uint32_t modifiers) {
    m_keyboardModifiers = modifiers;
    m_transfer.updateDragTarget();
    if (m_draggingFiles && m_surface)
      m_surface->requestUpdate();
  });
  navigate(m_path, false);
  TimerPollSource timers;
  MainLoop loop(
      m_wayland,
      [this, &timers] {
        return std::vector<PollSource*>{&m_gioPollSource, &m_configPollSource, &m_pickerThumbnails, &timers, &m_transfer};
      },
      [this] { return !m_running && !m_transfer.ownsSelection() && !m_transfer.busy(); },
      [this] {
        if (m_destinationPicker) {
          m_destinationPicker->closeFileDialogWithoutResult();
          FileDialog::cancelIfPending();
        }
        if (m_applicationPicker)
          m_applicationPicker->close();
        cancelFileGesture();
        TooltipManager::instance().forceDestroy();
        m_dispatcher.setSceneRoot(nullptr);
        m_progressDispatcher.setSceneRoot(nullptr);
        m_dialogDispatcher.setSceneRoot(nullptr);
        if (m_dialogSurface) {
          m_dialogSurface->setSceneRoot(nullptr);
        }
        m_dialogRoot.reset();
        m_dialogSurface.reset();
        if (m_progressSurface) {
          m_progressSurface->setSceneRoot(nullptr);
          m_progressSurface.reset();
        }
        m_progressRoot.reset();
        m_progressLabel = nullptr;
        m_progressCancelButton = nullptr;
        if (m_surface) {
          m_surface->setSceneRoot(nullptr);
          m_surface.reset();
        }
      }
  );
  loop.run();
  return 0;
}

void FileManager::navigate(const std::filesystem::path& requestedPath, bool recordHistory) {
  m_operationResultAfterRefresh.reset();
  cancelFileGesture();
  std::error_code ec;
  const bool trash = requestedPath == "trash:" || requestedPath.string().starts_with("trash:/");
  auto path =
      trash ? requestedPath.lexically_normal() : std::filesystem::absolute(requestedPath, ec).lexically_normal();
  if (ec)
    path = requestedPath.lexically_normal();
  if (trash && !path.string().starts_with("trash:/"))
    path = "trash:/";
  if (m_renamePath && path != m_path)
    cancelRename();
  if (recordHistory && path != m_path) {
    m_backHistory.push_back(m_path);
    m_forwardHistory.clear();
  }
  if (path != m_path) {
    ++m_navigationGeneration;
    m_pendingPasteSelection.reset();
    m_pendingPasteRevealPath.reset();
    m_pendingDropScrollGeneration.reset();
    m_selectedPath.reset();
    m_selectedPaths.clear();
    m_selectionAnchor.reset();
    m_chooseFirstAfterLoad = true;
    m_entries.clear();
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(std::nullopt, false);
      m_grid->notifyDataChanged();
    }
  }
  m_lastPointerIndex.reset();
  m_path = std::move(path);
  m_browsingTrash = trash;
  m_trashUris.clear();
  m_trashOriginalPaths.clear();
  for (const auto& capacity : m_placeCapacities)
    requestPlaceCapacity(capacity, false);
  if (m_locationInput != nullptr)
    m_locationInput->setValue(m_path.string());
  rebuildBreadcrumbs();
  updateNavigationControls();
  enumerate(m_path);
}

void FileManager::navigateTrash() { navigate(std::filesystem::path("trash:/")); }

std::optional<std::uint64_t> FileManager::folderItemCount(const FileEntry& entry) {
  if (!entry.isDir || m_loading || !m_fileColumnLayout.visible[columnIndex(FileColumn::Size)])
    return std::nullopt;
  const auto path = entry.absPath.string();
  if (const auto found = m_folderCounts.find(path); found != m_folderCounts.end())
    return found->second.items;
  const auto id = ++m_nextFolderCountId;
  m_folderCounts.emplace(path, FolderCount{id, std::nullopt});
  m_pendingFolderCounts.emplace_back(path, id);
  startNextFolderCount();
  return std::nullopt;
}

void FileManager::startNextFolderCount() {
  if (m_folderCountRunning)
    return;
  // Only one worker per window. Recent rows take priority after a scroll.
  while (!m_pendingFolderCounts.empty()) {
    auto [path, id] = std::move(m_pendingFolderCounts.back());
    m_pendingFolderCounts.pop_back();
    const auto cached = m_folderCounts.find(path);
    if (cached == m_folderCounts.end() || cached->second.id != id)
      continue;
    g_autoptr(GFile) directory = nullptr;
    if (m_browsingTrash) {
      const auto uri = m_trashUris.find(path);
      if (uri == m_trashUris.end())
        continue;
      directory = g_file_new_for_uri(uri->second.c_str());
    } else {
      directory = g_file_new_for_path(path.c_str());
    }

    struct Request {
      FileManager* manager;
      std::weak_ptr<bool> alive;
      std::filesystem::path path;
      std::uint64_t id;
    };
    m_folderCountRunning = true;
    m_folderCountCancellable = g_cancellable_new();
    GTask* task = g_task_new(
        directory, m_folderCountCancellable,
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* completed = G_TASK(result);
          const auto* request = static_cast<const Request*>(g_task_get_task_data(completed));
          const std::unique_ptr<std::uint64_t> count(
              static_cast<std::uint64_t*>(g_task_propagate_pointer(completed, nullptr))
          );
          const auto alive = request->alive.lock();
          if (!alive || !*alive)
            return;
          auto* self = request->manager;
          g_clear_object(&self->m_folderCountCancellable);
          self->m_folderCountRunning = false;
          const auto found = self->m_folderCounts.find(request->path.string());
          if (count && found != self->m_folderCounts.end() && found->second.id == request->id) {
            found->second.items = *count;
            const auto row = std::ranges::find(self->m_entries, request->path, &FileEntry::absPath);
            if (self->m_grid && row != self->m_entries.end()) {
              self->m_grid->notifyItemChanged(static_cast<std::size_t>(row - self->m_entries.begin()));
              if (self->m_surface)
                self->m_surface->requestLayout();
            }
          }
          self->startNextFolderCount();
        },
        nullptr
    );
    g_task_set_task_data(
        task, new Request{this, m_alive, std::filesystem::path(std::move(path)), id},
        [](gpointer data) { delete static_cast<Request*>(data); }
    );
    g_task_run_in_thread(task, [](GTask* worker, gpointer source, gpointer, GCancellable* cancellable) {
      const auto count = filemanager::countImmediateFolderItems(G_FILE(source), cancellable);
      g_task_return_pointer(
          worker, count ? new std::uint64_t(*count) : nullptr,
          [](gpointer data) { delete static_cast<std::uint64_t*>(data); }
      );
    });
    g_object_unref(task);
    return;
  }
}

void FileManager::clearFolderCounts() {
  if (m_folderCountCancellable) {
    g_cancellable_cancel(m_folderCountCancellable);
    g_clear_object(&m_folderCountCancellable);
  }
  m_pendingFolderCounts.clear();
  m_folderCounts.clear();
}

void FileManager::enumerate(const std::filesystem::path& path) {
  FileIcon::invalidateDirectoryMetadata();
  cancelFileGesture();
  if (m_renamePath) {
    m_refreshAfterRename = true;
    return;
  }
  closeMonitor();
  m_path = path;
  clearFolderCounts();
  const auto generation = ++m_enumerationGeneration;
  m_pendingMonitorQueries = 0;
  m_loading = true;
  m_loadingEntries.clear();
  m_pendingMonitorChanges.clear();
  setStatus("Loading…");
  rebuildBreadcrumbs();
  if (m_locationInput != nullptr)
    m_locationInput->setValue(m_path.string());
  if (m_surface)
    m_surface->requestLayout();
  monitorDirectory();

  GFile* file = browsingFile();
  g_file_enumerate_children_async(
      file,
      "standard::name,standard::display-name,standard::type,standard::size,standard::content-type,time::modified,time::"
      "created,time::access,trash::orig-path",
      G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::pair<FileManager*, std::uint64_t>> request(
            static_cast<std::pair<FileManager*, std::uint64_t>*>(data)
        );
        auto* self = request->first;
        const auto requestGeneration = request->second;
        GError* error = nullptr;
        GFileEnumerator* enumerator = g_file_enumerate_children_finish(G_FILE(source), result, &error);
        if (requestGeneration != self->m_enumerationGeneration) {
          if (enumerator != nullptr)
            g_object_unref(enumerator);
          g_clear_error(&error);
          return;
        }
        if (enumerator == nullptr) {
          self->m_loading = false;
          self->m_loadingEntries.clear();
          const std::string detail = error == nullptr ? "Unable to open this folder" : error->message;
          self->setStatus(self->m_operationResultAfterRefresh
                              ? *self->m_operationResultAfterRefresh + "; refresh failed: " + detail
                              : detail);
          self->m_operationResultAfterRefresh.reset();
          g_clear_error(&error);
          if (self->m_surface)
            self->m_surface->requestLayout();
          self->applyPendingMonitorChanges();
          return;
        }
        g_clear_error(&error);
        self->fetchNext(enumerator, requestGeneration);
        g_object_unref(enumerator);
      },
      new std::pair<FileManager*, std::uint64_t>{this, generation}
  );
  g_object_unref(file);
}

void FileManager::restoreWastebinSelection(bool chooseDestination) {
  if (!m_browsingTrash || !m_windowOpen)
    return;
  if (!m_deviceOperations.empty()) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (std::ranges::any_of(m_operations.operations(), [](const auto& operation) {
        return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
      })) {
    setStatus("Wait for Wastebin emptying to finish");
    return;
  }
  std::vector<std::string> uris;
  for (const auto& path : selectedPaths()) {
    const auto found = m_trashUris.find(path);
    if (found == m_trashUris.end()) {
      setStatus("A selected Wastebin item is no longer available; refresh the view");
      return;
    }
    uris.push_back(found->second);
  }
  if (uris.empty())
    return;
  if (!chooseDestination) {
    m_operations.restore(std::move(uris));
    return;
  }
  FileDialogOptions options{
      .mode = FileDialogMode::SelectFolder, .startDirectory = g_get_home_dir(), .title = "Restore to…"
  };
  const auto alive = m_alive;
  if (!FileDialog::open(std::move(options), [this, alive, uris = std::move(uris)](auto destination) mutable {
        if (*alive && m_windowOpen && destination)
          m_operations.restore(std::move(uris), destination->string());
      }))
    setStatus("Could not open the destination picker");
}

void FileManager::confirmEmptyWastebin() {
  if (!m_windowOpen)
    return;
  if (m_operations.active() || !m_deviceOperations.empty()) {
    setStatus("Wait for the current operation to finish before emptying Wastebin");
    return;
  }
  showDialog(
      "Empty Wastebin?",
      "Permanently delete all items in Wastebin, including Wastebins on mounted filesystems?\n\n"
      "This cannot be undone. These items cannot be restored from Wastebin afterwards.",
      {{"Cancel", 0}, {"Empty Wastebin", 1}}, [this](int response) {
        if (response == 1 && m_windowOpen) {
          if (m_operations.active() || !m_deviceOperations.empty())
            setStatus("Wait for the current operation to finish before emptying Wastebin");
          else
            m_operations.emptyWastebin(true);
        }
      }
  );
}

void FileManager::fetchNext(GFileEnumerator* enumerator, std::uint64_t generation) {
  g_file_enumerator_next_files_async(
      enumerator, 128, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::tuple<FileManager*, GFileEnumerator*, std::uint64_t>> state(
            static_cast<std::tuple<FileManager*, GFileEnumerator*, std::uint64_t>*>(data)
        );
        auto* self = std::get<0>(*state);
        auto* activeEnumerator = std::get<1>(*state);
        const auto requestGeneration = std::get<2>(*state);
        GError* error = nullptr;
        GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
        if (requestGeneration != self->m_enumerationGeneration) {
          g_list_free_full(infos, g_object_unref);
          g_clear_error(&error);
          g_object_unref(activeEnumerator);
          return;
        }
        if (error != nullptr) {
          self->m_loading = false;
          self->m_loadingEntries.clear();
          self->setStatus(
              self->m_operationResultAfterRefresh
                  ? *self->m_operationResultAfterRefresh + "; refresh failed: " + error->message
                  : error->message
          );
          self->m_operationResultAfterRefresh.reset();
          g_clear_error(&error);
          g_list_free_full(infos, g_object_unref);
          g_object_unref(activeEnumerator);
          if (self->m_surface)
            self->m_surface->requestLayout();
          self->applyPendingMonitorChanges();
          return;
        }
        for (GList* node = infos; node != nullptr; node = node->next) {
          auto* info = G_FILE_INFO(node->data);
          const char* name = g_file_info_get_name(info);
          if (name == nullptr || (!self->m_showHidden && name[0] == '.'))
            continue;
          auto entry = makeFileEntry(info, self->m_path / name);
          if (self->m_browsingTrash) {
            GFile* child = g_file_enumerator_get_child(activeEnumerator, info);
            char* uri = g_file_get_uri(child);
            if (uri != nullptr)
              self->m_trashUris[entry.absPath.string()] = uri;
            const char* original = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
            if (original != nullptr)
              self->m_trashOriginalPaths[entry.absPath.string()] = original;
            const char* displayName = g_file_info_get_display_name(info);
            if (displayName != nullptr)
              entry.name = displayName;
            g_free(uri);
            g_object_unref(child);
          }
          self->m_loadingEntries.push_back(std::move(entry));
        }
        g_list_free_full(infos, g_object_unref);
        if (infos == nullptr) {
          self->m_loading = false;
          self->m_entries = std::move(self->m_loadingEntries);
          if (!self->m_selectedPaths.empty()) {
            const auto selected = self->selectedPaths();
            self->m_selectedPaths = std::unordered_set<std::string>(selected.begin(), selected.end());
          }
          if (self->m_selectionAnchor
              && std::ranges::find(self->m_entries, *self->m_selectionAnchor, &FileEntry::absPath)
                  == self->m_entries.end())
            self->m_selectionAnchor.reset();
          self->sortEntries();
          self->setStatus(
              self->m_operationResultAfterRefresh.value_or(self->m_entries.empty() ? "This folder is empty" : "")
          );
          self->m_operationResultAfterRefresh.reset();
          if (self->m_grid != nullptr) {
            self->m_grid->notifyDataChanged();
          }
          self->restoreSelection(self->m_chooseFirstAfterLoad);
          self->m_chooseFirstAfterLoad = false;
          if (self->m_surface)
            self->m_surface->requestLayout();
          g_object_unref(activeEnumerator);
          self->applyPendingMonitorChanges();
          self->applyPendingPasteSelection();
          return;
        }
        self->fetchNext(activeEnumerator, requestGeneration);
        g_object_unref(activeEnumerator);
      },
      new std::tuple<FileManager*, GFileEnumerator*, std::uint64_t>{
          this, G_FILE_ENUMERATOR(g_object_ref(enumerator)), generation
      }
  );
}

void FileManager::openApplicationChooser() {
  if (!m_applicationPicker || !m_contextMenuTarget || m_browsingTrash)
    return;
  GFile* file = g_file_new_for_path(m_contextMenuTarget->c_str());
  GFileInfo* info =
      g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
  g_object_unref(file);
  const char* contentType = info ? g_file_info_get_content_type(info) : nullptr;
  m_applicationChooserMime = contentType ? contentType : "";
  if (info)
    g_object_unref(info);
  m_applicationChooserApps.clear();
  std::vector<SearchPickerOption> options;
  std::unordered_set<std::string> recommendedIds;
  std::unordered_set<std::string> associatedIds;
  if (!m_applicationChooserMime.empty()) {
    GList* recommended = g_app_info_get_recommended_for_type(m_applicationChooserMime.c_str());
    for (GList* item = recommended; item; item = item->next) {
      const char* id = g_app_info_get_id(G_APP_INFO(item->data));
      if (id && *id)
        recommendedIds.emplace(id);
    }
    g_list_free_full(recommended, g_object_unref);
    associatedIds = recommendedIds;
    GList* associated = g_app_info_get_all_for_type(m_applicationChooserMime.c_str());
    for (GList* item = associated; item; item = item->next) {
      const char* id = g_app_info_get_id(G_APP_INFO(item->data));
      if (id && *id)
        associatedIds.emplace(id);
    }
    g_list_free_full(associated, g_object_unref);
  }

  std::unordered_set<std::string> seen;
  auto appendApps = [&](GList* apps) {
    for (GList* item = apps; item; item = item->next) {
      auto* app = G_APP_INFO(item->data);
      const char* id = g_app_info_get_id(app);
      if (!id
          || !*id
          || !g_app_info_should_show(app)
          || (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))
          || !seen.emplace(id).second) {
        g_object_unref(app);
        continue;
      }
      const bool compatible = associatedIds.contains(id);
      const std::size_t index = m_applicationChooserApps.size();
      options.push_back(
          SearchPickerOption{
              .value = std::to_string(index),
              .label = applicationDisplayName(app),
              .description = compatible ? "Compatible with this file" : "Installed application",
              .iconNames = applicationIconNames(app),
              .compatible = compatible
          }
      );
      m_applicationChooserApps.emplace_back(app, g_object_unref);
    }
    g_list_free(apps);
  };

  if (!m_applicationChooserMime.empty()) {
    GAppInfo* defaultApp = g_app_info_get_default_for_type(m_applicationChooserMime.c_str(), FALSE);
    if (defaultApp != nullptr) {
      const char* id = g_app_info_get_id(defaultApp);
      if (id && *id) {
        associatedIds.emplace(id);
        GList* defaultList = nullptr;
        defaultList = g_list_append(defaultList, defaultApp);
        appendApps(defaultList);
      } else {
        g_object_unref(defaultApp);
      }
    }
  }

  if (!m_applicationChooserMime.empty()) {
    appendApps(g_app_info_get_recommended_for_type(m_applicationChooserMime.c_str()));
    appendApps(g_app_info_get_all_for_type(m_applicationChooserMime.c_str()));
  }
  appendApps(g_app_info_get_all());
  m_applicationPicker->setOnSelectWithOption([this](const std::string& value, bool setAsDefault) {
    std::size_t index = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
    if (error != std::errc{}
        || end != value.data() + value.size()
        || index >= m_applicationChooserApps.size()
        || !m_contextMenuTarget) {
      setStatus("The selected application is no longer available");
      return;
    }
    GAppInfo* app = m_applicationChooserApps[index].get();
    if (setAsDefault) {
      GError* defaultError = nullptr;
      if (!filemanager::savePropertyDefaultApplication(app, m_applicationChooserMime, &defaultError)) {
        setStatus(defaultError ? defaultError->message : "Could not save the default application");
        g_clear_error(&defaultError);
      }
    }
    GError* launchError = nullptr;
    if (!launchApplicationForFile(app, *m_contextMenuTarget, &launchError))
      setStatus(launchError ? launchError->message : "Could not open this file with the selected application");
    else if (!setAsDefault)
      setStatus("");
    g_clear_error(&launchError);
  });
  SearchPickerPopupRequest request{
      .title = "Open With",
      .options = std::move(options),
      .placeholder = "Search applications…",
      .emptyText = "No matching applications",
      .optionToggleChecked = false,
      .showCancelButton = true,
      .explicitConfirmation = true,
      .showAllApplicationsToggle = true,
      .preferredWidth = 500.0F,
      .preferredHeight = 500.0F,
      .nativeWindow = true,
      .nativeParent = m_surface ? m_surface->xdgToplevel() : nullptr,
      .nativeParentSurface = m_surface ? m_surface->wlSurface() : nullptr
  };
  if (!m_applicationChooserMime.empty())
    request.optionToggleLabel = "Set as default for this file type";
  m_applicationPicker->open(std::move(request));
}

void FileManager::activateContextMenuEntry(const ContextMenuControlEntry& entry) {
  if (entry.id >= 30001 && entry.id <= kShowMenuBar) {
    switch (entry.id) {
    case 30001:
      if (!m_browsingTrash)
        launchNewWindow(m_path);
      break;
    case 30002:
      activateSelected();
      break;
    case 30003:
      if (m_selectedPath) {
        m_contextMenuTarget = *m_selectedPath;
        openApplicationChooser();
      }
      break;
    case 30004:
      if (m_selectedPath)
        launchNewWindow(*m_selectedPath);
      break;
    case 30005:
      if (m_selectedPath)
        savePlace(*m_selectedPath);
      break;
    case 30006:
      if (m_selectedPath && m_selectedPaths.size() == 1)
        openProperties(*m_selectedPath);
      break;
    case 30007:
      requestQuit(m_surface.get());
      break;
    case kShowMenuBar:
      m_showApplicationMenuBar = !m_showApplicationMenuBar;
      persistApplicationMenuPreference();
      m_surface->requestLayout();
      break;
    }
    return;
  }
  if (entry.id >= 20000 && m_contextMenuTarget) {
    const auto index = static_cast<std::size_t>(entry.id - 20000);
    if (index >= m_openWithApplications.size()) {
      setStatus("That application is no longer available");
      return;
    }
    GError* error = nullptr;
    const bool launched = launchApplicationForFile(m_openWithApplications[index].get(), *m_contextMenuTarget, &error);
    if (!launched)
      setStatus(error ? error->message : "Could not open this file with the selected application");
    else
      setStatus("");
    g_clear_error(&error);
    return;
  }
  if (entry.id == 81) {
    openApplicationChooser();
    return;
  }
  if (entry.id >= 90 && entry.id <= 93 && m_locationInput != nullptr) {
    switch (entry.id) {
    case 90: // Cut
      m_dispatcher.keyEvent('x', 0, KeyMod::Ctrl, true, false);
      break;
    case 91: // Copy
      (void)m_locationInput->copySelectedText();
      break;
    case 92: // Paste
      m_dispatcher.keyEvent('v', 0, KeyMod::Ctrl, true, false);
      break;
    case 93: // Select all
      m_locationInput->selectAll();
      break;
    }
    return;
  }
  const bool createsItem = entry.id >= 111 && entry.id <= kSelectionSymlink;
  if (!m_deviceOperations.empty()
      && ((entry.id >= 1 && entry.id <= 6) || createsItem || entry.id == kDuplicate
          || entry.id == kUndo || entry.id == kRedo)) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (m_operations.replaying()
      && ((entry.id >= 3 && entry.id <= 6) || (entry.id >= 70 && entry.id <= 72) || createsItem
          || entry.id == kDuplicate)) {
    setStatus("Wait for Undo or Redo to finish");
    return;
  }
  switch (entry.id) {
  case kSelectAll:
    changeSelection(SelectionChange::All);
    return;
  case kDeselectAll:
    changeSelection(SelectionChange::None);
    return;
  case kInvertSelection:
    changeSelection(SelectionChange::Invert);
    return;
  case kDuplicate:
    duplicateSelection();
    return;
  case kSelectionSymlink:
    createSelectionSymlink();
    return;
  case kManualSymlink:
    showSymlinkDialog();
    return;
  case kUndo:
  case kRedo:
    if (!m_dialogSurface && !m_renamePath && !m_transfer.busy() && m_pendingClipboardReads == 0) {
      if (entry.id == kUndo && m_operations.canUndo())
        m_operations.undo();
      else if (entry.id == kRedo && m_operations.canRedo())
        m_operations.redo();
    }
    return;
  default:
    break;
  }
  if (entry.id == 70 || entry.id == 71) {
    restoreWastebinSelection(entry.id == 71);
    return;
  }
  if (entry.id == 72) {
    confirmEmptyWastebin();
    return;
  }
  if (entry.id == 20) {
    if (m_contextMenuTarget)
      savePlace(*m_contextMenuTarget);
    return;
  }
  if (entry.id == 60 && m_contextMenuTarget) {
    launchNewWindow(*m_contextMenuTarget);
    return;
  }
  if (entry.id == 59) {
    if (m_contextMenuTarget) {
      writeDesktopClipboard(m_contextMenuTarget->string(), "text/plain;charset=utf-8");
    } else {
      const auto paths = selectedPaths();
      if (paths.size() == 1)
        writeDesktopClipboard(paths.front(), "text/plain;charset=utf-8");
    }
    return;
  }
  if (entry.id == 21) {
    savePlace(m_path);
    return;
  }
  if (entry.id == 38) {
    m_showHiddenMounts = !m_showHiddenMounts;
    m_placesDirty = true;
    m_surface->requestLayout();
    return;
  }
  if ((entry.id == 36 || entry.id == 37) && m_placeMenuTarget) {
    setPlaceHidden(*m_placeMenuTarget, entry.id == 36);
    return;
  }
  if ((entry.id == 39 || entry.id == 40) && m_placeMenuTarget) {
    const auto place = *m_placeMenuTarget;
    if (entry.id == 40) {
      if (place.mounted && !place.path.empty())
        launchNewWindow(place.path);
    } else if (place.mounted && !place.path.empty()) {
      openProperties(place.path);
    } else {
      if (!m_propertiesWindow)
        m_propertiesWindow = std::make_unique<PropertiesWindow>(
            m_wayland, m_renderContext, m_configService, m_operations,
            [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); }
        );
      if (!m_propertiesWindow->openVolume(place, m_surface.get()))
        setStatus("Could not open Properties window");
    }
    return;
  }
  if (entry.id >= 30 && entry.id <= 35 && m_placeMenuTarget) {
    auto place = *m_placeMenuTarget;
    if (entry.id == 30) {
      if (place.identity == "personal:trash")
        navigateTrash();
      else if (place.mounted && !place.path.empty())
        navigate(place.path);
    } else if (entry.id == 31) {
      std::erase_if(m_savedPlaces, [&](const auto& saved) { return "saved:" + saved.id == place.identity; });
      persistSavedPlaces();
    } else if (entry.id == 32)
      savePlace(place.path);
    else
      operatePlace(
          place,
          entry.id == 33       ? filemanager::PlaceOperation::Mount
              : entry.id == 34 ? filemanager::PlaceOperation::Unmount
                               : filemanager::PlaceOperation::Eject
      );
    return;
  }
  if (entry.id == 10) {
    if (m_contextMenuTarget)
      activatePath(*m_contextMenuTarget);
  } else if (entry.id == 22) {
    if (m_contextMenuTarget && m_selectedPaths.size() == 1)
      openProperties(*m_contextMenuTarget);
  } else if (entry.id == 1)
    copyToDesktopClipboard(false);
  else if (entry.id == 2)
    copyToDesktopClipboard(true);
  else if (entry.id == 3)
    pasteClipboard();
  else if (entry.id == 4)
    beginRename();
  else if (entry.id == 5 && !m_browsingTrash) {
    auto targets = selectedPaths();
    if (!targets.empty())
      m_operations.trash(std::move(targets));
  } else if (entry.id == 6 && !m_browsingTrash) {
    auto targets = selectedPaths();
    if (!targets.empty()) {
      const auto prompt = filemanager::permanentDeletePrompt(targets, targets.size());
      showDialog(
          "Permanently delete?", prompt, {{"Cancel", 0}, {"Delete", 1}},
          [this, targets = std::move(targets)](int response) mutable {
            if (response == 1)
              m_operations.removePermanently(std::move(targets));
          }
      );
    }
  } else if (entry.id >= 114 && entry.id <= 116) {
    if (m_browsingTrash || !directorySupportsCreation(m_path)) {
      setStatus("This directory does not support file creation");
      return;
    }
    const int id = entry.id;
    const char* extension = id == 114 ? ".odt" : id == 115 ? ".ods" : ".odp";
    const char* asset = id == 114 ? "templates/filemanager/office/blank.odt"
        : id == 115                        ? "templates/filemanager/office/blank.ods"
                                          : "templates/filemanager/office/blank.odp";
    const std::string defaultName = id == 114 ? "New Document" : id == 115 ? "New Spreadsheet" : "New Presentation";
    std::error_code destinationError;
    auto destination =
        filemanager::creationDestinationWithExtension(m_path, defaultName, extension, destinationError);
    if (!destination) {
      setStatus(destinationError ? destinationError.message() : "Could not choose a filename");
      return;
    }
    const auto sourcePath = paths::assetPath(asset);
    GFile* source = g_file_new_for_path(sourcePath.c_str());
    GFile* target = g_file_new_for_path(destination->c_str());
    GError* error = nullptr;
    const bool copied = g_file_copy(source, target, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &error);
    g_object_unref(source);
    g_object_unref(target);
    if (!copied) {
      // A failed backend copy can leave a destination behind.
      std::error_code probeError;
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)
          && std::filesystem::exists(std::filesystem::symlink_status(*destination, probeError))) {
        m_operations.noteExternalMutation();
      }
      setStatus(error ? error->message : "Could not create office document");
      g_clear_error(&error);
      return;
    }
    g_clear_error(&error);
    m_operations.noteExternalMutation();
    m_selectedPath = *destination;
    m_selectedPaths.clear();
    m_selectedPaths.insert(destination->string());
    m_pendingInlineRenamePath = *destination;
    setStatus("");
    enumerate(m_path);
  } else if (entry.id >= 111 && entry.id <= 113) {
    if (m_browsingTrash || !directorySupportsCreation(m_path)) {
      setStatus("This directory does not support file creation");
      return;
    }
    const int id = entry.id;
    const std::string defaultName = id == 111 ? "New Folder" : "New File";
    std::error_code error;
    auto destination = filemanager::creationDestination(m_path, defaultName, id == 113, error);
    if (!destination) {
      setStatus(error ? error.message() : "Could not choose a filename");
      return;
    }
    if (id == 111)
      m_operations.createFolder(destination->string());
    else
      m_operations.createFile(destination->string());
  } else if (entry.id == 13)
    enumerate(m_path);
  else if (entry.id == 14 || entry.id == kListView || entry.id == kGridView) {
    setGridMode(entry.id == 14 ? !m_gridMode : entry.id == kGridView);
  } else if (entry.id >= kZoomIn && entry.id <= kResetZoom) {
    adjustViewScale(entry.id == kResetZoom ? filemanager::kFileViewDefaultScale
        : m_viewScale + (entry.id == kZoomIn ? filemanager::kFileViewZoomStep : -filemanager::kFileViewZoomStep));
  } else if (entry.id >= kFirstColumnAction
             && entry.id < kFirstColumnAction + static_cast<int>(filemanager::kFileColumns.size())) {
    const auto column = static_cast<FileColumn>(entry.id - kFirstColumnAction);
    if (column != FileColumn::Name) {
      auto& visible = m_fileColumnLayout.visible[columnIndex(column)];
      visible = !visible;
      if (column == FileColumn::Size && !visible)
        clearFolderCounts();
      if (!m_gridMode)
        updateFileColumnHeaders();
      persistFileColumnLayout();
    }
  } else if (entry.id >= 53 && entry.id <= 57) {
    const auto field = static_cast<FileDialogSortField>(entry.id - 53);
    if (field != m_sortField)
      setSortField(field);
  } else if (entry.id == 61) {
    if (m_sortField != FileDialogSortField::Type)
      setSortField(FileDialogSortField::Type);
  } else if (entry.id == 58 || entry.id == 62) {
    const auto requested = entry.id == 58 ? FileDialogSortOrder::Ascending : FileDialogSortOrder::Descending;
    if (m_sortOrder != requested)
      toggleSortOrder();
  } else if (entry.id == 50) {
    m_showHidden = !m_showHidden;
    persistFileViewState();
    enumerate(m_path);
  } else if (entry.id == 51 || entry.id == 52) {
    if (entry.id == 51)
      m_foldersFirst = !m_foldersFirst;
    else
      m_hiddenLast = !m_hiddenLast;
    persistFileViewState();
    sortEntries();
    if (m_grid) {
      m_grid->notifyDataChanged();
      restoreSelection();
    }
  }
}

void FileManager::openProperties(const std::filesystem::path& path) {
  if (!m_propertiesWindow)
    m_propertiesWindow = std::make_unique<PropertiesWindow>(
        m_wayland, m_renderContext, m_configService, m_operations,
        [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); }
    );
  if (!m_propertiesWindow->open(path, m_surface.get()))
    setStatus("Could not open Properties window");
}

void FileManager::sortEntries() {
  cancelFileGesture();
  std::ranges::stable_sort(m_entries, [this](const FileEntry& lhs, const FileEntry& rhs) {
    return filemanager::fileEntryLess(lhs, rhs, m_sortField, m_sortOrder, m_foldersFirst, m_hiddenLast);
  });
}

void FileManager::persistFileViewState() {
  bool saved = m_configService.setStateBool("files", "show_hidden", m_showHidden);
  saved = m_configService.setStateBool("files", "folders_first", m_foldersFirst) && saved;
  saved = m_configService.setStateBool("files", "hidden_last", m_hiddenLast) && saved;
  saved = m_configService.setStateString("files", "sort_field", std::to_string(static_cast<int>(m_sortField))) && saved;
  saved =
      m_configService.setStateBool("files", "sort_descending", m_sortOrder == FileDialogSortOrder::Descending) && saved;
  saved = m_configService.setStateBool("files", "grid_view", m_gridMode) && saved;
  if (!saved)
    setStatus("Unable to save file view preferences");
}

void FileManager::persistApplicationMenuPreference() {
  if (!m_configService.setStateBool("files", "show_menu_bar", m_showApplicationMenuBar))
    setStatus("Unable to save menu preference");
}

void FileManager::updateApplicationMenuUnderline() {
  if (!m_applicationMenuUnderline)
    return;
  std::optional<std::size_t> target;
  if (!m_applicationMenuCollapsed) {
    if (m_applicationMenuOpen)
      target = m_applicationMenuOpen;
    else
      target = m_applicationMenuHovered;
  }

  m_menuAnimations.cancel(m_menuUnderlineAnimation);
  m_menuUnderlineStartX = m_applicationMenuUnderline->x();
  m_menuUnderlineStartWidth = m_applicationMenuUnderline->width();
  const float startOpacity = m_applicationMenuUnderline->opacity();
  float targetX = m_menuUnderlineStartX;
  float targetWidth = m_menuUnderlineStartWidth;
  float targetY = m_applicationMenuUnderline->y();
  if (target && *target < m_applicationMenuButtons.size()) {
    const Button* button = m_applicationMenuButtons[*target];
    if (button && button->label()) {
      targetX = button->x() + button->label()->x();
      targetY = button->y() + button->height() - 2.0F;
      targetWidth = button->label()->width();
    } else {
      target.reset();
    }
  }
  const float targetOpacity = target ? 1.0F : 0.0F;
  m_applicationMenuUnderline->setPosition(m_menuUnderlineStartX, targetY);
  m_menuUnderlineAnimation = m_menuAnimations.animate(
      0.0F, 1.0F, 200.0F, Easing::EaseOutCubic,
      [this, targetX, targetWidth, startOpacity, targetOpacity](float progress) {
        if (!m_applicationMenuUnderline)
          return;
        m_applicationMenuUnderline->setPosition(std::lerp(m_menuUnderlineStartX, targetX, progress),
                                                m_applicationMenuUnderline->y());
        m_applicationMenuUnderline->setFrameSize(
            std::lerp(m_menuUnderlineStartWidth, targetWidth, progress), 2.0F);
        m_applicationMenuUnderline->setOpacity(std::lerp(startOpacity, targetOpacity, progress));
      },
      {}, this);
}

std::vector<ContextMenuControlEntry> FileManager::fileMenuEntries() const {
  const auto selectedEntry =
      m_selectedPath ? std::ranges::find(m_entries, *m_selectedPath, &FileEntry::absPath) : m_entries.end();
  const bool selectedDirectory = selectedEntry != m_entries.end() && selectedEntry->isDir;
  bool selectedDirectorySaved = false;
  if (selectedDirectory) {
    std::error_code canonicalError;
    const auto canonicalSelected = std::filesystem::canonical(selectedEntry->absPath, canonicalError);
    selectedDirectorySaved = !canonicalError && std::ranges::any_of(m_placeRows, [&](const auto& row) {
      if (!row.second.personal)
        return false;
      std::error_code savedError;
      const auto canonicalSaved = std::filesystem::canonical(row.second.path, savedError);
      return row.second.path == selectedEntry->absPath || (!savedError && canonicalSaved == canonicalSelected);
    });
  }
  std::vector<ContextMenuControlEntry> entries{
      {.id = 30001,
       .label = "New Window",
       .enabled = !m_browsingTrash,
       .shortcut = "Ctrl+N",
       .actionIconNames = {"window-new-symbolic", "window-new"},
       .actionGlyph = "app-window"},
      newItemMenu(),
      {.separator = true},
      {.id = 30002,
       .label = "Open",
       .enabled = selectedEntry != m_entries.end(),
       .shortcut = "Enter",
       .actionIconNames = {"folder-open-symbolic", "folder-open"},
       .actionGlyph = "folder-open"},
      {.id = 30003,
       .label = "Open With…",
       .enabled = !m_browsingTrash
           && m_selectedPaths.size() == 1
           && selectedEntry != m_entries.end()
           && !selectedEntry->isDir},
      {.id = 30004,
       .label = "Open in New Window",
       .enabled = !m_browsingTrash && selectedDirectory,
       .actionIconNames = {"window-new-symbolic", "window-new"},
       .actionGlyph = "app-window"},
      {.separator = true},
      {.id = 30005,
       .label = "Add to Places",
       .enabled = !m_browsingTrash && selectedDirectory && !selectedDirectorySaved,
       .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
       .actionGlyph = "bookmark-plus"},
      {.separator = true},
      {.id = 30006,
       .label = "Properties",
       .enabled = !m_browsingTrash && m_selectedPaths.size() == 1 && selectedEntry != m_entries.end(),
       .shortcut = "Alt+Enter",
       .actionIconNames = {"document-properties-symbolic", "document-properties"},
       .actionGlyph = "file-info"},
      {.separator = true},
      {.id = 30007, .label = "Quit", .shortcut = "Ctrl+Q"},
  };
  for (auto& entry : entries)
    if (entry.id == 110)
      entry.enabled = !m_browsingTrash && directorySupportsCreation(m_path);
  return entries;
}

std::vector<ContextMenuControlEntry> FileManager::editMenuEntries() const {
  const bool hasSelection = !m_loading && !m_selectedPaths.empty();
  const bool oneItem = hasSelection && m_selectedPaths.size() == 1;
  const bool canSelect = canChangeSelection();
  const bool canMutate = m_deviceOperations.empty() && !m_operations.replaying();
  const bool canReplay =
      canMutate && !m_dialogSurface && !m_renamePath && !m_transfer.busy() && m_pendingClipboardReads == 0;
  const bool local = !m_browsingTrash;
  const bool canCreate = local && canMutate && directorySupportsCreation(m_path);
  std::vector<ContextMenuControlEntry> entries{
      {.id = kUndo, .label = m_operations.undoLabel(), .enabled = canReplay && m_operations.canUndo(),
       .shortcut = "Ctrl+Z", .actionGlyph = "arrow-back-up"},
      {.id = kRedo, .label = m_operations.redoLabel(), .enabled = canReplay && m_operations.canRedo(),
       .shortcut = "Ctrl+Shift+Z", .actionGlyph = "arrow-forward-up"},
      {.separator = true},
      {.id = 2, .label = "Cut", .enabled = local && hasSelection && m_deviceOperations.empty(),
       .shortcut = "Ctrl+X", .actionGlyph = "scissors"},
      {.id = 1, .label = "Copy", .enabled = local && hasSelection && m_deviceOperations.empty(),
       .shortcut = "Ctrl+C", .actionGlyph = "copy"},
      {.id = 3, .label = "Paste", .enabled = local && canMutate, .shortcut = "Ctrl+V", .actionGlyph = "U+EA6F"},
      {.id = 59, .label = "Copy Location", .enabled = local && oneItem, .actionGlyph = "link"},
      {.separator = true},
      {.id = kSelectAll, .label = "Select All", .enabled = canSelect && m_selectedPaths.size() < m_entries.size(),
       .shortcut = "Ctrl+A"},
      {.id = kDeselectAll, .label = "Deselect All", .enabled = canSelect && hasSelection},
      {.id = kInvertSelection, .label = "Invert Selection", .enabled = canSelect && !m_entries.empty()},
      {.separator = true},
  };
  if (m_browsingTrash) {
    const bool restoring = canMutate && hasSelection
        && std::ranges::none_of(m_operations.operations(), [](const auto& operation) {
             return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
           });
    entries.push_back({.id = 70, .label = "Restore", .enabled = restoring});
    entries.push_back({.id = 71, .label = "Restore to…", .enabled = restoring});
    return entries;
  }
  entries.insert(entries.end(), {
      {.id = kDuplicate, .label = "Duplicate", .enabled = canCreate && hasSelection, .actionGlyph = "copy"},
      {.id = 4, .label = "Rename", .enabled = canMutate && hasSelection, .shortcut = "F2", .actionGlyph = "pencil"},
      {.id = 5, .label = "Move to Wastebin", .enabled = canMutate && hasSelection,
       .shortcut = "Delete", .actionGlyph = "trash"},
      {.id = 6, .label = "Delete Permanently", .enabled = canMutate && hasSelection, .shortcut = "Shift+Delete"},
      {.separator = true},
  });
  if (const auto selection = canCreate ? symlinkSelection() : std::nullopt) {
    entries.push_back({.id = kSelectionSymlink, .label = selection->second ? "Symlink to Folder" : "Symlink to File",
                       .actionGlyph = "link"});
  }
  entries.push_back({.id = kManualSymlink, .label = "Symlink…", .enabled = canCreate, .actionGlyph = "link"});
  return entries;
}

std::vector<ContextMenuControlEntry> FileManager::viewMenuEntries() const {
  std::vector<ContextMenuControlEntry> columns;
  for (auto column : filemanager::kFileColumns) {
    const auto index = columnIndex(column);
    columns.push_back(
        {.id = kFirstColumnAction + static_cast<int>(index), .label = std::string(filemanager::kFileColumnLabels[index]),
         .enabled = column != FileColumn::Name, .checkmark = true, .toggleState = m_fileColumnLayout.visible[index] ? 1 : 0}
    );
  }
  return {
      {.id = kListView, .label = "List View", .radio = true, .toggleState = m_gridMode ? 0 : 1,
       .actionGlyph = "layout-list"},
      {.id = kGridView, .label = "Grid View", .radio = true, .toggleState = m_gridMode ? 1 : 0,
       .actionGlyph = "layout-grid"},
      {.id = kShowColumns, .label = "Show Columns", .hasSubmenu = true,
       .submenuEntries = std::move(columns), .actionGlyph = "columns-3"},
      {.separator = true},
      filemanager::fileSortByMenuEntry(m_sortField, m_sortOrder, m_foldersFirst, m_hiddenLast),
      {.id = 50, .label = "Show Hidden Files", .checkmark = true, .toggleState = m_showHidden ? 1 : 0},
      {.separator = true},
      {.id = kZoomIn, .label = "Zoom In", .enabled = !m_renamePath && m_viewScale < filemanager::kFileViewMaxScale,
       .shortcut = "Ctrl++", .actionGlyph = "zoom-in"},
      {.id = kZoomOut, .label = "Zoom Out", .enabled = !m_renamePath && m_viewScale > filemanager::kFileViewMinScale,
       .shortcut = "Ctrl+-", .actionGlyph = "zoom-out"},
      {.id = kResetZoom, .label = "Reset Zoom", .enabled = !m_renamePath && m_viewScale != filemanager::kFileViewDefaultScale,
       .shortcut = "Ctrl+0", .actionGlyph = "zoom-reset"},
      {.separator = true},
      {.id = 13, .label = "Refresh", .shortcut = "Ctrl+R", .actionGlyph = "refresh"},
      {.id = kShowMenuBar, .label = "Show Menu Bar", .checkmark = true, .toggleState = m_showApplicationMenuBar ? 1 : 0},
  };
}

bool FileManager::openApplicationMenu(float x, float y, std::size_t menuIndex) {
  if (!m_contextMenuPopup || !m_surface)
    return false;
  std::vector<ContextMenuControlEntry> entries;
  switch (menuIndex) {
  case 0:
    entries = fileMenuEntries();
    break;
  case 1:
    entries = editMenuEntries();
    break;
  case 2:
    entries = viewMenuEntries();
    break;
  default:
    return false;
  }
  m_contextMenuTarget.reset();
  if (m_applicationMenuCollapsed && menuIndex == 0) {
    entries.insert(entries.end(), {
        {.separator = true},
        {.id = kCompactEditMenu, .label = "Edit", .hasSubmenu = true, .submenuEntries = editMenuEntries(),
         .attachSubmenuContent = true},
        {.id = kCompactViewMenu, .label = "View", .hasSubmenu = true, .submenuEntries = viewMenuEntries(),
         .attachSubmenuContent = true},
    });
  }
  const auto output = m_wayland.outputForSurface(m_surface->wlSurface());
  m_dispatcher.pointerLeave();
  TooltipManager::instance().forceDestroy();
  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = 240.0F,
          .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
          .parent =
              PopupSurfaceParent{
                  .xdgSurface = m_surface->xdgSurface(), .output = output, .wlSurface = m_surface->wlSurface()
              },
          .inputSerial = m_lastInputSerial
      }
  );
  if (m_contextMenuPopup->isOpen())
    m_applicationMenuOpen = menuIndex;
  updateApplicationMenuUnderline();
  return m_contextMenuPopup->isOpen();
}

void FileManager::launchNewWindow(const std::filesystem::path& path) {
  std::error_code pathError;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", pathError);
  if (pathError) {
    setStatus("Could not locate the Files executable");
    return;
  }
  const std::string executableText = executable.string();
  const std::string pathText = path.string();
  gchar* argv[] = {const_cast<gchar*>(executableText.c_str()), const_cast<gchar*>(pathText.c_str()), nullptr};
  GError* error = nullptr;
  if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
    setStatus(error ? error->message : "Could not open a new window");
    g_clear_error(&error);
  }
}

void FileManager::restoreSelection(bool chooseFirst) {
  if (m_grid == nullptr)
    return;
  const auto setGridSelection = [this](std::optional<std::size_t> index) {
    m_restoringSelection = true;
    // Metadata/monitor refreshes before the next layout must not cancel the
    // paste's pending reveal in VirtualGridView.
    const bool reveal = index && m_pendingPasteRevealPath && m_selectedPath == m_pendingPasteRevealPath
        && !m_pendingDropScrollGeneration;
    m_grid->setSelectedIndex(index, reveal);
    m_restoringSelection = false;
  };
  if (m_selectedPath.has_value()) {
    const auto selected = std::ranges::find(m_entries, *m_selectedPath, &FileEntry::absPath);
    if (selected != m_entries.end()) {
      setGridSelection(static_cast<std::size_t>(selected - m_entries.begin()));
      return;
    }
    if (m_loading)
      return;
    m_selectedPath.reset();
  }
  if (chooseFirst && !m_entries.empty()) {
    setGridSelection(0);
    return;
  }
  setGridSelection(std::nullopt);
}

void FileManager::activateSelected() {
  if (m_grid == nullptr)
    return;
  const auto selected = m_grid->selectedIndex();
  if (!selected || *selected >= m_entries.size())
    return;
  activatePath(m_entries[*selected].absPath);
}

void FileManager::activatePath(const std::filesystem::path& path) {
  const auto found = std::ranges::find(m_entries, path, &FileEntry::absPath);
  if (found == m_entries.end())
    return;
  const auto entry = *found;
  if (entry.isDir) {
    navigate(entry.absPath);
    return;
  }
  if (m_browsingTrash && !m_trashUris.contains(entry.absPath.string())) {
    setStatus("This Wastebin item is no longer available; refresh the view");
    return;
  }
  GFile* file = m_browsingTrash ? g_file_new_for_uri(m_trashUris.at(entry.absPath.string()).c_str())
                                : g_file_new_for_path(entry.absPath.c_str());
  char* uri = g_file_get_uri(file);
  g_app_info_launch_default_for_uri_async(
      uri, nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<FileManager*>(data);
        GError* error = nullptr;
        if (!g_app_info_launch_default_for_uri_finish(result, &error))
          self->setStatus(error == nullptr ? "Could not open this file" : error->message);
        else
          self->setStatus("");
        g_clear_error(&error);
      },
      this
  );
  g_free(uri);
  g_object_unref(file);
}

void FileManager::activatePointerItem(std::size_t index) {
  if (!m_grid || index >= m_entries.size())
    return;
  m_grid->setSelectedIndex(index, false);
  const std::uint32_t elapsed = m_pointerClickTime - m_lastPointerClickTime;
  if (m_lastPointerIndex == index && elapsed <= 500U) {
    m_lastPointerIndex.reset();
    activateSelected();
    return;
  }
  m_lastPointerIndex = index;
  m_lastPointerClickTime = m_pointerClickTime;
}

void FileManager::setStatus(std::string text) {
  m_statusMessage = std::move(text);
  m_statusMessageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  updateStatusBar();
  m_statusMessageTimer.stop();
  if (!m_statusMessage.empty())
    m_statusMessageTimer.start(std::chrono::seconds(5), [this] {
      m_statusMessage.clear();
      updateStatusBar();
      if (m_surface)
        m_surface->requestRedraw();
    });
}

void FileManager::updateSelectionSummary() { updateStatusBar(); }

void FileManager::updateStatusBar() {
  if (!m_statusLabel)
    return;
  std::string primary;
  std::string details;
  std::string freeSpace;
  if (!m_statusMessage.empty() && std::chrono::steady_clock::now() < m_statusMessageUntil) {
    primary = m_statusMessage;
  } else if (m_selectedPaths.size() == 1) {
    const std::filesystem::path path(*m_selectedPaths.begin());
    const auto entry = std::ranges::find(m_entries, path, &FileEntry::absPath);
    const std::string name = entry == m_entries.end() ? path.filename().string() : entry->name;
    std::error_code error;
    const auto linkStatus = std::filesystem::symlink_status(path, error);
    const bool link = !error && std::filesystem::is_symlink(linkStatus);
    primary = name;
    std::string type = queryFileTypeDescription(path);
    std::string target;
    if (link) {
      const auto suffix = type.find(" (link to ");
      if (suffix != std::string::npos) {
        target = type.substr(suffix + 10);
        if (!target.empty() && target.back() == ')')
          target.pop_back();
        type.resize(suffix);
      } else if (type.ends_with(" (symbolic link)")) {
        type.resize(type.size() - std::string_view(" (symbolic link)").size());
      }
      type += " (symbolic link)";
    }
    details = type;
    if (!target.empty())
      details += "  |  Target: " + target;
    if (!error && ((link) || std::filesystem::is_regular_file(linkStatus))) {
      std::uint64_t size = 0;
      bool sizeAvailable = false;
      if (link) {
        struct stat linkInfo{};
        if (::lstat(path.c_str(), &linkInfo) == 0) {
          size = static_cast<std::uint64_t>(linkInfo.st_size);
          sizeAvailable = true;
        }
      } else {
        size = std::filesystem::file_size(path, error);
        sizeAvailable = !error;
      }
      if (sizeAvailable) {
        const auto formatted = compactSize(size);
        std::ostringstream out;
        out
            << std::fixed
            << std::setprecision(formatted.value == std::round(formatted.value) ? 0 : 1)
            << formatted.value;
        details += "  |  " + out.str() + " " + formatted.unit;
      }
    }
  } else if (!m_selectedPaths.empty()) {
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    bool hasSize = false;
    for (const auto& selected : m_selectedPaths) {
      const auto found = std::ranges::find(m_entries, std::filesystem::path(selected), &FileEntry::absPath);
      if (found == m_entries.end())
        continue;
      if (found->isDir)
        ++folders;
      else {
        ++files;
        bytes += found->size;
        hasSize = true;
      }
    }
    primary = std::to_string(m_selectedPaths.size()) + " selected";
    details = std::to_string(folders) + " folders, " + std::to_string(files) + " files";
    if (hasSize) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += "  |  " + out.str() + " " + size.unit;
    }
  } else {
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : m_entries) {
      if (entry.isDir)
        ++folders;
      else {
        ++files;
        bytes += entry.size;
      }
    }
    primary = std::to_string(folders) + " folders";
    details = std::to_string(files) + " files";
    if (files) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += " · " + out.str() + " " + size.unit;
    }
    struct statvfs capacity{};
    if (!m_browsingTrash && ::statvfs(m_path.c_str(), &capacity) == 0) {
      const auto free = static_cast<std::uint64_t>(capacity.f_bavail) * capacity.f_frsize;
      const auto size = compactSize(free);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      freeSpace = out.str() + " " + size.unit + " free";
    }
  }
  m_statusLabel->setText(primary);
  if (m_statusDetailLabel)
    m_statusDetailLabel->setText(details.empty() ? "" : "  |  " + details);
  if (m_statusFreeLabel)
    m_statusFreeLabel->setText(freeSpace);
  m_statusLabel->setTooltip("");
}

void FileManager::updateNavigationControls() {
  if (m_backButton != nullptr)
    m_backButton->setEnabled(!m_backHistory.empty());
  if (m_forwardButton != nullptr)
    m_forwardButton->setEnabled(!m_forwardHistory.empty());
  if (m_upButton != nullptr)
    m_upButton->setEnabled(
        m_path.has_parent_path()
        && m_path.parent_path() != m_path
        && (!m_browsingTrash || m_path != std::filesystem::path("trash:/"))
    );
  bool selected = false;
  for (const auto& [button, path] : m_placeButtons) {
    const bool match = !selected && path == m_path;
    button->setSelected(match);
    selected = selected || match;
  }
  syncSortControls();
}

void FileManager::persistSavedPlaces() {
  std::vector<std::string> records;
  for (const auto& saved : m_savedPlaces) {
    records.push_back(saved.id);
    records.push_back(saved.path.string());
    records.push_back(saved.label);
  }
  if (!m_configService.setStateStringArray("files", "saved_places", records))
    setStatus("Unable to save Places");
  handlePlacesChanged();
}

void FileManager::textPrompt(
    std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
) {
  if (m_dialogSurface) {
    response(std::nullopt);
    return;
  }
  m_textPrompt = true;
  m_passwordPrompt = password;
  showDialog(
      std::move(title), std::move(message), {{"Cancel", 0}, {"Continue", 1}},
      [this, response = std::move(response)](int result) {
        auto value = m_dialogAlternativeName;
        m_dialogAlternativeName.clear();
        m_textPrompt = false;
        m_passwordPrompt = false;
        response(result == 1 ? std::optional<std::string>(std::move(value)) : std::nullopt);
      },
      nullptr, false, true
  );
}

void FileManager::savePlace(const std::filesystem::path& requested) {
  if (m_browsingTrash || requested.empty() || !requested.is_absolute())
    return;
  std::error_code error;
  if (!std::filesystem::is_directory(requested, error)) {
    setStatus("This folder is unavailable");
    return;
  }
  // Keep the user's link spelling; canonicalisation is only for duplicate checks.
  const auto path = requested.lexically_normal();
  const auto target = std::filesystem::canonical(path, error);
  if (std::ranges::any_of(m_placeRows, [&](const auto& row) {
        std::error_code ec;
        return row.second.personal
            && (row.second.path == path || std::filesystem::canonical(row.second.path, ec) == target);
      })) {
    setStatus("This folder is already saved in Places");
    return;
  }
  const auto label = path.filename().empty() ? path.string() : path.filename().string();
  gchar* uuid = g_uuid_string_random();
  m_savedPlaces.push_back({uuid, path, label});
  g_free(uuid);
  persistSavedPlaces();
}

std::optional<std::string>
FileManager::placeInsertionAt(float x, float y, std::string_view group, bool edgesOnly) const {
  if (!m_placesScroll)
    return std::nullopt;
  float vx = 0, vy = 0;
  Node::absolutePosition(m_placesScroll, vx, vy);
  if (x < vx || x >= vx + m_placesScroll->width() || y < vy || y >= vy + m_placesScroll->height())
    return std::nullopt;
  // Lines exist before every row and after each group. Their small hit regions
  // distinguish bookmark insertion from dropping files onto a folder's centre.
  const float strip = edgesOnly ? 7.0F : 100000.0F;
  float distance = strip;
  std::optional<std::string> result;
  for (const auto& [id, line] : m_placeInsertionLines) {
    bool visible = true;
    for (const Node* node = line; node; node = node->parent())
      if (!node->visible()) {
        visible = false;
        break;
      }
    if (!visible)
      continue;
    const auto row = std::ranges::find_if(m_placeRows, [&](const auto& entry) { return entry.second.identity == id; });
    if (id != "end:" + std::string(group)
        && (row == m_placeRows.end() || filemanager::placeGroup(row->second) != group))
      continue;
    float lx = 0, ly = 0;
    Node::absolutePosition(line, lx, ly);
    // Reordering remains within its own group's bounds, including capacity text.
    if (x < lx || x >= lx + line->width())
      continue;
    const float delta = std::abs(y - (ly + line->height() * 0.5F));
    if (delta <= distance) {
      distance = delta;
      result = id;
    }
  }
  if (!edgesOnly && result) {
    float first = 100000.0F, last = -100000.0F;
    for (const auto& [id, line] : m_placeInsertionLines) {
      const auto row =
          std::ranges::find_if(m_placeRows, [&](const auto& entry) { return entry.second.identity == id; });
      if (id == "end:" + std::string(group)
          || (row != m_placeRows.end() && filemanager::placeGroup(row->second) == group)) {
        float lx = 0, ly = 0;
        Node::absolutePosition(line, lx, ly);
        first = std::min(first, ly);
        last = std::max(last, ly + line->height());
      }
    }
    if (y < first - 4.0F || y > last + 4.0F)
      return std::nullopt;
  }
  return result;
}

void FileManager::showPlaceInsertion(const std::optional<std::string>& before) {
  for (const auto& [id, line] : m_placeInsertionLines)
    line->setFill(before && *before == id ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec());
}

void FileManager::reorderPlace(const std::string& identity, const std::string& before) {
  std::vector<filemanager::Place> places;
  for (const auto& [row, place] : m_placeRows)
    places.push_back(place);
  auto order = m_configService.stateStringArray("files", "place_order").value_or(std::vector<std::string>{});
  order = filemanager::reorderedPlaces(places, std::move(order), identity, before);
  if (!m_configService.setStateStringArray("files", "place_order", order))
    setStatus("Unable to save Places order");
  handlePlacesChanged();
}

void FileManager::pinFolders(const std::vector<std::string>& paths, const std::string& before) {
  std::vector<filemanager::Place> places;
  for (const auto& [row, place] : m_placeRows)
    places.push_back(place);
  if (before != "end:Personal"
      && std::ranges::none_of(places, [&](const auto& place) { return place.personal && place.identity == before; }))
    return;
  auto order = m_configService.stateStringArray("files", "place_order").value_or(std::vector<std::string>{});
  bool added = false;
  for (const auto& requested : paths) {
    std::error_code ec;
    const auto path = std::filesystem::path(requested).lexically_normal();
    if (!path.is_absolute() || !std::filesystem::is_directory(path, ec))
      continue;
    const auto target = std::filesystem::canonical(path, ec);
    if (ec || std::ranges::any_of(places, [&](const auto& place) {
          std::error_code error;
          return place.personal && (place.path == path || std::filesystem::canonical(place.path, error) == target);
        }))
      continue;
    gchar* uuid = g_uuid_string_random();
    const std::string id(uuid);
    g_free(uuid);
    const std::string label = path.filename().empty() ? path.string() : path.filename().string();
    m_savedPlaces.push_back({id, path, label});
    places.push_back({.label = label, .identity = "saved:" + id, .path = path, .personal = true});
    order = filemanager::reorderedPlaces(places, std::move(order), "saved:" + id, before);
    added = true;
  }
  if (added) {
    if (!m_configService.setStateStringArray("files", "place_order", order))
      setStatus("Unable to save Places order");
    persistSavedPlaces();
    setStatus("Folders pinned to Places");
  } else
    setStatus("No new folders to pin");
}

void FileManager::openPlaceMenu(const filemanager::Place& place, float x, float y) {
  if (!m_contextMenuPopup || !m_surface)
    return;
  m_placeMenuTarget = place;
  std::vector<ContextMenuControlEntry> entries;
  if (place.mounted)
    entries.push_back({.id = 30,
                       .label = "Open",
                       .actionIconNames = {"folder-open-symbolic", "folder-open"},
                       .actionGlyph = "folder-open"});
  if (place.identity == "personal:trash")
    entries.push_back({.id = 72, .label = "Empty Wastebin…"});
  if (place.identity != "personal:trash") {
    entries.push_back(
        {.id = 39,
         .label = "Properties",
         .actionIconNames = {"document-properties-symbolic", "document-properties"},
         .actionGlyph = "file-info"}
    );
    if (place.mounted && !place.path.empty())
      entries.push_back(
          {.id = 40,
           .label = "Open in New Window",
           .actionIconNames = {"window-new-symbolic", "window-new"},
           .actionGlyph = "app-window"}
      );
  }
  if (place.identity.starts_with("saved:"))
    entries.push_back(
        {.id = 31,
         .label = "Remove from Places",
         .actionIconNames = {"bookmark-remove-symbolic", "bookmark-remove"},
         .actionGlyph = "bookmark-minus"}
    );
  else if (place.mounted && !place.personal && !place.path.empty())
    entries.push_back(
        {.id = 32,
         .label = "Add to Places",
         .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
         .actionGlyph = "bookmark-plus"}
    );
  if (!m_deviceOperations.contains(place.identity)) {
    if (place.canMount)
      entries.push_back({.id = 33, .label = "Mount"});
    if (place.canUnmount)
      entries.push_back({.id = 34, .label = "Unmount"});
    if (place.canEject)
      entries.push_back({.id = 35, .label = "Eject"});
  }
  const auto hidden = m_configService.stateStringArray("files", "hidden_mounts").value_or(std::vector<std::string>{});
  if (!place.personal) {
    const bool isHidden = std::ranges::find(hidden, place.identity) != hidden.end();
    entries.push_back({.id = isHidden ? 37 : 36, .label = isHidden ? "Show in Places" : "Hide from Places"});
  }
  if (!hidden.empty() || m_showHiddenMounts)
    entries.push_back(
        {.id = 38, .label = "Show hidden mounts", .checkmark = true, .toggleState = m_showHiddenMounts ? 1 : 0}
    );
  filemanager::groupFileMenuEntries(entries, filemanager::FileMenuContext::Places);
  showPlacesMenu(std::move(entries), x, y);
}

void FileManager::setPlaceHidden(const filemanager::Place& place, bool hidden) {
  if (place.personal || place.identity.empty())
    return;
  auto identities = m_configService.stateStringArray("files", "hidden_mounts").value_or(std::vector<std::string>{});
  std::erase(identities, place.identity);
  if (hidden)
    identities.push_back(place.identity);
  if (!m_configService.setStateStringArray("files", "hidden_mounts", identities)) {
    setStatus("Unable to save hidden mounts");
    return;
  }
  setStatus(hidden ? "Hidden from Places" : "Shown in Places");
  m_placesDirty = true;
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::openPlacesVisibilityMenu(float x, float y) {
  m_placeMenuTarget.reset();
  showPlacesMenu(
      {{.id = 38, .label = "Show hidden mounts", .checkmark = true, .toggleState = m_showHiddenMounts ? 1 : 0}}, x, y
  );
}

void FileManager::showPlacesMenu(std::vector<ContextMenuControlEntry> entries, float x, float y) {
  if (!m_contextMenuPopup || !m_surface)
    return;
  if (entries.empty())
    return;
  const float menuHeight = static_cast<float>(entries.size()) * 40.0F + 20.0F;
  m_dispatcher.pointerLeave();
  TooltipManager::instance().forceDestroy();
  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .maxMenuWidth = static_cast<float>(m_surface->width()),
          .anchor =
              PopupAnchorRect{
                  static_cast<std::int32_t>(
                      std::clamp(x, 0.0F, std::max(0.0F, static_cast<float>(m_surface->width()) - 220.0F))
                  ),
                  static_cast<std::int32_t>(
                      std::clamp(y, 0.0F, std::max(0.0F, static_cast<float>(m_surface->height()) - menuHeight))
                  ),
                  1, 1
              },
          .parent =
              PopupSurfaceParent{
                  .xdgSurface = m_surface->xdgSurface(),
                  .output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output,
                  .wlSurface = m_surface->wlSurface()
              },
          .inputSerial = m_lastInputSerial
      }
  );
}

void FileManager::mountPassword(
    GMountOperation* operation, GAskPasswordFlags flags, std::string user, std::string domain, unsigned step
) {
  auto prompt = std::shared_ptr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(operation)), g_object_unref);
  if (step == 0 && (flags & G_ASK_PASSWORD_NEED_USERNAME)) {
    textPrompt(
        "Mount authentication", "Username (leave blank to use " + user + ")", false,
        [this, prompt, flags, user, domain](auto value) {
          if (!value) {
            g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
            return;
          }
          mountPassword(prompt.get(), flags, value->empty() ? user : *value, domain, 1);
        }
    );
    return;
  }
  if (step <= 1 && (flags & G_ASK_PASSWORD_NEED_DOMAIN)) {
    textPrompt(
        "Mount authentication", "Domain (leave blank to use " + domain + ")", false,
        [this, prompt, flags, user, domain](auto value) {
          if (!value) {
            g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
            return;
          }
          mountPassword(prompt.get(), flags, user, value->empty() ? domain : *value, 2);
        }
    );
    return;
  }
  g_mount_operation_set_username(operation, user.c_str());
  g_mount_operation_set_domain(operation, domain.c_str());
  g_mount_operation_set_password_save(operation, G_PASSWORD_SAVE_NEVER);
  if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
    textPrompt("Mount authentication", "Password", true, [prompt](auto value) {
      if (!value) {
        g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
        return;
      }
      g_mount_operation_set_password(prompt.get(), value->c_str());
      std::fill(value->begin(), value->end(), '\0');
      g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_HANDLED);
    });
  } else
    g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
}

void FileManager::operatePlace(const filemanager::Place& place, filemanager::PlaceOperation operation) {
  if (m_operations.replaying()) {
    setStatus("Wait for Undo or Redo to finish");
    return;
  }
  if (!m_deviceOperations.empty() || m_pendingClipboardReads || m_renamePath) {
    setStatus("Wait for the current device or clipboard operation");
    return;
  }
  // Eject may affect sibling volumes on the same physical device.
  if (operation != filemanager::PlaceOperation::Mount
      && ((operation == filemanager::PlaceOperation::Eject && m_operations.active())
          || (!place.path.empty() && m_operations.usesMount(place.path)))) {
    setStatus("Wait for transfers on this device to finish before removing it");
    return;
  }
  m_deviceOperations.insert(place.identity);
  setStatus("Working on " + place.label + "…");
  GMountOperation* prompt = g_mount_operation_new();
  m_mountPrompts.emplace_back(G_MOUNT_OPERATION(g_object_ref(prompt)), g_object_unref);
  g_signal_connect(
      prompt, "ask-password",
      G_CALLBACK((+[](GMountOperation* op, const char*, const char* user, const char* domain, GAskPasswordFlags flags,
                      gpointer data) {
        static_cast<FileManager*>(data)->mountPassword(op, flags, user ? user : "", domain ? domain : "", 0);
      })),
      this
  );
  g_signal_connect(
      prompt, "ask-question",
      G_CALLBACK((+[](GMountOperation* op, const char* message, const char* const* choices, gpointer data) {
        auto* self = static_cast<FileManager*>(data);
        auto retained = std::shared_ptr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(op)), g_object_unref);
        std::vector<std::pair<std::string, int>> actions;
        for (int i = 0; choices && choices[i]; ++i)
          actions.emplace_back(choices[i], i);
        self->showDialog(
            "Device operation", message ? message : "Choose an action", std::move(actions), [retained](int choice) {
              if (choice < 0)
                g_mount_operation_reply(retained.get(), G_MOUNT_OPERATION_ABORTED);
              else {
                g_mount_operation_set_choice(retained.get(), choice);
                g_mount_operation_reply(retained.get(), G_MOUNT_OPERATION_HANDLED);
              }
            }
        );
      })),
      this
  );
  // Busy-device requests never offer to force removal or terminate processes.
  g_signal_connect(
      prompt, "show-processes",
      G_CALLBACK((+[](GMountOperation* op, const char*, GArray*, const char* const*, gpointer) {
        g_object_set_data(G_OBJECT(op), "nythera-busy", GINT_TO_POINTER(1));
        g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
      })),
      nullptr
  );
  auto alive = m_alive;
  filemanager::PlacesModel::operate(
      place, operation, prompt, nullptr, [this, alive, prompt, identity = place.identity](std::string error) {
        if (!*alive)
          return;
        if (g_object_get_data(G_OBJECT(prompt), "nythera-busy"))
          error = "This device is busy; close files and wait for other applications before trying again";
        std::erase_if(m_mountPrompts, [prompt](const auto& retained) { return retained.get() == prompt; });
        m_deviceOperations.erase(identity);
        setStatus(error.empty() ? "Device operation completed" : error);
        handlePlacesChanged();
      }
  );
  g_object_unref(prompt);
}

void FileManager::setGridMode(bool gridMode) {
  if (m_gridMode == gridMode)
    return;
  cancelFileHeaderGesture();
  m_gridMode = gridMode;
  if (m_gridMode)
    clearFolderCounts();
  updateFileColumnHeaders();
  persistFileViewState();
  applyViewScale();
}

void FileManager::adjustViewScale(float value) {
  if (!m_grid || m_renamePath || m_resizingFileColumn || m_fileColumnDrag)
    return;
  const float scale = filemanager::clampFileViewScale(value);
  if (scale == m_viewScale)
    return;
  m_viewScale = scale;
  applyViewScale();
  if (!m_configService.setStateString("files", "view_scale", std::to_string(m_viewScale)))
    setStatus("Unable to save file view zoom");
}

void FileManager::applyViewScale() {
  if (!m_grid)
    return;
  cancelFileGesture();
  const auto metrics = filemanager::fileViewMetrics(m_viewScale);
  m_grid->setAdapter(nullptr);
  m_grid->setAdapter(m_adapter.get());
  m_grid->setColumns(m_gridMode ? 0 : 1);
  m_grid->setMinCellWidth(metrics.gridCellWidth);
  m_grid->setCellHeight(m_gridMode ? metrics.gridCellHeight : metrics.listRowHeight);
  m_grid->setRowGap(m_gridMode ? 3.0F : 1.0F);
  restoreSelection();
  if (m_surface)
    m_surface->requestLayout();
}

std::optional<std::filesystem::path> FileManager::dropDestination(float x, float y, bool incoming) const {
  if (!m_grid || m_browsingTrash)
    return std::nullopt;
  if (m_placesScroll) {
    float vx = 0, vy = 0;
    Node::absolutePosition(m_placesScroll, vx, vy);
    if (x >= vx && x < vx + m_placesScroll->width() && y >= vy && y < vy + m_placesScroll->height())
      for (const auto& [row, place] : m_placeRows) {
        if (!place.mounted || place.identity == "personal:trash")
          continue;
        bool visible = true;
        for (Node* node = row; node; node = node->parent())
          if (!node->visible()) {
            visible = false;
            break;
          }
        if (!visible)
          continue;
        float rx = 0, ry = 0;
        Node::absolutePosition(row, rx, ry);
        if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height())
          return place.path;
      }
  }
  float gx = 0, gy = 0;
  Node::absolutePosition(m_grid, gx, gy);
  if (x < gx || y < gy || x >= gx + m_grid->width() || y >= gy + m_grid->height())
    return std::nullopt;
  const auto index = m_grid->indexAtSurface(x, y);
  if (index && *index < m_entries.size()) {
    if (!m_entries[*index].isDir)
      return incoming ? std::optional(m_path) : std::nullopt;
    return m_entries[*index].absPath;
  }
  return m_path;
}

std::uint32_t FileManager::updateProtocolDrag(wl_surface* surface, float x, float y, std::uint32_t actions) {
  constexpr auto copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
  constexpr auto move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
  clearDragFeedback();
  m_pinBefore.reset();
  showPlaceInsertion(std::nullopt);
  m_dragTargetValid = false;
  m_protocolDropDestination.reset();
  m_protocolDropSources.clear();
  m_protocolDragInside = m_surface && surface == m_surface->wlSurface();
  if (!m_windowOpen
      || !m_surface
      || surface != m_surface->wlSurface()
      || !m_grid
      || m_browsingTrash
      || !m_deviceOperations.empty()
      || m_operations.replaying()
      || m_renamePath
      || m_loading) {
    if (m_surface)
      m_surface->requestUpdate();
    return 0;
  }
  m_dragPointerX = x;
  m_dragPointerY = y;
  const bool own = m_transfer.dragging() && !m_transfer.dragDropped() && !m_dragSources.empty();
  const bool folders = own && std::ranges::all_of(m_dragSources, [](const auto& source) {
                         std::error_code error;
                         return std::filesystem::is_directory(source, error);
                       });
  m_pinBefore = folders ? placeInsertionAt(x, y, "Personal", true) : std::nullopt;
  showPlaceInsertion(m_pinBefore);
  auto destination = m_pinBefore ? std::nullopt : dropDestination(x, y, !own);
  if (own && destination)
    destination = filemanager::checkedDropTarget(m_dragSources, *destination);
  if (!m_pinBefore && !destination) {
    m_surface->requestUpdate();
    return 0;
  }
  std::uint32_t action = copy;
  if (!m_pinBefore) {
    if (m_keyboardModifiers & KeyMod::Ctrl)
      action = copy;
    else if (m_keyboardModifiers & KeyMod::Shift)
      action = move;
    else {
      action = own && !filemanager::copyDropByDefault(m_dragSources, *destination) ? move : copy;
      if (!(actions & action))
        action = actions & copy ? copy : move;
    }
  }
  if (!(actions & action)) {
    m_surface->requestUpdate();
    return 0;
  }
  m_dragTargetValid = true;
  m_protocolDropDestination = destination;
  if (own)
    m_protocolDropSources = m_dragSources;
  if (auto index = m_grid->indexAtSurface(x, y); destination && index && m_entries[*index].isDir) {
    m_dropIndex = index;
    m_grid->notifyItemChanged(*index);
  }
  if (destination) {
    for (const auto& [row, place] : m_placeRows) {
      float rx = 0, ry = 0;
      Node::absolutePosition(row, rx, ry);
      if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height()) {
        m_dropPlace = row;
        row->setSelected(true);
        break;
      }
    }
  }
  setStatus(
      m_pinBefore ? "Pin folders in Places · Escape cancels"
                  : std::string(action == copy ? "Copy files to " : "Move files to ") + destination->string()
  );
  m_surface->requestUpdate();
  scheduleNativeDragPreview();
  return action;
}

NativeDataTransfer::DropReceiver FileManager::receiveProtocolDrop(std::uint32_t action) {
  m_protocolDragInside = false;
  if (m_surface)
    m_surface->requestUpdate();
  const auto pin = m_pinBefore;
  const auto destination = m_protocolDropDestination;
  if ((!pin && !destination) || !m_dragTargetValid)
    return {};
  const auto ownPaths = m_protocolDropSources;
  const auto navigation = m_navigationGeneration;
  const auto scrollGeneration = m_grid ? m_grid->scrollView().userScrollGeneration() : 0;
  return [this, pin, destination, ownPaths, navigation, scrollGeneration,
          action](std::optional<NativeDataTransfer::Payload> payload, NativeDataTransfer::FinishDrop finish) {
    if (!m_windowOpen || m_quitting || !payload || !payload->contains("text/uri-list") || !m_deviceOperations.empty()
        || m_operations.replaying()) {
      finish(false);
      return;
    }
    try {
      auto paths = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false).paths;
      if (paths.empty() || (!ownPaths.empty() && ownPaths != paths)) {
        finish(false);
        return;
      }
      if (pin) {
        const bool folders = std::ranges::all_of(paths, [](const auto& source) {
          std::error_code error;
          return std::filesystem::is_directory(source, error);
        });
        if (!folders || action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
          finish(false);
          return;
        }
        pinFolders(paths, *pin);
        finish(true);
      } else if (const auto target = filemanager::checkedDropTarget(paths, *destination)) {
        const auto id = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
            ? m_operations.move(std::move(paths), target->string())
            : m_operations.copy(std::move(paths), target->string());
        m_pasteRequests[id] = {target->string(), navigation, scrollGeneration};
        m_dropFinishes.emplace(id, std::move(finish));
      } else {
        setStatus("Cannot drop unavailable files or copy a folder into itself");
        finish(false);
      }
    } catch (const std::exception& error) {
      setStatus(std::string("Cannot drop files: ") + error.what());
      finish(false);
    }
  };
}
void FileManager::cancelFileGesture() {
  if (!m_transfer.dragDropped()) m_transfer.cancelDrag();
  clearNativeDragPreview();
  if (m_grid)
    m_grid->endRubberBand(true);
  m_dragPending = false;
  m_draggingFiles = false;
  m_dragPressSerial = 0;
  m_protocolDragInside = false;
  m_protocolDropDestination.reset();
  m_protocolDropSources.clear();
  m_dragIndex.reset();
  m_dragSources.clear();
  m_pinBefore.reset();
  m_reorderId.reset();
  m_reorderBefore.reset();
  m_reordering = false;
  if (m_dragPreview)
    m_dragPreview->setVisible(false);
  showPlaceInsertion(std::nullopt);
  clearDragFeedback();
  m_dispatcher.cancelPointerCapture();
}

void FileManager::clearNativeDragPreview() {
  ++m_nativePreviewGeneration;
  m_nativePreviewQueued = false;
  m_nativePreviewTarget.reset();
  if (m_nativePreviewSurface)
    wl_surface_destroy(m_nativePreviewSurface);
  m_nativePreviewSurface = nullptr;
}

void FileManager::scheduleNativeDragPreview() {
  if (!m_nativePreviewSurface || m_nativePreviewQueued || !m_transfer.dragging() || m_transfer.dragDropped())
    return;
  m_nativePreviewQueued = true;
  const auto generation = m_nativePreviewGeneration;
  auto alive = m_alive;
  DeferredCall::callLater([this, alive, generation] {
    if (!*alive || generation != m_nativePreviewGeneration)
      return;
    m_nativePreviewQueued = false;
    if (!m_surface || !m_nativePreviewSurface || !m_transfer.dragging() || m_transfer.dragDropped())
      return;
    try {
      if (!m_renderContext.makeCurrent(m_surface->renderTarget())) return;
      const bool canScale = wl_surface_get_version(m_nativePreviewSurface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
      const int scale = canScale
          ? std::clamp(static_cast<int>(std::ceil(m_surface->renderTarget().renderer().renderScale())), 1, 4)
          : 1;
      ScaledRenderer renderer(m_renderContext, static_cast<float>(scale));
      {
        UiPhaseScope layoutPhase(UiPhase::Layout);
        updateDragPreview(renderer, true);
      }
      if (!m_dragPreview)
        return;
      const auto width = static_cast<std::uint32_t>(std::ceil(m_dragPreview->width()));
      const auto height = static_cast<std::uint32_t>(std::ceil(m_dragPreview->height()));
      if (!width || !height)
        return;
      if (!m_nativePreviewTarget) {
        m_nativePreviewTarget = std::make_unique<RenderTarget>();
        m_nativePreviewTarget->create(m_nativePreviewSurface, m_renderContext);
      }
      if (canScale)
        wl_surface_set_buffer_scale(m_nativePreviewSurface, scale);
      m_nativePreviewTarget->setContentScale(static_cast<float>(scale));
      m_nativePreviewTarget->setLogicalSize(width, height);
      m_nativePreviewTarget->resize(
          width * static_cast<std::uint32_t>(scale), height * static_cast<std::uint32_t>(scale)
      );
      const auto previousX = m_dragPreview->x(), previousY = m_dragPreview->y();
      m_dragPreview->setPosition(0, 0);
      m_renderContext.renderScene(*m_nativePreviewTarget, m_dragPreview);
      m_dragPreview->setPosition(previousX, previousY);
      m_dragPreview->setVisible(false);
      m_renderContext.makeCurrent(m_surface->renderTarget());
    } catch (const std::exception& error) {
      cancelFileGesture();
      setStatus(std::string("Could not render drag preview: ") + error.what());
    }
  });
}

void FileManager::updateDragPreview(Renderer& renderer, bool nativeIcon) {
  if (!m_root)
    return;
  if (!m_draggingFiles
      || m_dragSources.empty()
      || (!nativeIcon
          && (m_transfer.dragIconVisible()
              || (m_transfer.dragging() && (!m_protocolDragInside || m_transfer.dragDropped()))))) {
    if (m_dragPreview)
      m_dragPreview->setVisible(false);
    return;
  }
  if (!m_dragPreview) {
    auto preview = std::make_unique<Flex>();
    preview->setParticipatesInLayout(false);
    preview->setHitTestVisible(false);
    preview->setExcludeSubtreeFromTabOrder(true);
    preview->setZIndex(1000);
    preview->setPadding(8.0F);
    preview->setGap(8.0F);
    preview->setAlign(FlexAlign::Center);
    constexpr float previewOpacity = 0.94F;
    preview->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, previewOpacity));
    applyNytheraBevel(*preview, Style::scaledRadiusMd(1.0F));
    auto icon = std::make_unique<FileIcon>();
    icon->setHitTestVisible(false);
    icon->setGlyphSize(36.0F);
    m_dragPreviewIcon = static_cast<FileIcon*>(preview->addChild(std::move(icon)));
    auto text = std::make_unique<Flex>();
    text->setDirection(FlexDirection::Vertical);
    text->setHitTestVisible(false);
    auto name = std::make_unique<Label>();
    name->setFontSize(Style::fontSizeBody);
    name->setMaxWidth(220.0F);
    name->setMaxLines(1);
    name->setColor(colorSpecFromRole(ColorRole::OnSurface));
    m_dragPreviewLabel = static_cast<Label*>(text->addChild(std::move(name)));
    auto action = std::make_unique<Label>();
    action->setFontSize(Style::fontSizeCaption);
    action->setColor(colorSpecFromRole(ColorRole::Primary));
    m_dragPreviewAction = static_cast<Label*>(text->addChild(std::move(action)));
    preview->addChild(std::move(text));
    m_dragPreview = static_cast<Flex*>(m_root->addChild(std::move(preview)));
  }
  const std::filesystem::path path(m_dragSources.front());
  std::error_code error;
  m_dragPreviewIcon->setFile(path, std::filesystem::is_directory(path, error));
  m_dragPreviewLabel->setText(
      m_dragSources.size() == 1 ? path.filename().string() : std::to_string(m_dragSources.size()) + " selected items"
  );
  const auto destination = dropDestination(m_dragPointerX, m_dragPointerY);
  const bool externalTarget = nativeIcon && m_transfer.dragging() && !m_protocolDragInside;
  const bool targetValid = externalTarget ? m_transfer.dragAction() != 0 : m_dragTargetValid;
  const bool copy = externalTarget ? m_transfer.dragAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                   : (m_keyboardModifiers & KeyMod::Ctrl)
          || (destination
              && !(m_keyboardModifiers & KeyMod::Shift)
              && filemanager::copyDropByDefault(m_dragSources, *destination));
  m_dragPreviewAction->setText(
      m_pinBefore        ? "Pin in Places"
          : !targetValid ? "Cannot drop here"
          : copy         ? "Copy here"
                         : "Move here"
  );
  constexpr float previewBorderOpacity = 0.85F;
  m_dragPreview->setBorder(
      colorSpecFromRole(targetValid ? ColorRole::Primary : ColorRole::OutlineVariant, previewBorderOpacity), 1.0F
  );
  m_dragPreview->setVisible(true);
  m_dragPreview->layout(renderer);
  float px = m_dragPointerX + 18.0F;
  float py = m_dragPointerY + 20.0F;
  if (px + m_dragPreview->width() > static_cast<float>(m_surface->width()))
    px = m_dragPointerX - m_dragPreview->width() - 18.0F;
  if (py + m_dragPreview->height() > static_cast<float>(m_surface->height()))
    py = m_dragPointerY - m_dragPreview->height() - 20.0F;
  m_dragPreview->setPosition(std::max(0.0F, px), std::max(0.0F, py));
}
void FileManager::clearDragFeedback() {
  if (m_dropPlace)
    m_dropPlace->setSelected(false);
  m_dropPlace = nullptr;
  auto old = m_dropIndex;
  m_dropIndex.reset();
  if (old && m_grid)
    m_grid->notifyItemChanged(*old);
  updateNavigationControls();
}

void FileManager::handlePlacesChanged() {
  m_placesDirty = true;
  m_placesRefreshCapacity = true;
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::rebuildPlacesSidebar() {
  m_placesDirty = false;
  if (m_placesContent == nullptr || m_placesModel == nullptr)
    return;
  const float scrollOffset = m_placesScroll ? m_placesScroll->scrollOffset() : 0.0F;
  auto places = m_placesModel->discover();
  if (!m_savedPlacesLoaded) {
    m_savedPlacesLoaded = true;
    auto stored = m_configService.stateStringArray("files", "saved_places");
    if (stored)
      for (std::size_t i = 0; i + 2 < stored->size(); i += 3) {
        const auto& path = (*stored)[i + 1];
        if (!path.empty() && std::filesystem::path(path).is_absolute())
          m_savedPlaces.push_back({(*stored)[i], path, (*stored)[i + 2]});
      }
  }
  places.push_back(
      {.label = "Wastebin",
       .identity = "personal:trash",
       .path = "trash:/",
       .iconNames = {"user-trash", "user-trash-symbolic"},
       .personal = true}
  );
  std::unordered_set<std::string> savedPaths;
  for (const auto& saved : m_savedPlaces) {
    if (!savedPaths.insert(saved.path.lexically_normal().string()).second)
      continue;
    places.push_back(
        {.label = saved.label,
         .identity = "saved:" + saved.id,
         .path = saved.path,
         .iconNames = {"folder", "folder-symbolic"},
         .personal = true}
    );
  }
  filemanager::orderPlaces(
      places, m_configService.stateStringArray("files", "place_order").value_or(std::vector<std::string>{})
  );
  bool currentMountRemoved = false;
  for (const auto& previous : m_placeCapacities) {
    const bool stillMounted = std::ranges::any_of(places, [&](const auto& place) {
      return place.mounted && !place.personal && place.path == previous.mountPath;
    });
    const auto relative = m_path.lexically_relative(previous.mountPath);
    if (!stillMounted && !relative.empty() && *relative.begin() != "..")
      currentMountRemoved = true;
  }
  std::unordered_set<std::string> liveCapacityKeys;
  for (const auto& place : places)
    if (place.mounted && !place.personal)
      liveCapacityKeys.insert(place.capacityIdentity.empty() ? place.identity : place.capacityIdentity);
  for (auto it = m_capacityQueries.begin(); it != m_capacityQueries.end();) {
    if (!liveCapacityKeys.contains(it->first)) {
      g_cancellable_cancel(it->second);
      g_object_unref(it->second);
      it = m_capacityQueries.erase(it);
    } else
      ++it;
  }
  while (!m_placesContent->children().empty())
    m_placesContent->removeChild(m_placesContent->children().front().get());
  m_dropPlace = nullptr;
  m_placeInsertionLines.clear();
  m_placeRows.clear();
  m_placeButtons.clear();
  m_placeCapacities.clear();
  const std::array<std::pair<const char*, const char*>, 3> groups{
      {{"Personal", "personal_expanded"}, {"Devices", "devices_expanded"}, {"Network", "network_expanded"}}
  };
  const auto hidden = m_configService.stateStringArray("files", "hidden_mounts").value_or(std::vector<std::string>{});
  for (const auto& [nameRaw, keyRaw] : groups) {
    const std::string name(nameRaw), key(keyRaw);
    const bool expanded = m_configService.stateBool("files", key).value_or(true);
    auto group = std::make_unique<Flex>();
    Flex* groupPtr = group.get();
    auto headingPtr = std::make_shared<Button*>(nullptr);
    auto heading = makeButton(
        name,
        [this, key, groupPtr, headingPtr] {
          const bool expand = !m_configService.stateBool("files", key).value_or(true);
          m_configService.setStateBool("files", key, expand);
          groupPtr->setVisible(expand);
          groupPtr->setParticipatesInLayout(expand);
          if (*headingPtr)
            (*headingPtr)->setGlyph(expand ? "chevron-down" : "chevron-right");
          if (m_surface)
            m_surface->requestLayout();
        },
        headingPtr.get()
    );
    *headingPtr = static_cast<Button*>(heading.get());
    (*headingPtr)->setGlyph(expanded ? "chevron-down" : "chevron-right");
    (*headingPtr)->setVariant(ButtonVariant::Ghost);
    (*headingPtr)->setContentAlign(ButtonContentAlign::Start);
    (*headingPtr)->setFontSize(Style::fontSizeBody * 1.02F);
    (*headingPtr)->setControlHeight(30.0F);
    if (name != "Personal")
      (*headingPtr)->setOnRightClickWithPointer([this](float x, float y, std::uint32_t serial, std::uint32_t) {
        m_lastInputSerial = serial;
        openPlacesVisibilityMenu(x, y);
      });
    m_placesContent->addChild(std::move(heading));
    group->setDirection(FlexDirection::Vertical);
    group->setAlign(FlexAlign::Stretch);
    group->setGap(2.0F);
    group->setVisible(expanded);
    group->setParticipatesInLayout(expanded);
    for (const auto& place : places) {
      if ((name == "Personal" && !place.personal)
          || (name == "Devices" && (place.personal || place.network))
          || (name == "Network" && !place.network))
        continue;
      const bool isHidden = !place.personal && std::ranges::find(hidden, place.identity) != hidden.end();
      if (isHidden && !m_showHiddenMounts)
        continue;
      std::string caption = place.label;
      if (isHidden)
        caption += " (hidden)";
      {
        auto line = std::make_unique<Flex>();
        line->setMinHeight(2);
        line->setMaxHeight(2);
        line->setFill(clearColorSpec());
        m_placeInsertionLines.emplace_back(place.identity, line.get());
        group->addChild(std::move(line));
      }
      auto row = makeButton(caption, [this, place] {
        if (place.mounted)
          navigate(place.path);
      });
      row->setOnRightClickWithPointer([this, place](float x, float y, std::uint32_t serial, std::uint32_t) {
        m_lastInputSerial = serial;
        openPlaceMenu(place, x, y);
      });
      row->setContentAlign(ButtonContentAlign::Start);
      row->setFocusHighlightEnabled(false);
      useSubtleSelection(*row, !place.mounted);
      row->setTooltip(filemanager::placeTooltip(place));
      if (!place.mounted) {
        row->label()->setFlexGrow(1.0F);
        auto disconnected = std::make_unique<FileIcon>();
        disconnected->setThemedIconNames({"network-offline-symbolic", "network-offline"});
        disconnected->setGlyphSize(14.0F);
        row->addChild(std::move(disconnected));
      }
      if (!place.iconNames.empty()) {
        auto icon = std::make_unique<FileIcon>();
        icon->setFile(place.path, true);
        icon->setThemedIconNames(place.iconNames);
        icon->setGlyphSize(22.0F);
        row->setLeadingIcon(std::move(icon), 22.0F);
      }
      if (place.mounted)
        m_placeButtons.emplace_back(row.get(), place.path);
      auto* rowPtr = static_cast<Button*>(group->addChild(std::move(row)));
      m_placeRows.emplace_back(rowPtr, place);
      if (place.mounted && !place.personal) {
        auto bar = std::make_unique<ProgressBar>();
        bar->setSize(std::max(80.0F, m_sidebarWidth - 28.0F), 4.0F);
        bar->setFillColor(colorSpecFromRole(ColorRole::Primary, 0.75F));
        bar->setTrackColor(colorSpecFromRole(ColorRole::SurfaceContainerHighest, 0.6F));
        bar->setVisible(false);
        bar->setParticipatesInLayout(false);
        auto* barPtr = static_cast<ProgressBar*>(group->addChild(std::move(bar)));
        auto label = std::make_unique<Label>();
        label->setFontSize(Style::fontSizeCaption);
        label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        label->setMaxLines(1);
        label->setEllipsize(TextEllipsize::End);
        auto* labelPtr = static_cast<Label*>(group->addChild(std::move(label)));
        const std::string capacityKey = place.capacityIdentity.empty() ? place.identity : place.capacityIdentity;
        m_placeCapacities.push_back({place.identity, capacityKey, place.path, labelPtr, barPtr, rowPtr});
        requestPlaceCapacity(m_placeCapacities.back(), m_placesRefreshCapacity);
      }
    }
    {
      auto line = std::make_unique<Flex>();
      line->setMinHeight(2);
      line->setMaxHeight(2);
      line->setFill(clearColorSpec());
      m_placeInsertionLines.emplace_back("end:" + name, line.get());
      group->addChild(std::move(line));
    }
    m_placesContent->addChild(std::move(group));
  }
  m_placesRefreshCapacity = false;
  updateNavigationControls();
  if (m_placesScroll)
    m_placesScroll->setScrollOffset(scrollOffset);
  if (currentMountRemoved)
    navigate(std::filesystem::path(g_get_home_dir()), false);
}

void FileManager::requestPlaceCapacity(const PlaceCapacityView& place, bool forceRefresh) {
  if (place.capacityIdentity.empty() || !place.freeSpaceLabel)
    return;
  const auto filesystemId = m_placeFilesystemIds.find(place.capacityIdentity);
  const std::string cacheKey = filesystemId != m_placeFilesystemIds.end() && !filesystemId->second.empty()
      ? "filesystem:" + filesystemId->second
      : place.capacityIdentity;
  auto cached = m_capacityCache.find(cacheKey);
  const bool fresh = cached != m_capacityCache.end()
      && std::chrono::steady_clock::now() - cached->second.updatedAt < std::chrono::seconds(30);
  auto render = [&](const CapacityInfo& info) {
    const auto entry =
        std::ranges::find_if(m_placeRows, [&](const auto& item) { return item.second.identity == place.identity; });
    if (entry != m_placeRows.end()) {
      const auto tooltip = filemanager::placeTooltip(entry->second, info.filesystemType, info.free, info.total);
      place.row->setTooltip(tooltip);
      place.freeSpaceLabel->setTooltip(tooltip);
    }
    if (info.total == 0) {
      place.freeSpaceLabel->setText("Capacity unavailable");
      place.bar->setVisible(false);
      place.bar->setParticipatesInLayout(false);
      return;
    }
    place.freeSpaceLabel->setText(compactCapacity(info.free, info.total));
    place.bar->setProgress(
        static_cast<float>(info.total - std::min(info.free, info.total)) / static_cast<float>(info.total)
    );
    place.bar->setVisible(true);
    place.bar->setParticipatesInLayout(true);
  };
  if (cached != m_capacityCache.end())
    render(cached->second);
  else
    place.freeSpaceLabel->setText("Checking capacity…");
  if (!forceRefresh && fresh)
    return;
  if (m_capacityQueries.contains(place.capacityIdentity))
    return;
  GCancellable* cancellable = g_cancellable_new();
  m_capacityQueries.emplace(place.capacityIdentity, cancellable);
  auto* query = new CapacityQuery{
      this, m_placesAlive, place.identity, place.capacityIdentity, G_CANCELLABLE(g_object_ref(cancellable))
  };
  filemanager::PlacesModel::queryCapacity(place.mountPath, cancellable, placeCapacityReady, query);
}

void FileManager::refreshPlaceCapacities() {
  for (const auto& place : m_placeCapacities)
    requestPlaceCapacity(place, true);
}

void FileManager::cancelPlaceCapacityQueries() {
  for (auto& [key, cancellable] : m_capacityQueries) {
    (void)key;
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
  }
  m_capacityQueries.clear();
}

void FileManager::placeCapacityReady(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<CapacityQuery> query(static_cast<CapacityQuery*>(data));
  GError* error = nullptr;
  GFileInfo* info = g_file_query_filesystem_info_finish(G_FILE(source), result, &error);
  const bool cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  if (error)
    g_error_free(error);
  if (query->alive.expired()) {
    if (query->cancellable)
      g_object_unref(query->cancellable);
    if (info)
      g_object_unref(info);
    return;
  }
  query->manager->completePlaceCapacityQuery(query->identity, query->requestKey, query->cancellable, info, cancelled);
  if (query->cancellable)
    g_object_unref(query->cancellable);
  if (info)
    g_object_unref(info);
}

void FileManager::completePlaceCapacityQuery(
    const std::string& identity, const std::string& requestKey, GCancellable* cancellable, GFileInfo* info,
    bool cancelled
) {
  auto pending = m_capacityQueries.find(requestKey);
  if (pending == m_capacityQueries.end() || pending->second != cancellable)
    return;
  g_object_unref(pending->second);
  m_capacityQueries.erase(pending);
  const bool hasView = std::ranges::any_of(m_placeCapacities, [&](const auto& item) {
    return item.identity == identity && item.capacityIdentity == requestKey;
  });
  if (!hasView || cancelled)
    return;
  CapacityInfo value;
  if (info != nullptr) {
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)
        && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
      value.total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      value.free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    }
    const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
    const char* fsid = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    if (type)
      value.filesystemType = type;
    if (fsid)
      value.filesystemId = fsid;
  }
  value.updatedAt = std::chrono::steady_clock::now();
  m_placeFilesystemIds[requestKey] = value.filesystemId;
  m_capacityCache[requestKey] = value;
  if (!value.filesystemId.empty())
    m_capacityCache["filesystem:" + value.filesystemId] = value;
  for (const auto& item : m_placeCapacities)
    if (item.capacityIdentity == requestKey)
      requestPlaceCapacity(item, false);
}

void FileManager::rebuildBreadcrumbs() {
  if (m_breadcrumbBar == nullptr)
    return;
  while (!m_breadcrumbBar->children().empty())
    m_breadcrumbBar->removeChild(m_breadcrumbBar->children().front().get());
  auto addCrumb = [this](std::string label, std::filesystem::path path, bool last) {
    if (!m_breadcrumbBar->children().empty()) {
      auto separator = std::make_unique<Label>();
      separator->setText("›");
      separator->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      separator->setFontSize(Style::fontSizeCaption);
      m_breadcrumbBar->addChild(std::move(separator));
    }
    auto button = std::make_unique<Button>();
    button->setText(label);
    button->setVariant(ButtonVariant::Ghost);
    button->setFocusHighlightEnabled(false);
    useSubtleSelection(*button);
    button->setSelected(false);
    if (last && button->label())
      button->label()->setFontWeight(FontWeight::SemiBold);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick([this, path = std::move(path)] { navigate(path); });
    m_breadcrumbBar->addChild(std::move(button));
  };
  const auto crumbs = filemanager::fileBreadcrumbs(m_path);
  for (std::size_t i = 0; i < crumbs.size(); ++i)
    addCrumb(crumbs[i].label, crumbs[i].path, i + 1 == crumbs.size());
  auto emptyField = std::make_unique<Button>();
  emptyField->setVariant(ButtonVariant::Ghost);
  emptyField->setFocusHighlightEnabled(false);
  emptyField->setControlHeight(kToolbarHeight);
  emptyField->setFlexGrow(1.0F);
  emptyField->setOnClick([this] { setLocationEditMode(true); });
  m_breadcrumbBar->addChild(std::move(emptyField));
}

void FileManager::setLocationEditMode(bool editing, bool preserveOutsideClickFocus) {
  ++m_locationEditGeneration;
  m_editingLocation = editing;
  if (m_locationInput != nullptr) {
    m_locationInput->setVisible(editing);
    m_locationInput->setParticipatesInLayout(editing);
    if (!editing)
      m_locationInput->setValue(m_path.string());
  }
  if (m_breadcrumbBar != nullptr) {
    m_breadcrumbBar->setVisible(!editing);
    m_breadcrumbBar->setParticipatesInLayout(!editing);
  }
  if (editing && m_locationInput != nullptr) {
    m_locationInput->setValue(m_path.string());
    m_dispatcher.setFocus(m_locationInput->inputArea());
    m_locationInput->selectAll();
  } else if (m_grid != nullptr) {
    if (!preserveOutsideClickFocus
        || m_dispatcher.focusedArea() == (m_locationInput ? m_locationInput->inputArea() : nullptr))
      m_dispatcher.setFocus(m_grid->focusArea());
  }
  if (m_surface != nullptr)
    m_surface->requestLayout();
}

void FileManager::deferCancelLocationEdit() {
  if (!m_editingLocation)
    return;
  const auto generation = m_locationEditGeneration;
  const std::weak_ptr<bool> alive = m_alive;
  DeferredCall::callLater([this, alive, generation] {
    const auto locked = alive.lock();
    if (locked
        && *locked
        && filemanager::locationEditCancelIsCurrent(m_editingLocation, m_locationEditGeneration, generation))
      setLocationEditMode(false, true);
  });
}

void FileManager::setSortField(FileDialogSortField field) {
  if (m_sortField == field) {
    toggleSortOrder();
    return;
  }
  m_sortField = field;
  persistFileViewState();
  sortEntries();
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
    restoreSelection();
  }
  updateNavigationControls();
  syncSortControls();
}

void FileManager::toggleSortOrder() {
  m_sortOrder =
      m_sortOrder == FileDialogSortOrder::Ascending ? FileDialogSortOrder::Descending : FileDialogSortOrder::Ascending;
  persistFileViewState();
  sortEntries();
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
    restoreSelection();
  }
  updateNavigationControls();
  syncSortControls();
}

void FileManager::syncSortControls() {
  for (auto column : filemanager::kFileColumns) {
    const auto index = columnIndex(column);
    if (auto* header = m_fileColumnHeaders[index]) {
      const bool selected = m_sortField == kColumnSortFields[index];
      auto text = std::string(filemanager::kFileColumnLabels[index]);
      if (selected)
        text += m_sortOrder == FileDialogSortOrder::Ascending ? "  ↑" : "  ↓";
      header->setText(text);
      header->setSelected(selected);
    }
  }
}

void FileManager::updateFileColumnHeaders() {
  if (!m_fileHeader)
    return;
  cancelFileHeaderGesture();
  m_visibleFileColumns = m_gridMode
      ? std::vector<FileColumn>(filemanager::kDefaultFileColumns.begin(), filemanager::kDefaultFileColumns.end())
      : m_fileColumnLayout.displayed();
  for (auto column : filemanager::kFileColumns) {
    const auto index = columnIndex(column);
    const bool visible = std::ranges::contains(m_visibleFileColumns, column);
    m_fileColumnHeaders[index]->setVisible(visible);
    m_fileColumnHeaders[index]->setParticipatesInLayout(visible);
    m_fileColumnMarkers[index]->setVisible(visible);
  }
  for (auto column : m_visibleFileColumns)
    m_fileHeader->addChild(m_fileHeader->removeChild(m_fileColumnHeaders[columnIndex(column)]));

  applyListColumnWidths();
  if (!m_gridMode && m_grid)
    m_grid->notifyDataChanged();
  m_surface->requestLayout();
}

void FileManager::persistFileColumnLayout() {
  if (!m_configService.setStateString("files", "list_column_layout",
                                      filemanager::serializeFileColumnLayout(m_fileColumnLayout)))
    setStatus("Unable to save file column layout");
}

bool FileManager::applyListColumnWidths() {
  if (!m_fileHeader || m_visibleFileColumns.empty())
    return false;
  float leftGutter = 0.0F;
  float rightGutter = 0.0F;
  if (!m_gridMode && m_grid && m_grid->focusArea()->width() > 0.0F) {
    float headerX = 0.0F, headerY = 0.0F, rowX = 0.0F, rowY = 0.0F;
    Node::absolutePosition(m_fileHeader, headerX, headerY);
    Node::absolutePosition(m_grid->focusArea(), rowX, rowY);
    leftGutter = std::max(0.0F, rowX - headerX);
    rightGutter = std::max(0.0F, m_fileHeader->width() - m_grid->focusArea()->width() - leftGutter);
  }
  const bool paddingChanged = std::abs(leftGutter - m_listHeaderLeftGutter) > 0.5F
      || std::abs(rightGutter - m_listHeaderRightGutter) > 0.5F;
  if (paddingChanged) {
    m_listHeaderLeftGutter = leftGutter;
    m_listHeaderRightGutter = rightGutter;
    m_fileHeader->setPadding(5.0F, Style::spaceSm + rightGutter, 5.0F, Style::spaceSm + leftGutter);
  }
  const auto fitted = filemanager::fitFileColumns(
      m_resizingFileColumn ? m_listColumns : m_preferredListColumns, listColumnAvailableWidth(), m_visibleFileColumns
  );
  bool changed = false;
  bool textInsetChanged = false;
  for (auto column : m_visibleFileColumns) {
    auto* header = m_fileColumnHeaders[columnIndex(column)];
    changed |= std::abs(fitted[column] - m_listColumns[column]) > 0.5F;
    header->setMinWidth(fitted[column]);
    header->setMaxWidth(fitted[column]);
    const bool name = column == FileColumn::Name;
    const float inset = name ? filemanager::fileViewMetrics(m_viewScale).listIcon + Style::spaceSm
        : filemanager::fileColumnTextInset(column == m_visibleFileColumns.back(), Style::spaceSm);
    const bool onLeft = name != Style::rtl();
    const float left = onLeft ? inset : 0.0F;
    const float right = onLeft ? 0.0F : inset;
    if (header->paddingLeft() != left || header->paddingRight() != right) {
      header->setPadding(0.0F, right, 0.0F, left);
      textInsetChanged = true;
    }
  }
  m_listColumns = fitted;
  if (changed && m_grid && !m_gridMode)
    m_grid->notifyDataChanged();
  return changed || paddingChanged || textInsetChanged;
}

void FileManager::positionFileColumnMarkers() {
  const bool rtl = Style::rtl();
  for (auto column : m_visibleFileColumns) {
    auto* header = m_fileColumnHeaders[columnIndex(column)];
    const bool trailing = column == m_visibleFileColumns.back();
    const float halfGap = trailing ? 0.0F : Style::spaceSm * 0.5F;
    const float edge = rtl ? header->x() - halfGap : header->x() + header->width() + halfGap;
    m_fileColumnMarkers[columnIndex(column)]->setPosition(edge - 1.0F, (m_fileHeader->height() - 20.0F) * 0.5F);
  }
}

void FileManager::cancelFileHeaderGesture() {
  const auto resize = m_resizingFileColumn;
  m_resizingFileColumn.reset();
  if (resize)
    m_fileColumnMarkers[columnIndex(*resize)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
  if (m_fileColumnHoverBoundary)
    m_fileColumnMarkers[columnIndex(*m_fileColumnHoverBoundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
  m_fileColumnHoverBoundary.reset();
  if (m_fileColumnDrag) {
    m_fileColumnHeaders[columnIndex(m_fileColumnDrag->column)]->setBorder(clearColorSpec(), 0.0F);
    m_fileColumnDropMarker->setVisible(false);
    m_fileColumnDrag.reset();
  }
  if (resize)
    applyListColumnWidths();
  if (m_surface)
    m_surface->requestLayout();
}

bool FileManager::handleFileHeaderPointer(const PointerEvent& event, float x, float y) {
  if (m_gridMode || !m_fileHeader || m_visibleFileColumns.empty()
      || m_dragPending || m_draggingFiles || m_transfer.dragging() || m_reorderId || m_resizingSidebar
      || m_resizingStatus || (m_grid && m_grid->rubberBandActive()))
    return false;

  const bool leftButton = event.type == PointerEvent::Type::Button && event.button == BTN_LEFT;
  const bool motion = event.type == PointerEvent::Type::Motion;
  const bool leave = event.type == PointerEvent::Type::Leave;
  const bool rtl = Style::rtl();
  float hx = 0.0F, hy = 0.0F;
  Node::absolutePosition(m_fileHeader, hx, hy);
  const bool inHeader = !leave && y >= hy - 2.0F && y <= hy + m_fileHeader->height() + 2.0F;
  const float available = listColumnAvailableWidth();
  if (m_resizingFileColumn) {
    const auto column = *m_resizingFileColumn;
    if (leave) {
      cancelFileHeaderGesture();
      return true;
    }
    if (m_pointerSerial)
      m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    const bool flexibleName = m_preferredListColumns[FileColumn::Name] <= 0.0F;
    if (motion) {
      const float delta = rtl ? m_fileColumnDragX - x : x - m_fileColumnDragX;
      m_listColumns = filemanager::resizeFileColumnBoundary(
          m_fileColumnStart, column, delta, available, m_visibleFileColumns, flexibleName
      );
      applyListColumnWidths();
      if (m_grid)
        m_grid->notifyDataChanged();
      m_surface->requestLayout();
      return true;
    }
    if (leftButton && !event.pressed) {
      if (available >= filemanager::fileColumnTotal(filemanager::kMinimumFileColumns, m_visibleFileColumns)
          && m_listColumns[column] != m_fileColumnStart[column]) {
        const bool keepNameFlexible = flexibleName && column != FileColumn::Name
            && filemanager::hasOptionalFileColumns(m_visibleFileColumns);
        for (auto visible : m_visibleFileColumns)
          if (!keepNameFlexible || visible != FileColumn::Name)
            m_preferredListColumns[visible] = m_listColumns[visible];
        if (!m_configService.setStateString("files", "list_column_widths",
                                            filemanager::serializeFileColumns(m_preferredListColumns)))
          setStatus("Unable to save file column widths");
      }
      cancelFileHeaderGesture();
      m_dispatcher.cancelPointerCapture();
      return true;
    }
    return false;
  }

  if (m_fileColumnDrag) {
    auto& drag = *m_fileColumnDrag;
    if (leave) {
      m_dispatcher.cancelPointerCapture();
      cancelFileHeaderGesture();
      return false;
    }
    if (leftButton && !event.pressed) {
      const auto column = drag.column;
      const auto destination = drag.destination;
      cancelFileHeaderGesture();
      if (!destination)
        return false; // A normal header click still goes through the button's sort action.
      if (inHeader && m_fileColumnLayout.move(column, *destination)) {
        updateFileColumnHeaders();
        persistFileColumnLayout();
      }
      return true;
    }
    if (motion && (drag.destination || std::hypot(x - drag.x, y - drag.y) > 5.0F)) {
      if (!drag.destination) {
        m_dispatcher.cancelPointerCapture();
        m_dispatcher.pointerLeave();
        m_fileColumnHeaders[columnIndex(drag.column)]->setBorder(colorSpecFromRole(ColorRole::Primary, 0.7F), 1.0F);
      }

      float edge = x - hx;
      std::size_t destination = 0;
      for (auto column : m_visibleFileColumns) {
        if (column == drag.column)
          continue;
        const auto* header = m_fileColumnHeaders[columnIndex(column)];
        const float center = hx + header->x() + header->width() * 0.5F;
        if (rtl ? x > center : x < center) {
          edge = rtl ? header->x() + header->width() + Style::spaceSm * 0.5F
                     : header->x() - Style::spaceSm * 0.5F;
          break;
        }
        edge = m_fileColumnMarkers[columnIndex(column)]->x() + 1.0F;
        ++destination;
      }
      drag.destination = destination;
      m_fileColumnDropMarker->setVisible(inHeader);
      m_fileColumnDropMarker->setPosition(edge - 1.0F, (m_fileHeader->height() - 20.0F) * 0.5F);
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
      m_surface->requestLayout();
      return true;
    }
    return false;
  }

  std::optional<FileColumn> boundary;
  if (inHeader && available >= filemanager::fileColumnTotal(filemanager::kMinimumFileColumns, m_visibleFileColumns))
    for (auto column : m_visibleFileColumns)
      if (std::abs(x - (hx + m_fileColumnMarkers[columnIndex(column)]->x() + 1.0F)) <= 6.0F) {
        boundary = column;
        break;
      }
  if ((motion || leave || event.type == PointerEvent::Type::Enter) && m_fileColumnHoverBoundary != boundary) {
    if (m_fileColumnHoverBoundary)
      m_fileColumnMarkers[columnIndex(*m_fileColumnHoverBoundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
    m_fileColumnHoverBoundary = boundary;
    if (boundary)
      m_fileColumnMarkers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.82F));
    m_surface->requestRedraw();
  }
  if (boundary && leftButton && event.pressed) {
    m_dispatcher.cancelPointerCapture();
    m_resizingFileColumn = boundary;
    m_fileColumnDragX = x;
    m_fileColumnStart = m_listColumns;
    m_fileColumnMarkers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary));
    return true;
  }
  if (boundary && (motion || event.type == PointerEvent::Type::Enter)) {
    m_dispatcher.pointerLeave();
    if (m_pointerSerial)
      m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    return true;
  }
  if (leftButton && event.pressed && inHeader && m_visibleFileColumns.size() > 1)
    for (auto column : m_visibleFileColumns)
      if (m_fileColumnHeaders[columnIndex(column)]->containsScenePoint(x, y)) {
        m_fileColumnDrag = FileColumnDrag{column, x, y, std::nullopt};
        break;
      }
  return false;
}

float FileManager::listColumnAvailableWidth() const {
  if (!m_fileHeader || m_visibleFileColumns.empty())
    return 0.0F;
  // The list's focus plane shares the resolved row width, including the scrollbar gutter.
  const float rowWidth = m_grid && !m_gridMode && m_grid->focusArea()->width() > 0.0F
      ? m_grid->focusArea()->width() : m_fileHeader->width();
  const float spacing = static_cast<float>(m_visibleFileColumns.size() + 1) * Style::spaceSm;
  return std::max(0.0F, rowWidth - spacing);
}

GFile* FileManager::browsingFile() const {
  if (!m_browsingTrash)
    return g_file_new_for_path(m_path.c_str());
  GFile* file = g_file_new_for_uri("trash:///");
  for (const auto& part : m_path) {
    if (part == "trash:" || part == "/" || part == ".")
      continue;
    GFile* child = g_file_get_child(file, part.c_str());
    g_object_unref(file);
    file = child;
  }
  return file;
}

void FileManager::monitorDirectory() {
  closeMonitor();
  // Trash is a GIO virtual namespace. Never monitor a filesystem path bearing
  // its URI token; explicit refresh enumerates the backend again.
  if (m_browsingTrash)
    return;
  GFile* file = g_file_new_for_path(m_path.c_str());
  GError* error = nullptr;
  m_monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
  g_object_unref(file);
  if (m_monitor != nullptr) {
    g_signal_connect(
        m_monitor, "changed",
        G_CALLBACK(+[](GFileMonitor* monitor, GFile* changedFile, GFile* otherFile, GFileMonitorEvent event,
                       gpointer data) {
          auto* self = static_cast<FileManager*>(data);
          if (monitor == self->m_monitor)
            self->handleMonitorEvent(changedFile, otherFile, event);
        }),
        this
    );
  }
  g_clear_error(&error);
}

void FileManager::handleMonitorEvent(GFile* file, GFile* otherFile, GFileMonitorEvent event) {
  if (file == nullptr)
    return;
  char* rawPath = g_file_get_path(file);
  char* rawOtherPath = otherFile == nullptr ? nullptr : g_file_get_path(otherFile);
  if (rawPath == nullptr) {
    g_free(rawOtherPath);
    return;
  }
  PendingMonitorChange change{
      .event = event,
      .path = std::filesystem::path(rawPath).lexically_normal(),
      .otherPath =
          rawOtherPath == nullptr ? std::filesystem::path{} : std::filesystem::path(rawOtherPath).lexically_normal(),
  };
  g_free(rawPath);
  g_free(rawOtherPath);
  if (change.path == m_path || change.path.parent_path() != m_path)
    return;
  if (m_loading || m_renamePath) {
    m_pendingMonitorChanges.push_back(std::move(change));
    return;
  }

  switch (change.event) {
  case G_FILE_MONITOR_EVENT_DELETED:
  case G_FILE_MONITOR_EVENT_MOVED_OUT:
    removeMonitoredFile(change.path);
    break;
  case G_FILE_MONITOR_EVENT_RENAMED:
  case G_FILE_MONITOR_EVENT_MOVED:
    if (!change.otherPath.empty() && change.otherPath.parent_path() == m_path)
      queryMonitoredFile(change.otherPath, change.path);
    else
      removeMonitoredFile(change.path);
    break;
  case G_FILE_MONITOR_EVENT_CREATED:
  case G_FILE_MONITOR_EVENT_MOVED_IN:
  case G_FILE_MONITOR_EVENT_CHANGED:
  case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
  case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    queryMonitoredFile(change.path);
    break;
  case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
  case G_FILE_MONITOR_EVENT_UNMOUNTED:
    setStatus("This folder is no longer available");
    break;
  }
}

void FileManager::applyPendingMonitorChanges() {
  auto changes = std::move(m_pendingMonitorChanges);
  m_pendingMonitorChanges.clear();
  for (const auto& change : changes) {
    GFile* file = g_file_new_for_path(change.path.c_str());
    GFile* otherFile = change.otherPath.empty() ? nullptr : g_file_new_for_path(change.otherPath.c_str());
    handleMonitorEvent(file, otherFile, change.event);
    g_object_unref(file);
    if (otherFile != nullptr)
      g_object_unref(otherFile);
  }
}

void FileManager::queryMonitoredFile(const std::filesystem::path& path, const std::filesystem::path& replacedPath) {
  ++m_pendingMonitorQueries;
  GFile* file = g_file_new_for_path(path.c_str());
  g_file_query_info_async(
      file,
      "standard::name,standard::type,standard::size,standard::content-type,time::modified,time::created,time::access",
      G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<MonitoredFileQuery> query(static_cast<MonitoredFileQuery*>(data));
        auto* self = query->manager;
        GError* error = nullptr;
        GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
        if (query->generation == self->m_enumerationGeneration) {
          --self->m_pendingMonitorQueries;
          if (self->m_pendingPasteRevealPath && self->m_surface)
            self->m_surface->requestLayout();
        }
        if (query->generation != self->m_enumerationGeneration || query->path.parent_path() != self->m_path) {
          if (info != nullptr)
            g_object_unref(info);
          g_clear_error(&error);
          return;
        }
        if (info == nullptr) {
          g_clear_error(&error);
          self->removeMonitoredFile(query->replacedPath.empty() ? query->path : query->replacedPath);
          return;
        }
        g_clear_error(&error);
        const char* name = g_file_info_get_name(info);
        if (name == nullptr || (!self->m_showHidden && name[0] == '.')) {
          g_object_unref(info);
          self->removeMonitoredFile(query->replacedPath.empty() ? query->path : query->replacedPath);
          return;
        }

        FileEntry entry = makeFileEntry(info, query->path);
        g_object_unref(info);
        auto oldEntry = std::ranges::find(self->m_entries, query->path, &FileEntry::absPath);
        const auto replacedEntry = query->replacedPath.empty()
            ? self->m_entries.end()
            : std::ranges::find(self->m_entries, query->replacedPath, &FileEntry::absPath);
        if (query->replacedPath.empty() && oldEntry != self->m_entries.end() && sameEntry(*oldEntry, entry))
          return;
        self->m_folderCounts.erase(query->path.string());
        if (!query->replacedPath.empty()) {
          self->m_folderCounts.erase(query->replacedPath.string());
          if (self->m_selectedPath == query->replacedPath)
            self->m_selectedPath = query->path;
          if (self->m_selectionAnchor == query->replacedPath)
            self->m_selectionAnchor = query->path;
          if (self->m_selectedPaths.erase(query->replacedPath.string()))
            self->m_selectedPaths.insert(query->path.string());
        }
        if (replacedEntry != self->m_entries.end())
          self->m_entries.erase(replacedEntry);
        oldEntry = std::ranges::find(self->m_entries, query->path, &FileEntry::absPath);
        if (oldEntry != self->m_entries.end())
          *oldEntry = std::move(entry);
        else
          self->m_entries.push_back(std::move(entry));

        self->sortEntries();
        if (self->m_grid != nullptr) {
          self->m_grid->notifyDataChanged();
          self->restoreSelection();
        }
        self->setStatus("");
        if (self->m_surface)
          self->m_surface->requestLayout();
      },
      new MonitoredFileQuery{this, m_enumerationGeneration, path, replacedPath}
  );
  g_object_unref(file);
}

void FileManager::removeMonitoredFile(const std::filesystem::path& path) {
  if (path.empty())
    return;
  const auto oldSize = m_entries.size();
  std::erase_if(m_entries, [&path](const FileEntry& entry) { return entry.absPath == path; });
  if (oldSize == m_entries.size())
    return;
  m_folderCounts.erase(path.string());
  if (m_selectedPath == path)
    m_selectedPath.reset();
  m_selectedPaths.erase(path.string());
  if (m_selectionAnchor == path)
    m_selectionAnchor.reset();
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
    restoreSelection();
  }
  setStatus(m_entries.empty() ? "This folder is empty" : "");
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::closeMonitor() {
  if (m_monitor != nullptr) {
    g_file_monitor_cancel(m_monitor);
    g_object_unref(m_monitor);
    m_monitor = nullptr;
  }
}

void FileManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  (void)needsUpdate;
  if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
    return;
  m_renderContext.makeCurrent(m_surface->renderTarget());
  if (m_placesDirty)
    rebuildPlacesSidebar();
  Renderer& renderer = m_surface->renderTarget().renderer();
  if (m_pickerThumbnails.uploadPending(renderer.textureManager()) && m_grid)
    m_grid->notifyDataChanged();
  m_adapter->setRenderer(&renderer);
  if (!m_root) {
    constexpr float pad = 8.0F;
    m_root = std::make_unique<Flex>();
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(3.0F);
    m_root->setPadding(pad);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.88F));
    m_root->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.72F), Style::borderWidth);
    applyNytheraBevel(*m_root, Style::scaledRadiusLg(1.0F));

    auto appBar = std::make_unique<Flex>();
    appBar->setAlign(FlexAlign::Center);
    appBar->setGap(3.0F);
    appBar->setMinHeight(34.0F);
    appBar->setPadding(Style::spaceSm, Style::spaceMd);
    appBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, 0.72F));
    appBar->setBorder(colorSpecFromRole(ColorRole::Primary, 0.38F), Style::borderWidth);
    applyNytheraBevel(*appBar, Style::scaledRadiusMd(1.0F));
    auto appName = std::make_unique<Flex>();
    appName->setAlign(FlexAlign::Center);
    appName->setGap(9.0F); // The display font's space advance is 360/1000 em at 25 px.
    auto appNameNythera = std::make_unique<Label>();
    appNameNythera->setText("NYTHERA");
    appNameNythera->setFontSize(25.0F);
    appNameNythera->setDirectDisplayFont(true);
    appNameNythera->setColor(colorSpecFromRole(ColorRole::OnSurface));
    appName->addChild(std::move(appNameNythera));
    auto appNameFiles = std::make_unique<Label>();
    appNameFiles->setText("FILES");
    appNameFiles->setFontSize(25.0F);
    appNameFiles->setDirectDisplayFont(true);
    appNameFiles->setColor(colorSpecFromRole(ColorRole::Primary));
    appName->addChild(std::move(appNameFiles));
    appBar->addChild(std::move(appName));
    auto appSpacer = std::make_unique<Flex>();
    appSpacer->setFlexGrow(1.0F);
    appBar->addChild(std::move(appSpacer));

    auto menuItems = std::make_unique<Flex>();
    menuItems->setAlign(FlexAlign::Center);
    menuItems->setGap(Style::spaceXs);
    m_appMenuItems = menuItems.get();
    auto fileMenu = makeButton(
        "File",
        [this] {
          float x = 0, y = 0;
          Node::absolutePosition(m_fileMenuButton, x, y);
          openApplicationMenu(x, y + m_fileMenuButton->height());
        },
        &m_fileMenuButton
    );
    fileMenu->setVariant(ButtonVariant::Tab);
    fileMenu->setPadding(0.0F, Style::spaceSm);
    m_applicationMenuButtons[0] = m_fileMenuButton;
    m_fileMenuButton->setOnEnter([this] {
      m_applicationMenuHovered = 0;
      updateApplicationMenuUnderline();
    });
    m_fileMenuButton->setOnLeave([this] {
      if (!m_applicationMenuOpen && m_applicationMenuHovered == 0) {
        m_applicationMenuHovered.reset();
        updateApplicationMenuUnderline();
      }
    });
    menuItems->addChild(std::move(fileMenu));
    const std::array<const char*, 4> menuNames{"Edit", "View", "Tools", "Help"};
    for (std::size_t i = 0; i < menuNames.size(); ++i) {
      auto item = std::make_unique<Button>();
      item->setText(menuNames[i]);
      item->setVariant(ButtonVariant::Tab);
      item->setFontSize(Style::fontSizeCaption);
      item->setControlHeight(kToolbarHeight);
      item->setPadding(0.0F, Style::spaceSm);
      m_applicationMenuButtons[i + 1] = item.get();
      if (i < 2) {
        item->setOnClick([this, index = i + 1] {
          const auto* menu = m_applicationMenuButtons[index];
          float x = 0, y = 0;
          Node::absolutePosition(menu, x, y);
          openApplicationMenu(x, y + menu->height(), index);
        });
      } else {
        item->setOnClick([] {});
      }
      item->setOnEnter([this, index = i + 1] {
        m_applicationMenuHovered = index;
        updateApplicationMenuUnderline();
      });
      item->setOnLeave([this, index = i + 1] {
        if (!m_applicationMenuOpen && m_applicationMenuHovered == index) {
          m_applicationMenuHovered.reset();
          updateApplicationMenuUnderline();
        }
      });
      menuItems->addChild(std::move(item));
    }
    auto underline = std::make_unique<Box>();
    underline->setFill(colorSpecFromRole(ColorRole::Primary));
    underline->setRadius(1.0F);
    underline->setFrameSize(0.0F, 2.0F);
    underline->setOpacity(0.0F);
    underline->setHitTestVisible(false);
    underline->setParticipatesInLayout(false);
    underline->setZIndex(1);
    m_applicationMenuUnderline = underline.get();
    menuItems->addChild(std::move(underline));
    appBar->addChild(std::move(menuItems));
    auto compactMenu = makeButton(
        "",
        [this] {
          float x = 0, y = 0;
          Node::absolutePosition(m_compactMenuButton, x, y);
          openApplicationMenu(x, y + m_compactMenuButton->height());
        },
        &m_compactMenuButton
    );
    m_compactMenuButton->setGlyph("menu-2");
    m_compactMenuButton->setTooltip("Open application menu");
    appBar->addChild(std::move(compactMenu));
    if (m_showApplicationMenuBar) {
      m_compactMenuButton->setVisible(false);
      m_compactMenuButton->setParticipatesInLayout(false);
    } else {
      m_appMenuItems->setVisible(false);
      m_appMenuItems->setParticipatesInLayout(false);
    }
    m_root->addChild(std::move(appBar));

    auto content = std::make_unique<Flex>();
    content->setAlign(FlexAlign::Stretch);
    content->setGap(2.0F);
    content->setFlexGrow(1.0F);

    auto sidebar = std::make_unique<Flex>();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->setGap(3.0F);
    sidebar->setPadding(7.0F);
    if (auto stored = m_configService.stateString("files", "sidebar_width")) {
      try {
        const float width = std::stof(*stored);
        if (std::isfinite(width))
          m_sidebarWidth = std::clamp(width, kSidebarMinWidth, kSidebarMaxWidth);
      } catch (...) {
      }
    }
    sidebar->setMinWidth(m_sidebarWidth);
    sidebar->setMaxWidth(m_sidebarWidth);
    sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, 0.62F));
    sidebar->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.42F), Style::borderWidth);
    applyNytheraBevel(*sidebar, Style::scaledRadiusMd(1.0F));
    auto placesScroll = std::make_unique<ScrollView>();
    placesScroll->setFlexGrow(1.0F);
    placesScroll->setScrollbarVisible(true);
    m_placesScroll = placesScroll.get();
    m_placesContent = placesScroll->content();
    m_placesContent->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(placesScroll));
    m_placesModel = std::make_unique<filemanager::PlacesModel>();
    m_placesModel->setChangedCallback([this] { handlePlacesChanged(); });
    rebuildPlacesSidebar();
    m_sidebar = static_cast<Flex*>(content->addChild(std::move(sidebar)));

    auto divider = std::make_unique<Flex>();
    divider->setMinWidth(5.0F);
    divider->setMaxWidth(5.0F);
    divider->setFill(clearColorSpec());
    divider->setHitTestVisible(true);
    m_sidebarDivider = static_cast<Flex*>(content->addChild(std::move(divider)));

    auto mainPane = std::make_unique<Flex>();
    mainPane->setDirection(FlexDirection::Vertical);
    mainPane->setAlign(FlexAlign::Stretch);
    mainPane->setGap(2.0F);
    mainPane->setFlexGrow(1.0F);
    if (auto stored = m_configService.stateString("files", "status_height")) {
      try {
        const float height = std::stof(*stored);
        if (std::isfinite(height))
          m_statusHeight = std::clamp(height, kStatusMinHeight, kStatusMaxHeight);
      } catch (...) {
      }
    }

    auto navigation = std::make_unique<Flex>();
    navigation->setAlign(FlexAlign::Center);
    navigation->setGap(2.0F);
    navigation->addChild(makeButton(
        "‹",
        [this] {
          if (m_backHistory.empty())
            return;
          auto path = m_backHistory.back();
          m_backHistory.pop_back();
          m_forwardHistory.push_back(m_path);
          navigate(path, false);
        },
        &m_backButton
    ));
    navigation->addChild(makeButton(
        "›",
        [this] {
          if (m_forwardHistory.empty())
            return;
          auto path = m_forwardHistory.back();
          m_forwardHistory.pop_back();
          m_backHistory.push_back(m_path);
          navigate(path, false);
        },
        &m_forwardButton
    ));
    navigation->addChild(makeButton(
        "↑",
        [this] {
          if (m_path.has_parent_path())
            navigate(m_path.parent_path());
        },
        &m_upButton
    ));

    auto locationBar = std::make_unique<Flex>();
    locationBar->setAlign(FlexAlign::Center);
    locationBar->setGap(2.0F);
    locationBar->setPadding(2.0F, 4.0F);
    locationBar->setFlexGrow(1.0F);
    locationBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.78F));
    locationBar->setBorder(colorSpecFromRole(ColorRole::Primary, 0.28F), Style::borderWidth);
    applyNytheraBevel(*locationBar, Style::scaledRadiusSm(1.0F));
    auto location = std::make_unique<Input>();
    m_locationInput = location.get();
    m_locationInput->setValue(m_path.string());
    m_locationInput->setPlaceholder("Enter a folder path");
    m_locationInput->setFlexGrow(1.0F);
    // This standalone file-manager process does not share the shell's global Input clipboard.
    // Route text copies through the same Wayland clipboard writer used by its other copy actions.
    m_locationInput->setOnCopyText([this](std::string text) {
      writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8");
    });
    m_locationInput->setVisible(false);
    m_locationInput->setParticipatesInLayout(false);
    m_locationInput->setOnSubmit([this](const std::string& value) {
      navigate(value);
      setLocationEditMode(false);
    });
    m_locationInput->setOnFocusLoss([this] { deferCancelLocationEdit(); });
    locationBar->addChild(std::move(location));
    auto crumb = std::make_unique<Flex>();
    crumb->setAlign(FlexAlign::Center);
    crumb->setGap(1.0F);
    crumb->setFlexGrow(1.0F);
    crumb->setClipChildren(true);
    crumb->setMinHeight(kToolbarHeight);
    m_breadcrumbBar = static_cast<Flex*>(locationBar->addChild(std::move(crumb)));
    navigation->addChild(std::move(locationBar));
    navigation->addChild(makeButton("↻", [this] { activateContextMenuEntry({.id = 13}); }, &m_refreshButton));
    mainPane->addChild(std::move(navigation));

    auto fileHeader = std::make_unique<Flex>();
    m_fileHeader = fileHeader.get();
    fileHeader->setAlign(FlexAlign::Center);
    fileHeader->setGap(Style::spaceSm);
    fileHeader->setPadding(5.0F, Style::spaceSm);
    const auto styleHeader = [](Button& button, ButtonContentAlign alignment) {
      auto headerPalette = Button::defaultPalette(ButtonVariant::Ghost);
      headerPalette.normal = {clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurfaceVariant)};
      headerPalette.hover = {
          colorSpecFromRole(ColorRole::Hover, 0.35F), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)
      };
      headerPalette.pressed = headerPalette.hover;
      headerPalette.selected =
          Button::ButtonStateColors{clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::Primary)};
      button.setCustomPalette(std::move(headerPalette));
      button.setFontSize(Style::fontSizeCaption);
      button.setControlHeight(26.0F);
      button.setPadding(0.0F);
      button.setContentAlign(alignment);
    };
    for (auto column : filemanager::kFileColumns) {
      const auto index = columnIndex(column);
      auto header = makeButton(filemanager::kFileColumnLabels[index], [this, index] {
        setSortField(kColumnSortFields[index]);
      }, &m_fileColumnHeaders[index]);
      styleHeader(*header, column == FileColumn::Name || column == FileColumn::Type
          ? ButtonContentAlign::Start : ButtonContentAlign::End);
      header->setMinWidth(filemanager::kMinimumFileColumns[column]);
      header->setMaxWidth(std::max(filemanager::kMinimumFileColumns[column], m_listColumns[column]));
      fileHeader->addChild(std::move(header));
    }
    const auto makeColumnMarker = [](ColorSpec color) {
      auto node = std::make_unique<Separator>();
      node->setParticipatesInLayout(false);
      node->setHitTestVisible(false);
      node->setZIndex(1);
      node->setOrientation(SeparatorOrientation::VerticalRule);
      node->setThickness(2.0F);
      node->setGradientEdges(true);
      node->setColor(color);
      node->setSize(2.0F, 20.0F);
      return node;
    };
    for (auto& marker : m_fileColumnMarkers) {
      auto node = makeColumnMarker(colorSpecFromRole(ColorRole::Primary, 0.58F));
      marker = node.get();
      fileHeader->addChild(std::move(node));
    }
    auto dropMarker = makeColumnMarker(colorSpecFromRole(ColorRole::Primary));
    m_fileColumnDropMarker = dropMarker.get();
    dropMarker->setZIndex(2);
    dropMarker->setVisible(false);
    fileHeader->addChild(std::move(dropMarker));
    mainPane->addChild(std::move(fileHeader));
    mainPane->addChild(
        ui::separator({
            .color = colorSpecFromRole(ColorRole::OutlineVariant, 0.45F),
            .gradientEdges = false,
        })
    );

    auto grid = std::make_unique<VirtualGridView>();
    m_grid = grid.get();
    m_grid->setFlexGrow(1.0F);
    m_grid->setFill(colorSpecFromRole(ColorRole::Surface, 0.38F));
    m_grid->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.26F), Style::borderWidth);
    applyNytheraBevel(*m_grid, Style::scaledRadiusMd(1.0F));
    m_grid->setAdapter(m_adapter.get());
    m_grid->setSquareCells(false);
    if (!m_viewScaleLoaded) {
      m_viewScaleLoaded = true;
      if (auto stored = m_configService.stateString("files", "view_scale"))
        try {
          const float scale = std::stof(*stored);
          if (std::isfinite(scale))
            m_viewScale = filemanager::clampFileViewScale(scale);
        } catch (...) {
        }
    }
    const auto metrics = filemanager::fileViewMetrics(m_viewScale);
    m_grid->setColumns(m_gridMode ? 0 : 1);
    m_grid->setMinCellWidth(metrics.gridCellWidth);
    m_grid->setCellHeight(m_gridMode ? metrics.gridCellHeight : metrics.listRowHeight);
    m_grid->setRowGap(m_gridMode ? 3.0F : 1.0F);
    updateFileColumnHeaders();
    m_grid->setOnRubberBandSelection([this](const std::vector<std::size_t>& hits, bool finished, bool cancelled) {
      if (cancelled) {
        m_selectedPaths = m_rubberBaseline;
        m_selectedPath = m_rubberSelectedPath;
        m_selectionAnchor = m_rubberAnchor;
      } else {
        const bool ctrl = m_rubberModifiers & KeyMod::Ctrl;
        const bool shift = m_rubberModifiers & KeyMod::Shift;
        m_selectedPaths = (ctrl || shift) ? m_rubberBaseline : std::unordered_set<std::string>{};
        for (const auto index : hits) {
          if (index >= m_entries.size())
            continue;
          const auto path = m_entries[index].absPath.string();
          if (ctrl && m_rubberBaseline.contains(path))
            m_selectedPaths.erase(path);
          else
            m_selectedPaths.insert(path);
        }
        if (shift && !hits.empty() && m_rubberAnchor) {
          const auto anchor = std::ranges::find(m_entries, *m_rubberAnchor, &FileEntry::absPath);
          if (anchor != m_entries.end()) {
            const auto index = static_cast<std::size_t>(anchor - m_entries.begin());
            const auto endpoint = hits.back();
            for (auto i = std::min(index, endpoint); i <= std::max(index, endpoint) && i < m_entries.size(); ++i)
              m_selectedPaths.insert(m_entries[i].absPath.string());
          }
        }
        if (!hits.empty() && hits.back() < m_entries.size())
          m_selectedPath = m_entries[hits.back()].absPath;
        else if (m_selectedPaths.empty())
          m_selectedPath.reset();
        if (finished && !ctrl && !shift)
          m_selectionAnchor = m_selectedPath;
      }
      m_restoringSelection = true;
      const auto selected =
          m_selectedPath ? std::ranges::find(m_entries, *m_selectedPath, &FileEntry::absPath) : m_entries.end();
      m_grid->setSelectedIndex(
          selected == m_entries.end() ? std::nullopt : std::optional<std::size_t>(selected - m_entries.begin()), false
      );
      m_restoringSelection = false;
      m_grid->notifyDataChanged();
      updateSelectionSummary();
      if (finished) {
        m_rubberBaseline.clear();
        m_selectionTimer.stop();
      }
      if (m_surface)
        m_surface->requestRedraw();
    });
    m_grid->setOnSelectionChanged([this](std::optional<std::size_t> index) {
      if (m_restoringSelection)
        return;
      if (index.has_value() && *index < m_entries.size()) {
        m_selectedPath = m_entries[*index].absPath;
        const auto path = m_entries[*index].absPath.string();
        if ((m_keyboardModifiers & KeyMod::Ctrl) != 0U) {
          if (!m_selectedPaths.erase(path))
            m_selectedPaths.insert(path);
          if (!m_selectionAnchor)
            m_selectionAnchor = m_entries[*index].absPath;
        } else if ((m_keyboardModifiers & KeyMod::Shift) != 0U && m_selectionAnchor) {
          const auto anchor = std::ranges::find(m_entries, *m_selectionAnchor, &FileEntry::absPath);
          if (anchor != m_entries.end()) {
            const auto anchorIndex = static_cast<std::size_t>(anchor - m_entries.begin());
            const auto first = std::min(anchorIndex, *index);
            const auto last = std::max(anchorIndex, *index);
            m_selectedPaths.clear();
            for (auto selected = first; selected <= last; ++selected)
              m_selectedPaths.insert(m_entries[selected].absPath.string());
          }
        } else {
          m_selectedPaths.clear();
          m_selectedPaths.insert(path);
          m_selectionAnchor = m_entries[*index].absPath;
        }
        m_grid->notifyDataChanged();
      } else {
        m_selectedPaths.clear();
        m_selectionAnchor.reset();
        m_selectedPath.reset();
      }
      updateSelectionSummary();
    });
    mainPane->addChild(std::move(grid));

    auto status = std::make_unique<Label>();
    m_statusLabel = status.get();
    m_statusLabel->setText("Loading…");
    const float statusFontSize =
        12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
    m_statusLabel->setFontSize(statusFontSize);
    m_statusLabel->setMaxLines(1);
    m_statusLabel->setEllipsize(TextEllipsize::Middle);
    m_statusLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    auto statusDetails = std::make_unique<Label>();
    m_statusDetailLabel = statusDetails.get();
    m_statusDetailLabel->setFontSize(statusFontSize);
    m_statusDetailLabel->setMaxLines(1);
    m_statusDetailLabel->setEllipsize(TextEllipsize::Middle);
    m_statusDetailLabel->setFlexGrow(1.0F);
    m_statusDetailLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    auto statusFree = std::make_unique<Label>();
    m_statusFreeLabel = statusFree.get();
    m_statusFreeLabel->setFontSize(statusFontSize);
    m_statusFreeLabel->setMaxLines(1);
    m_statusFreeLabel->setEllipsize(TextEllipsize::End);
    m_statusFreeLabel->setTextAlign(TextAlign::End);
    m_statusFreeLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    auto statusRow = std::make_unique<Flex>();
    statusRow->setAlign(FlexAlign::Center);
    statusRow->setGap(Style::spaceSm);
    statusRow->setFlexGrow(1.0F);
    statusRow->addChild(std::move(status));
    statusRow->addChild(std::move(statusDetails));
    statusRow->addChild(std::move(statusFree));
    auto statusDivider = std::make_unique<Flex>();
    statusDivider->setMinHeight(4.0F);
    statusDivider->setMaxHeight(4.0F);
    statusDivider->setHitTestVisible(true);
    statusDivider->setFill(clearColorSpec());
    m_statusDivider = static_cast<Flex*>(mainPane->addChild(std::move(statusDivider)));
    auto statusStrip = std::make_unique<Flex>();
    statusStrip->setAlign(FlexAlign::Center);
    statusStrip->setPadding(2.0F, 10.0F);
    statusStrip->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, 0.82F));
    statusStrip->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.55F), Style::borderWidth);
    statusStrip->setRadius(Style::scaledRadiusSm(1.0F));
    statusStrip->setMinHeight(m_statusHeight);
    statusStrip->setMaxHeight(m_statusHeight);
    statusStrip->setHitTestVisible(true);
    m_statusStrip = static_cast<Flex*>(mainPane->addChild(std::move(statusStrip)));
    m_statusStrip->addChild(std::move(statusRow));
    m_statusStrip->setPadding(std::clamp((m_statusHeight - statusFontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
    content->addChild(std::move(mainPane));
    m_root->addChild(std::move(content));
    m_dispatcher.setSceneRoot(m_root.get());
    m_dispatcher.setFocus(m_grid->focusArea());
    m_surface->setSceneRoot(m_root.get());
    rebuildBreadcrumbs();
    updateNavigationControls();
    setLocationEditMode(false);
    needsLayout = true;
  }
  if (m_appMenuItems && m_compactMenuButton) {
    const bool collapse = !m_showApplicationMenuBar || m_surface->width() < 640;
    if (collapse != m_applicationMenuCollapsed) {
      m_applicationMenuCollapsed = collapse;
      m_appMenuItems->setVisible(!collapse);
      m_appMenuItems->setParticipatesInLayout(!collapse);
      m_compactMenuButton->setVisible(collapse);
      m_compactMenuButton->setParticipatesInLayout(collapse);
      updateApplicationMenuUnderline();
      m_surface->requestLayout();
    }
  }
  if (m_draggingFiles || m_dragPreview) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    updateDragPreview(renderer);
  }
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const float maxSidebar =
        std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300.0F));
    const float effectiveSidebar = std::clamp(m_sidebarWidth, kSidebarMinWidth, maxSidebar);
    if (m_sidebar) {
      m_sidebar->setMinWidth(effectiveSidebar);
      m_sidebar->setMaxWidth(effectiveSidebar);
    }
    for (const auto& capacity : m_placeCapacities)
      if (capacity.bar)
        capacity.bar->setSize(std::max(80.0F, effectiveSidebar - 28.0F), 4.0F);
    const bool showSidebar = m_surface->width() >= 760;
    m_sidebar->setVisible(showSidebar);
    m_sidebar->setParticipatesInLayout(showSidebar);
    m_sidebarDivider->setVisible(showSidebar);
    m_sidebarDivider->setParticipatesInLayout(showSidebar);
    m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_root->layout(renderer);
    if (applyListColumnWidths()) {
      m_fileHeader->arrange(renderer, LayoutRect{.x = m_fileHeader->x(), .y = m_fileHeader->y(),
          .width = m_fileHeader->width(), .height = m_fileHeader->height()});
      m_surface->requestLayout();
    }
    positionFileColumnMarkers();
    finishPendingTransferReveal(renderer);
    // Markers are positioned overlays: keep them out of Flex sizing, but still
    // lay out each Separator's internal gradient rectangles explicitly.
    for (auto* marker : m_fileColumnMarkers)
      if (marker && marker->visible())
        marker->layout(renderer);
    if (m_fileColumnDropMarker && m_fileColumnDropMarker->visible())
      m_fileColumnDropMarker->layout(renderer);
    syncTooltipHover();
  }
  if (m_pendingInlineRenamePath) {
    const auto pending = *m_pendingInlineRenamePath;
    m_pendingInlineRenamePath.reset();
    beginRenamePath(pending);
  }
}

void FileManager::syncProgressWindow() {
  if (!m_wayland.isConnected())
    return;
  const bool visibleJob = std::ranges::any_of(m_operations.operations(), [](const auto& operation) {
    return !operation.finished && operation.progressRevealed;
  });
  const bool needFallback = visibleJob && (!m_operations.hasJobService() || m_backgroundConflictPending);
  if (!needFallback) {
    if (m_progressSurface) {
      m_progressDispatcher.setSceneRoot(nullptr);
      m_progressSurface->setSceneRoot(nullptr);
      m_progressSurface.reset();
      m_progressRoot.reset();
      m_progressLabel = nullptr;
      m_progressCancelButton = nullptr;
    }
    return;
  }
  if (m_progressSurface) {
    if (m_progressLabel != nullptr)
      m_progressLabel->setText(progressSummary(m_operations.operations()));
    m_progressSurface->requestLayout();
    return;
  }
  auto surface = std::make_unique<ToplevelSurface>(m_wayland);
  surface->setRenderContext(&m_renderContext);
  surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_progressSurface)
      m_progressSurface->requestLayout();
  });
  surface->setClosedCallback([this] {
    g_idle_add(
        [](gpointer context) -> gboolean {
          auto* self = static_cast<FileManager*>(context);
          if (self->m_progressSurface) {
            self->m_progressDispatcher.setSceneRoot(nullptr);
            self->m_progressSurface->setSceneRoot(nullptr);
            self->m_progressSurface.reset();
            self->m_progressRoot.reset();
            self->m_progressLabel = nullptr;
            self->m_progressCancelButton = nullptr;
          }
          self->syncProgressWindow();
          return G_SOURCE_REMOVE;
        },
        this
    );
  });
  surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareProgressFrame(update, layout); });
  ToplevelSurfaceConfig config{
      .width = 560,
      .height = 340,
      .minWidth = 480,
      .minHeight = 260,
      .maxWidth = 640,
      .maxHeight = 440,
      .parent = m_surface ? m_surface->xdgToplevel() : nullptr,
      .title = "Nythera Files — Operations",
      .appId = "dev.nythera.FileManager.Progress"
  };
  wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
  if (!surface->initialize(output, config))
    return;
  m_progressSurface = std::move(surface);
  m_progressWindowClosed = false;
}

void FileManager::prepareProgressFrame(bool, bool needsLayout) {
  if (!m_progressSurface || m_progressSurface->width() == 0 || m_progressSurface->height() == 0)
    return;
  m_renderContext.makeCurrent(m_progressSurface->renderTarget());
  auto& renderer = m_progressSurface->renderTarget().renderer();
  if (!m_progressRoot) {
    m_progressRoot = std::make_unique<Flex>();
    m_progressRoot->setDirection(FlexDirection::Vertical);
    m_progressRoot->setAlign(FlexAlign::Stretch);
    m_progressRoot->setGap(8.0F);
    m_progressRoot->setPadding(16.0F);
    m_progressRoot->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.95F));
    m_progressRoot->setBorder(colorSpecFromRole(ColorRole::Primary, 0.55F), Style::borderWidth);
    applyNytheraBevel(*m_progressRoot, Style::scaledRadiusMd(1.0F));
    auto heading = std::make_unique<Label>();
    heading->setText("FILE OPERATIONS");
    heading->setFontWeight(FontWeight::SemiBold);
    heading->setColor(colorSpecFromRole(ColorRole::Primary));
    m_progressRoot->addChild(std::move(heading));
    auto label = std::make_unique<Label>();
    label->setText(progressSummary(m_operations.operations()));
    label->setMaxWidth(600.0F);
    label->setMaxLines(12);
    label->setFlexGrow(1.0F);
    m_progressLabel = label.get();
    m_progressRoot->addChild(std::move(label));
    auto cancel = std::make_unique<Button>();
    cancel->setText("Cancel active operations");
    cancel->setVariant(ButtonVariant::Outline);
    cancel->setOnClick([this] {
      for (const auto& operation : m_operations.operations())
        if (!operation.finished)
          m_operations.cancel(operation.id);
    });
    m_progressCancelButton = cancel.get();
    m_progressRoot->addChild(std::move(cancel));
    auto quit = std::make_unique<Button>();
    quit->setText("Quit and stop operations…");
    quit->setVariant(ButtonVariant::Ghost);
    quit->setOnClick([this] { requestQuit(m_progressSurface.get()); });
    m_progressRoot->addChild(std::move(quit));
    m_progressDispatcher.setSceneRoot(m_progressRoot.get());
    m_progressDispatcher.setFocus(m_progressCancelButton->inputArea());
    m_progressSurface->setSceneRoot(m_progressRoot.get());
    needsLayout = true;
  }
  if (m_progressLabel != nullptr)
    m_progressLabel->setText(progressSummary(m_operations.operations()));
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_progressRoot->setSize(
        static_cast<float>(m_progressSurface->width()), static_cast<float>(m_progressSurface->height())
    );
    m_progressRoot->layout(renderer);
  }
}

void FileManager::handleProgressKeyboard(const KeyboardEvent& event) {
  if (!event.pressed
      || event.preedit
      || !m_progressSurface
      || m_wayland.lastKeyboardSurface() != m_progressSurface->wlSurface())
    return;
  if (event.sym == XKB_KEY_Escape || event.sym == XKB_KEY_Delete) {
    for (const auto& operation : m_operations.operations())
      if (!operation.finished)
        m_operations.cancel(operation.id);
    return;
  }
  m_progressDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

void FileManager::requestQuit(ToplevelSurface* parent) {
  if (!m_operations.active()) {
    m_quitting = true;
    if (m_surface && m_windowOpen)
      ToplevelSurface::handleToplevelClose(m_surface.get(), nullptr);
    if (m_pendingClipboardReads == 0)
      m_running = false;
    return;
  }
  const auto activeCount =
      std::ranges::count_if(m_operations.operations(), [](const auto& operation) { return !operation.finished; });
  showDialog(
      "Quit Nythera Files?",
      "Quitting will stop "
          + std::to_string(activeCount)
          + (activeCount == 1 ? " active file operation. Incomplete staged copies will be removed."
                              : " active file operations. Incomplete staged copies will be removed."),
      {{"Cancel", 0}, {"Quit", 1}},
      [this](int choice) {
        if (choice != 1)
          return;
        for (const auto& operation : m_operations.operations())
          if (!operation.finished)
            m_operations.cancel(operation.id);
        m_quitting = true;
        if (m_surface && m_windowOpen)
          ToplevelSurface::handleToplevelClose(m_surface.get(), nullptr);
        if (!m_operations.active() && m_pendingClipboardReads == 0)
          m_running = false;
      },
      parent
  );
}

void FileManager::showDialog(
    std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
    std::function<void(int)> response, ToplevelSurface* parent, bool applyToAllCheckbox, bool alternativeName
) {
  if (m_dialogSurface || !m_wayland.isConnected()) {
    if (response)
      response(-1);
    return;
  }
  if (parent == nullptr)
    parent = m_surface ? m_surface.get() : m_progressSurface.get();
  m_dialogTitle = std::move(title);
  m_dialogWarningStyle = m_dialogTitle == "Permanently delete?" || m_dialogTitle == "Empty Wastebin?";
  m_dialogResizable = true;
  m_dialogResizeEdges.fill(nullptr);
  m_dialogMoveHandle = nullptr;
  m_dialogTitleRow = nullptr;
  m_dialogSpacer = nullptr;
  m_dialogFooter = nullptr;
  m_dialogDivider = nullptr;
  m_dialogPreferredWidth = 0;
  m_dialogPreferredHeight = 0;
  if (alternativeName && !m_textPrompt)
    m_dialogSizeStateKey = "conflict_dialog_size";
  else {
    m_dialogSizeStateKey = "dialog_size_";
    bool separator = false;
    for (const unsigned char character : m_dialogTitle) {
      if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
          || (character >= '0' && character <= '9')) {
        const auto normalized = character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        m_dialogSizeStateKey.push_back(static_cast<char>(normalized));
        separator = false;
      } else if (!separator && m_dialogSizeStateKey.back() != '_') {
        m_dialogSizeStateKey.push_back('_');
        separator = true;
      }
    }
    while (m_dialogSizeStateKey.back() == '_')
      m_dialogSizeStateKey.pop_back();
  }
  if (m_dialogResizable) {
    if (const auto saved = m_configService.stateString("files", m_dialogSizeStateKey)) {
      const auto separator = saved->find('x');
      if (separator != std::string::npos) {
        std::uint32_t savedWidth = 0;
        std::uint32_t savedHeight = 0;
        const auto [widthEnd, widthError] = std::from_chars(saved->data(), saved->data() + separator, savedWidth);
        const auto [heightEnd, heightError] =
            std::from_chars(saved->data() + separator + 1, saved->data() + saved->size(), savedHeight);
        if (widthError == std::errc{} && widthEnd == saved->data() + separator && heightError == std::errc{}
            && heightEnd == saved->data() + saved->size() && savedWidth >= 360 && savedHeight >= 180) {
          m_dialogPreferredWidth = savedWidth;
          m_dialogPreferredHeight = savedHeight;
        }
      }
    }
  }
  m_dialogMessage = std::move(message);
  m_dialogChoices = std::move(choices);
  m_dialogApplyToAllCheckbox = applyToAllCheckbox;
  m_dialogApplyToAllChecked = false;
  m_dialogAlternativeNameEnabled = alternativeName;
  m_dialogAlternativeName.clear();
  m_dialogAlternativeNameInput = nullptr;
  m_dialogResponse = std::move(response);
  m_dialogCloseQueued = false;
  auto surface = std::make_unique<ToplevelSurface>(m_wayland);
  surface->setRenderContext(&m_renderContext);
  surface->setConfigureCallback([this](std::uint32_t width, std::uint32_t height) {
    if (m_dialogSurface) {
      if (m_dialogResizable && width > 0 && height > 0) {
        m_dialogPreferredWidth = width;
        m_dialogPreferredHeight = height;
      }
      m_dialogSurface->requestLayout();
    }
  });
  surface->setClosedCallback([this] { closeDialog(-1); });
  surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareDialogFrame(update, layout); });
  const auto* outputInfo = m_wayland.outputs().empty() ? nullptr : &m_wayland.outputs().front();
  const auto maxWidth = static_cast<std::uint32_t>(
      outputInfo == nullptr ? 720 : std::min(720, outputInfo->logicalWidth > 0 ? outputInfo->logicalWidth - 40 : 720)
  );
  const auto maxHeight = static_cast<std::uint32_t>(
      outputInfo == nullptr ? 900 : std::min(900, outputInfo->logicalHeight > 0 ? outputInfo->logicalHeight - 80 : 900)
  );
  const auto minWidth = std::min(alternativeName && !m_textPrompt ? 520U : 420U, maxWidth);
  const auto width = m_dialogResizable
      ? (m_dialogPreferredWidth > 0 ? std::max(m_dialogPreferredWidth, minWidth)
                                   : std::min(maxWidth, alternativeName ? 720U : 520U))
      : std::min(maxWidth, alternativeName ? 720U : 520U);
  buildDialog(static_cast<float>(width));
  ScaledRenderer measureRenderer(m_renderContext, outputInfo == nullptr ? 1.0F : outputInfo->configuredScale());
  LayoutConstraints constraints;
  constraints.setExactWidth(static_cast<float>(width));
  const auto measured = m_dialogRoot->measure(measureRenderer, constraints);
  const auto measuredHeight = std::clamp(static_cast<std::uint32_t>(std::ceil(measured.height)), 180U, maxHeight);
  const auto minHeight = std::min(alternativeName && !m_textPrompt ? 260U : 180U, maxHeight);
  const auto height = m_dialogResizable && m_dialogPreferredHeight > 0
      ? std::max(m_dialogPreferredHeight, minHeight)
      : measuredHeight;
  if (measured.height > static_cast<float>(height)) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const auto focus = m_dialogDispatcher.captureTabFocus();
    m_dialogDispatcher.setSceneRoot(nullptr);
    auto footer = m_dialogRoot->removeChild(m_dialogFooter);
    auto spacer = m_dialogRoot->removeChild(m_dialogSpacer);
    m_dialogSpacer->setFlexGrow(0.0F);
    auto scroll = std::make_unique<ScrollView>();
    scroll->setFlexGrow(1.0F);
    scroll->content()->setDirection(FlexDirection::Vertical);
    scroll->content()->setAlign(FlexAlign::Stretch);
    scroll->content()->setGap(10.0F);
    if (m_dialogWarningStyle) {
      const auto& children = m_dialogRoot->children();
      const auto content = std::ranges::find_if(children, [](const auto& child) {
        return child->participatesInLayout();
      });
      const auto index = static_cast<std::size_t>(content - children.begin());
      scroll->content()->addChild(m_dialogRoot->removeChild(content->get()));
      m_dialogRoot->insertChildAt(index, std::move(scroll));
    } else {
      const auto& children = m_dialogRoot->children();
      const auto content = std::ranges::find_if(children, [](const auto& child) {
        return child->participatesInLayout();
      });
      const auto scrollIndex = static_cast<std::size_t>(content - children.begin());
      auto index = scrollIndex;
      while (index < m_dialogRoot->children().size()) {
        auto* child = m_dialogRoot->children()[index].get();
        if (child->participatesInLayout())
          scroll->content()->addChild(m_dialogRoot->removeChild(child));
        else
          ++index;
      }
      m_dialogRoot->insertChildAt(scrollIndex, std::move(scroll));
    }
    m_dialogRoot->addChild(std::move(spacer));
    m_dialogRoot->addChild(std::move(footer));
    m_dialogDispatcher.setSceneRoot(m_dialogRoot.get());
    m_dialogDispatcher.restoreTabFocus(focus);
  }
  ToplevelSurfaceConfig config{
      .width = width,
      .height = height,
      .minWidth = minWidth,
      .minHeight = minHeight,
      .maxWidth = m_dialogResizable ? 0U : width,
      .maxHeight = m_dialogResizable ? 0U : height,
      .parent = parent == nullptr ? nullptr : parent->xdgToplevel(),
      .title = m_dialogTitle,
      .appId = "dev.nythera.FileManager.Dialog"
  };
  wl_output* output = outputInfo == nullptr ? nullptr : outputInfo->output;
  if (surface->initialize(output, config)) {
    m_dialogSurface = std::move(surface);
    m_dialogSurface->setSceneRoot(m_dialogRoot.get());
  } else {
    auto failed = std::move(m_dialogResponse);
    if (failed)
      failed(-1);
  }
}

void FileManager::closeDialog(int response) {
  if (m_dialogCloseQueued)
    return;
  if (m_symlinkDialogDestination && response == 1 && !submitSymlinkDialog())
    return;
  if (!m_textPrompt && response == 2 && m_dialogAlternativeNameInput != nullptr) {
    const auto& name = m_dialogAlternativeNameInput->value();
    if (!name.empty()) {
      std::error_code error;
      const bool occupied = std::filesystem::exists(m_dialogDestination.parent_path() / name, error);
      if (name == "." || name == ".." || name.find('/') != std::string::npos || occupied || error) {
        m_dialogAlternativeNameInput->setInvalid(true);
        m_dialogDispatcher.setFocus(m_dialogAlternativeNameInput->inputArea());
        m_dialogSurface->requestLayout();
        return;
      }
    }
  }
  m_dialogCloseQueued = true;
  g_idle_add(
      [](gpointer context) -> gboolean {
        auto* self = static_cast<FileManager*>(context);
        auto callback = std::move(self->m_dialogResponse);
        self->m_dialogAlternativeName =
            self->m_dialogAlternativeNameInput == nullptr ? std::string{} : self->m_dialogAlternativeNameInput->value();
        const int result = self->m_dialogResultPending
            + (self->m_dialogApplyToAllChecked
                       && self->m_dialogResultPending >= 1
                       && (self->m_dialogResultPending <= 3 || self->m_dialogResultPending == 5)
                   ? 10
                   : 0);
        if (self->m_dialogResizable && self->m_dialogSurface && self->m_dialogSurface->width() >= 360
            && self->m_dialogSurface->height() >= 180) {
          self->m_dialogPreferredWidth = self->m_dialogSurface->width();
          self->m_dialogPreferredHeight = self->m_dialogSurface->height();
          const auto size = std::to_string(self->m_dialogPreferredWidth) + "x"
              + std::to_string(self->m_dialogPreferredHeight);
          if (!self->m_configService.setStateString("files", self->m_dialogSizeStateKey, size))
            self->setStatus("Unable to save conflict dialog size");
        }
        self->m_dialogDispatcher.setSceneRoot(nullptr);
        if (self->m_dialogSurface) {
          self->m_dialogSurface->setSceneRoot(nullptr);
        }
        self->m_dialogCards.clear();
        self->m_dialogRoot.reset();
        self->m_dialogSurface.reset();
        self->m_dialogAlternativeNameInput = nullptr;
        self->m_dialogCloseQueued = false;
        if (callback)
          callback(result);
        return G_SOURCE_REMOVE;
      },
      this
  );
  m_dialogResultPending = response;
}

void FileManager::handleDialogKeyboard(const KeyboardEvent& event) {
  if (!event.pressed || event.preedit || !m_dialogSurface)
    return;
  if (event.sym == XKB_KEY_Escape) {
    closeDialog(-1);
    return;
  }
  if (m_symlinkDialogDestination && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter)) {
    auto* focus = m_dialogDispatcher.focusedArea();
    if (focus == m_symlinkDialogTarget->inputArea()) {
      closeDialog(1);
      return;
    }
    if (focus == m_symlinkDialogName->inputArea()) {
      if (filemanager::validFileName(m_symlinkDialogName->value()) && m_symlinkDialogTarget->value().empty())
        m_dialogDispatcher.setFocus(m_symlinkDialogTarget->inputArea());
      else
        closeDialog(1);
      return;
    }
  }
  m_dialogDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

void FileManager::buildDialog(float dialogWidth) {
  UiPhaseScope layoutPhase(UiPhase::Layout);
  m_dialogCards.clear();
  m_dialogRoot = std::make_unique<Flex>();
  m_dialogRoot->setDirection(FlexDirection::Vertical);
  m_dialogRoot->setAlign(FlexAlign::Stretch);
  m_dialogRoot->setGap(10.0F);
  m_dialogRoot->setPadding(16.0F);
  m_dialogRoot->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  m_dialogRoot->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
  applyNytheraBevel(*m_dialogRoot, Style::scaledRadiusMd(1.0F));
  if (m_dialogWarningStyle) {
    auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
    m_dialogTopAccent = topAccent.get();
    m_dialogRoot->addChild(std::move(topAccent));
    auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
    m_dialogCornerAccent = cornerAccent.get();
    m_dialogRoot->addChild(std::move(cornerAccent));

    auto content = std::make_unique<Flex>();
    content->setDirection(FlexDirection::Horizontal);
    content->setAlign(FlexAlign::Center);
    content->setGap(18.0F);
    // Keep the warning at its natural height. Only the footer spacer grows.
    auto warning = std::make_unique<Image>();
    warning->setHitTestVisible(false);
    warning->setForegroundTint(colorSpecFromRole(ColorRole::Error));
    warning->setFrameSize(72.0F, 64.0F);
    m_deleteWarning = warning.get();
    content->addChild(std::move(warning));
    auto text = std::make_unique<Flex>();
    text->setDirection(FlexDirection::Vertical);
    text->setAlign(FlexAlign::Stretch);
    text->setGap(7.0F);
    text->setFlexGrow(1.0F);
    auto heading = std::make_unique<Label>();
    heading->setText(m_dialogTitle);
    heading->setFontSize(Style::fontSizeTitle);
    heading->setFontWeight(FontWeight::SemiBold);
    heading->setColor(colorSpecFromRole(ColorRole::Primary));
    heading->setMaxLines(2);
    text->addChild(std::move(heading));
    auto message = std::make_unique<Label>();
    message->setText(m_dialogMessage);
    message->setMaxWidth(std::max(1.0F, dialogWidth - 122.0F));
    message->setMaxLines(6);
    text->addChild(std::move(message));
    if (m_dialogResizable) {
      auto moveHandle = std::make_unique<InputArea>();
      moveHandle->setParticipatesInLayout(false);
      moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
      moveHandle->setOnPress([this](const InputArea::PointerData& data) {
        if (data.pressed && m_dialogSurface)
          xdg_toplevel_move(m_dialogSurface->xdgToplevel(), m_wayland.seat(), data.serial);
      });
      m_dialogMoveHandle = moveHandle.get();
      m_dialogTitleRow = text.get();
      text->addChild(std::move(moveHandle));
    }
    content->addChild(std::move(text));
    m_dialogRoot->addChild(std::move(content));
  } else {
    auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
    m_dialogTopAccent = topAccent.get();
    m_dialogRoot->addChild(std::move(topAccent));
    auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
    m_dialogCornerAccent = cornerAccent.get();
    m_dialogRoot->addChild(std::move(cornerAccent));
    auto heading = std::make_unique<Label>();
    heading->setText(m_dialogTitle);
    heading->setMaxWidth(dialogWidth - 48.0F);
    heading->setMaxLines(2);
    heading->setFontWeight(FontWeight::SemiBold);
    heading->setColor(colorSpecFromRole(ColorRole::Primary));
    if (m_dialogResizable) {
      auto titleRow = std::make_unique<Flex>();
      titleRow->setDirection(FlexDirection::Horizontal);
      titleRow->setAlign(FlexAlign::Center);
      heading->setFlexGrow(1.0F);
      titleRow->addChild(std::move(heading));
      auto moveHandle = std::make_unique<InputArea>();
      moveHandle->setParticipatesInLayout(false);
      moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
      moveHandle->setOnPress([this](const InputArea::PointerData& data) {
        if (data.pressed && m_dialogSurface)
          xdg_toplevel_move(m_dialogSurface->xdgToplevel(), m_wayland.seat(), data.serial);
      });
      m_dialogMoveHandle = moveHandle.get();
      titleRow->addChild(std::move(moveHandle));
      m_dialogTitleRow = titleRow.get();
      m_dialogRoot->addChild(std::move(titleRow));
    } else
      m_dialogRoot->addChild(std::move(heading));
  }
  if (m_dialogAlternativeNameEnabled && !m_textPrompt) {
    auto explanation = std::make_unique<Label>();
    explanation->setText("Choose what to do with the incoming item.");
    explanation->setMaxWidth(dialogWidth - 48.0F);
    m_dialogRoot->addChild(std::move(explanation));
  }
  const auto existingMarker =
      m_dialogAlternativeNameEnabled && !m_textPrompt ? m_dialogMessage.find("\n\n") : std::string::npos;
  if (existingMarker != std::string::npos) {
    const std::string incoming = m_dialogMessage.substr(0, existingMarker);
    const std::string existing = m_dialogMessage.substr(existingMarker + 2);
    const auto addComparisonCard = [this, dialogWidth](std::string text, std::string_view sectionTitle) {
      auto card = std::make_unique<Flex>();
      m_dialogCards.push_back(card.get());
      card->setDirection(FlexDirection::Vertical);
      card->setAlign(FlexAlign::Stretch);
      card->setGap(4.0F);
      card->setPadding(10.0F, 12.0F);
      card->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
      card->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
      applyNytheraBevel(*card, Style::scaledRadiusSm(1.0F));
      auto headingLabel = std::make_unique<Label>();
      headingLabel->setText(sectionTitle);
      headingLabel->setFontWeight(FontWeight::SemiBold);
      headingLabel->setColor(colorSpecFromRole(ColorRole::Primary));
      card->addChild(std::move(headingLabel));
      auto itemRow = std::make_unique<Flex>();
      itemRow->setDirection(FlexDirection::Horizontal);
      itemRow->setAlign(FlexAlign::Center);
      itemRow->setGap(10.0F);
      auto icon = std::make_unique<FileIcon>();
      const auto firstNewline = text.find('\n');
      const auto itemName = text.substr(0, firstNewline);
      const auto metadataEnd = text.find('\n', firstNewline + 1);
      const auto metadata = text.substr(firstNewline + 1, metadataEnd - firstNewline - 1);
      const bool folder = metadata.starts_with("Folder");
      const auto pathStart = text.find("\n", text.find('\n') + 1);
      const auto itemPath = pathStart == std::string::npos ? std::string{} : text.substr(pathStart + 1);
      icon->setFile(itemPath, folder);
      icon->setGlyphSize(28.0F);
      itemRow->addChild(std::move(icon));
      auto details = std::make_unique<Label>();
      details->setText(itemName + "\n" + metadata);
      details->setMaxWidth(dialogWidth - 110.0F);
      details->setMaxLines(3);
      details->setEllipsize(TextEllipsize::End);
      details->setFlexGrow(1.0F);
      details->setTooltip(metadata);
      itemRow->addChild(std::move(details));
      card->addChild(std::move(itemRow));
      auto path = std::make_unique<Label>();
      path->setText(pathStart == std::string::npos ? std::string{} : text.substr(pathStart + 1));
      path->setMaxWidth(dialogWidth - 56.0F);
      path->setMaxLines(2);
      path->setTooltip(path->text());
      card->addChild(std::move(path));
      m_dialogRoot->addChild(std::move(card));
    };
    addComparisonCard(incoming, "INCOMING ITEM");
    addComparisonCard(existing, "EXISTING DESTINATION");
    if (!m_dialogComparisonSummary.empty()) {
      auto summary = std::make_unique<Label>();
      summary->setText(m_dialogComparisonSummary);
      summary->setMaxWidth(dialogWidth - 48.0F);
      summary->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_dialogRoot->addChild(std::move(summary));
    }
  } else if (!m_dialogWarningStyle) {
    auto message = std::make_unique<Label>();
    message->setText(m_dialogMessage);
    message->setMaxWidth(dialogWidth - 48.0F);
    message->setMaxLines(14);
    m_dialogRoot->addChild(std::move(message));
  }
  if (m_dialogApplyToAllCheckbox) {
    auto applyRow = std::make_unique<Flex>();
    applyRow->setDirection(FlexDirection::Horizontal);
    applyRow->setAlign(FlexAlign::Center);
    applyRow->setGap(6.0F);
    auto checkbox = std::make_unique<Checkbox>();
    checkbox->setOnChange([this](bool checked) { m_dialogApplyToAllChecked = checked; });
    applyRow->addChild(std::move(checkbox));
    auto label = std::make_unique<Label>();
    label->setText("Apply this decision to remaining conflicts");
    label->setMaxWidth(dialogWidth - 80.0F);
    applyRow->addChild(std::move(label));
    m_dialogRoot->addChild(std::move(applyRow));
  }
  if (m_dialogAlternativeNameEnabled) {
    auto renameRow = std::make_unique<Flex>();
    renameRow->setDirection(FlexDirection::Horizontal);
    renameRow->setAlign(FlexAlign::Center);
    renameRow->setGap(8.0F);
    auto input = std::make_unique<Input>();
    m_dialogAlternativeNameInput = input.get();
    input->setPlaceholder(m_textPrompt ? "" : "Alternative name (for Keep Both)");
    input->setPasswordMode(m_passwordPrompt);
    input->setControlHeight(34.0F);
    input->setFlexGrow(1.0F);
    input->setOnSubmit([this](const std::string&) {
      if (m_textPrompt) {
        closeDialog(1);
        return;
      }
      const auto keepBoth =
          std::ranges::find_if(m_dialogChoices, [](const auto& choice) { return choice.first == "Keep Both"; });
      if (keepBoth != m_dialogChoices.end())
        closeDialog(keepBoth->second);
    });
    input->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t) {
      if (m_textPrompt && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
        closeDialog(1);
        return true;
      }
      return false;
    });
    renameRow->addChild(std::move(input));
    if (!m_textPrompt) {
      auto suggest = std::make_unique<Button>();
      suggest->setText("Suggest New Name");
      suggest->setVariant(ButtonVariant::Ghost);
      const auto incomingName = m_dialogMessage.substr(0, m_dialogMessage.find('\n'));
      const auto existingStart = m_dialogMessage.find("\n\n") + 2;
      const auto existingMetadataEnd = m_dialogMessage.find('\n', m_dialogMessage.find('\n', existingStart) + 1);
      const auto destinationEnd = m_dialogMessage.find('\n', existingMetadataEnd + 1);
      const auto destinationPath = std::filesystem::path(
          m_dialogMessage.substr(existingMetadataEnd + 1, destinationEnd - existingMetadataEnd - 1)
      );
      m_dialogDestination = destinationPath;
      m_dialogAlternativeNameInput->setOnChange([this](const std::string&) {
        m_dialogAlternativeNameInput->setInvalid(false);
      });
      const bool directory = m_dialogMessage.substr(m_dialogMessage.find('\n') + 1).starts_with("Folder");
      const auto stem = directory ? incomingName : std::filesystem::path(incomingName).stem().string();
      const auto extension = directory ? std::string{} : std::filesystem::path(incomingName).extension().string();
      suggest->setOnClick([this, parentPath = destinationPath.parent_path(), stem, extension] {
        for (unsigned suffix = 1; suffix < 10000; ++suffix) {
          const auto candidate =
              stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
          std::error_code error;
          const bool exists = std::filesystem::exists(parentPath / candidate, error);
          if (error)
            break;
          if (!exists) {
            m_dialogAlternativeNameInput->setValue(candidate);
            break;
          }
        }
      });
      renameRow->addChild(std::move(suggest));
    }
    m_dialogRoot->addChild(std::move(renameRow));
  }
  if (m_symlinkDialogDestination) {
    const auto addField = [this](const char* text) {
      auto field = std::make_unique<Flex>();
      field->setDirection(FlexDirection::Vertical);
      field->setAlign(FlexAlign::Stretch);
      field->setGap(4.0F);
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setFontWeight(FontWeight::SemiBold);
      field->addChild(std::move(label));
      auto input = std::make_unique<Input>();
      input->setControlHeight(34.0F);
      auto* control = input.get();
      field->addChild(std::move(input));
      control->setOnChange([this, control](const std::string&) {
        control->setInvalid(false);
        m_symlinkDialogError->setText(" ");
        if (m_dialogSurface)
          m_dialogSurface->requestLayout();
      });
      m_dialogRoot->addChild(std::move(field));
      return control;
    };
    m_symlinkDialogName = addField("Symlink name");
    m_symlinkDialogTarget = addField("Target path");
    m_symlinkDialogName->setOnSubmit([this](const std::string&) {
      m_dialogDispatcher.setFocus(m_symlinkDialogTarget->inputArea());
    });
    m_symlinkDialogTarget->setOnSubmit([this](const std::string&) { closeDialog(1); });
    auto error = std::make_unique<Label>();
    error->setText(" ");
    error->setColor(colorSpecFromRole(ColorRole::Error));
    error->setMaxWidth(dialogWidth - 48.0F);
    error->setMaxLines(2);
    m_symlinkDialogError = error.get();
    m_dialogRoot->addChild(std::move(error));
  }
  auto row = std::make_unique<Flex>();
  row->setAlign(FlexAlign::Center);
  row->setJustify(FlexJustify::End);
  row->setGap(8.0F);
  row->setWrap(true);
  InputArea* safeFocus = nullptr;
  for (const auto& [label, result] : m_dialogChoices) {
    auto button = std::make_unique<Button>();
    button->setText(label);
    const bool destructive =
        result != 0 && (label.find("Delete") != std::string::npos || label == "Quit" || label == "Empty Wastebin"
                        || label.starts_with("Replace"));
    button->setVariant(destructive ? ButtonVariant::Destructive : ButtonVariant::Outline);
    button->setOnClick([this, result] { closeDialog(result); });
    if (label == "Cancel")
      safeFocus = button->inputArea();
    row->addChild(std::move(button));
  }
  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  m_dialogSpacer = spacer.get();
  m_dialogRoot->addChild(std::move(spacer));
  auto footer = std::make_unique<Flex>();
  footer->setDirection(FlexDirection::Vertical);
  footer->setAlign(FlexAlign::Stretch);
  footer->setGap(8.0F);
  auto divider = std::make_unique<Separator>();
  divider->setOrientation(SeparatorOrientation::HorizontalRule);
  divider->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
  m_dialogDivider = footer->addChild(std::move(divider));
  footer->addChild(std::move(row));
  m_dialogFooter = footer.get();
  m_dialogRoot->addChild(std::move(footer));
  if (m_dialogResizable) {
    const std::array<std::uint32_t, 8> edges{
        XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,      XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
        XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
        XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
    };
    const std::array<std::uint32_t, 8> cursors{
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
    };
    for (std::size_t i = 0; i < edges.size(); ++i) {
      auto edge = std::make_unique<InputArea>();
      edge->setParticipatesInLayout(false);
      edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      edge->setCursorShape(cursors[i]);
      edge->setOnPress([this, side = edges[i]](const InputArea::PointerData& data) {
        if (data.pressed && m_dialogSurface)
          xdg_toplevel_resize(m_dialogSurface->xdgToplevel(), m_wayland.seat(), data.serial, side);
      });
      m_dialogResizeEdges[i] = edge.get();
      m_dialogRoot->addChild(std::move(edge));
    }
    m_dialogDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, shape);
    });
  }
  updateDialogTheme();
  m_dialogDispatcher.setSceneRoot(m_dialogRoot.get());
  if (m_symlinkDialogName)
    m_dialogDispatcher.setFocus(m_symlinkDialogName->inputArea());
  else if (m_textPrompt && m_dialogAlternativeNameInput)
    m_dialogDispatcher.setFocus(m_dialogAlternativeNameInput->inputArea());
  else if (safeFocus != nullptr)
    m_dialogDispatcher.setFocus(safeFocus);
}

void FileManager::updateDialogTheme() {
  if (!m_dialogRoot)
    return;
  const auto mode = m_configService.config().shell.panel.transparencyMode;
  const float background = detachedPanelBackgroundOpacityForTransparencyMode(mode);
  m_dialogRoot->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, background));
  for (auto* card : m_dialogCards)
    card->setFill(
        colorSpecFromRole(ColorRole::SurfaceContainer, panelCardOpacityForTransparencyMode(mode, background))
    );
}

void FileManager::prepareDialogFrame(bool, bool needsLayout) {
  if (!m_dialogSurface || m_dialogSurface->width() == 0 || m_dialogSurface->height() == 0)
    return;
  m_renderContext.makeCurrent(m_dialogSurface->renderTarget());
  auto& renderer = m_dialogSurface->renderTarget().renderer();
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_dialogRoot->setSize(static_cast<float>(m_dialogSurface->width()), static_cast<float>(m_dialogSurface->height()));
    m_dialogRoot->layout(renderer);
    if (m_dialogMoveHandle && m_dialogTitleRow) {
      m_dialogMoveHandle->setPosition(0.0F, 0.0F);
      m_dialogMoveHandle->setSize(m_dialogTitleRow->width(), m_dialogTitleRow->height());
      m_dialogMoveHandle->layout(renderer);
    }
    if (m_dialogWarningStyle) {
      m_deleteWarning->setSourceFile(renderer, kDeleteWarning, 72);
    }
    m_dialogTopAccent->setPosition(2.0F, 2.0F);
    m_dialogTopAccent->load(renderer);
    const float cornerX = m_dialogRoot->width() - m_dialogCornerAccent->width() - 7.0F;
    float dividerX = 0.0F;
    float dividerY = 0.0F;
    Node::absolutePosition(m_dialogDivider, dividerX, dividerY);
    const float cornerY = dividerY - m_dialogCornerAccent->height() - 5.0F;
    m_dialogCornerAccent->setVisible(cornerX >= 2.0F && cornerY >= 2.0F);
    m_dialogCornerAccent->setPosition(cornerX, cornerY);
    m_dialogCornerAccent->load(renderer);
    if (m_dialogResizable) {
      const float width = m_dialogRoot->width();
      const float height = m_dialogRoot->height();
      const std::array<std::array<float, 4>, 8> bounds{{
          {10.0F, 0.0F, width - 20.0F, 10.0F},
          {10.0F, height - 10.0F, width - 20.0F, 10.0F},
          {0.0F, 10.0F, 10.0F, height - 20.0F},
          {width - 10.0F, 10.0F, 10.0F, height - 20.0F},
          {0.0F, 0.0F, 10.0F, 10.0F},
          {width - 10.0F, 0.0F, 10.0F, 10.0F},
          {0.0F, height - 10.0F, 10.0F, 10.0F},
          {width - 10.0F, height - 10.0F, 10.0F, 10.0F}
      }};
      for (std::size_t i = 0; i < bounds.size(); ++i) {
        m_dialogResizeEdges[i]->setPosition(bounds[i][0], bounds[i][1]);
        m_dialogResizeEdges[i]->setSize(bounds[i][2], bounds[i][3]);
        m_dialogResizeEdges[i]->layout(renderer);
      }
    }
  }
}

void FileManager::syncTooltipHover() {
  m_dispatcher.syncPointerHover();
  if (m_surface)
    TooltipManager::instance().onHoverChange(
        m_dispatcher.hoveredArea(), m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
    );
}

void FileManager::handlePointer(const PointerEvent& event) {
  if (m_surface && event.surface == m_surface->wlSurface()) {
    if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)
      TooltipManager::instance().setPointerPosition(static_cast<float>(event.sx), static_cast<float>(event.sy));
    // Menu routing below can consume the leave before the main dispatcher sees it.
    if (event.type == PointerEvent::Type::Leave)
      m_dispatcher.pointerLeave();
    if (event.type == PointerEvent::Type::Axis || (event.type == PointerEvent::Type::Button && event.pressed))
      TooltipManager::instance().forceDestroy();
  } else if (event.type == PointerEvent::Type::Enter) {
    m_dispatcher.pointerLeave();
  }
  dispatchPointer(event);
  syncTooltipHover();
}

void FileManager::dispatchPointer(const PointerEvent& event) {
  if (m_editingLocation
      && m_locationInput != nullptr
      && m_surface
      && event.surface == m_surface->wlSurface()
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float inputX = 0.0F;
    float inputY = 0.0F;
    Node::absolutePosition(m_locationInput, inputX, inputY);
    if (filemanager::pointerOutsideRect(
            static_cast<float>(event.sx), static_cast<float>(event.sy), inputX, inputY, m_locationInput->width(),
            m_locationInput->height()
        ))
      deferCancelLocationEdit();
    else if (event.button == BTN_RIGHT && m_contextMenuPopup && m_surface) {
      const bool selected = m_locationInput->hasTextSelection();
      auto* clipboard = Input::textClipboard();
      const bool hasClipboardText = clipboard != nullptr && clipboard->clipboardText().has_value();
      std::vector<ContextMenuControlEntry> entries{
          {.id = 90, .label = "Cut", .enabled = selected},
          {.id = 91, .label = "Copy", .enabled = selected},
          {.id = 92,
           .label = "Paste",
           .enabled = hasClipboardText,
           .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
           .actionGlyph = "U+EA6F"},
          {.id = 93, .label = "Select All", .enabled = !m_locationInput->value().empty()},
      };
      m_dispatcher.pointerLeave();
      m_contextMenuPopup->open(ContextMenuPopupRequest{
          .entries = std::move(entries),
          .anchor = PopupAnchorRect{static_cast<std::int32_t>(event.sx), static_cast<std::int32_t>(event.sy), 1, 1},
          .parent = PopupSurfaceParent{
              .xdgSurface = m_surface->xdgSurface(),
              .output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output,
              .wlSurface = m_surface->wlSurface(),
          },
          .inputSerial = event.serial,
      });
      return;
    }
  }
  if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_RIGHT)
    TooltipManager::instance().forceDestroy();
  if (m_contextMenuPopup
      && m_contextMenuPopup->isOpen()
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && !m_contextMenuPopup->ownsSurface(event.surface)) {
    if (m_applicationMenuOpen && !m_applicationMenuCollapsed && !m_dialogSurface && m_surface
        && event.surface == m_surface->wlSurface() && event.button == BTN_LEFT) {
      for (std::size_t i = 0; i < m_applicationMenuButtons.size(); ++i) {
        auto* button = m_applicationMenuButtons[i];
        if (i == *m_applicationMenuOpen || !button
            || !button->containsScenePoint(static_cast<float>(event.sx), static_cast<float>(event.sy)))
          continue;
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(button, x, y);
        m_dispatcher.cancelPointerCapture();
        m_lastInputSerial = event.serial;
        if (openApplicationMenu(x, y + button->height(), i))
          return;
        break;
      }
    }
    m_contextMenuPopup->close();
    m_dispatcher.cancelPointerCapture();
    return;
  }
  if (m_contextMenuPopup && m_contextMenuPopup->isOpen() && m_contextMenuPopup->onPointerEvent(event))
    return;
  if (event.type == PointerEvent::Type::Button && event.pressed)
    m_lastInputSerial = event.serial;
  if (m_dialogSurface) {
    if (event.surface == m_dialogSurface->wlSurface()) {
      const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      switch (event.type) {
      case PointerEvent::Type::Enter:
        m_dialogDispatcher.pointerEnter(x, y, event.serial);
        break;
      case PointerEvent::Type::Leave:
        m_dialogDispatcher.pointerLeave();
        break;
      case PointerEvent::Type::Motion:
        m_dialogDispatcher.pointerMotion(x, y, event.serial);
        break;
      case PointerEvent::Type::Button:
        if (event.button == BTN_LEFT)
          m_dialogDispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
        break;
      case PointerEvent::Type::Axis:
        m_dialogDispatcher.pointerAxis(
            x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
            event.axisLines, event.axisGestureSerial
        );
        break;
      }
    }
    return;
  }
  if (m_progressSurface && event.surface == m_progressSurface->wlSurface()) {
    const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_progressDispatcher.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_progressDispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_progressDispatcher.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      if (event.button == BTN_LEFT)
        m_progressDispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      break;
    case PointerEvent::Type::Axis:
      m_progressDispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
    return;
  }
  if (!m_surface || event.surface != m_surface->wlSurface())
    return;
  const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (m_renamePath && m_inlineEditArea && event.type == PointerEvent::Type::Button && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float editorX = 0.0F;
    float editorY = 0.0F;
    Node::absolutePosition(m_inlineEditArea, editorX, editorY);
    if (filemanager::pointerOutsideRect(x, y, editorX, editorY, m_inlineEditArea->width(), m_inlineEditArea->height())) {
      m_dispatcher.setFocus(m_inlineEditArea);
      m_dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true, false);
      if (m_renamePath)
        cancelRename();
    }
  }
  if (handleFileHeaderPointer(event, x, y))
    return;
  if (m_statusDivider && m_statusStrip) {
    float dx = 0.0F, dy = 0.0F;
    Node::absolutePosition(m_statusDivider, dx, dy);
    const bool over =
        x >= dx && x <= dx + m_statusDivider->width() && y >= dy - 2.0F && y <= dy + m_statusDivider->height() + 2.0F;
    if ((over || m_resizingStatus) && m_pointerSerial)
      m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE);
    if (over && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_statusDivider->setFill(colorSpecFromRole(ColorRole::Primary, m_resizingStatus ? 1.0F : 0.62F));
      m_surface->requestRedraw();
    } else if (!m_resizingStatus) {
      m_statusDivider->setFill(clearColorSpec());
    }
    if (m_resizingStatus)
      m_statusDivider->setFill(colorSpecFromRole(ColorRole::Primary));
    if (over && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed) {
      m_dispatcher.cancelPointerCapture();
      m_resizingStatus = true;
      m_statusDragY = y;
      m_statusDragHeight = m_statusHeight;
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Motion) {
      m_statusHeight = std::clamp(m_statusDragHeight + m_statusDragY - y, kStatusMinHeight, kStatusMaxHeight);
      const float fontSize =
          12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
      m_statusStrip->setMinHeight(m_statusHeight);
      m_statusStrip->setMaxHeight(m_statusHeight);
      m_statusStrip->setPadding(std::clamp((m_statusHeight - fontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
      m_statusLabel->setFontSize(fontSize);
      m_statusDetailLabel->setFontSize(fontSize);
      m_statusFreeLabel->setFontSize(fontSize);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingStatus = false;
      m_configService.setStateString("files", "status_height", std::to_string(m_statusHeight));
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (event.type == PointerEvent::Type::Leave && (m_draggingFiles || m_dragPending)) {
    if (!m_transfer.dragging()) cancelFileGesture();
    else {
      clearDragFeedback();
      if (m_dragPreview) m_dragPreview->setVisible(false);
    }
    m_dispatcher.pointerLeave();
    return;
  }
  if (event.type == PointerEvent::Type::Enter && event.serial)
    m_pointerSerial = event.serial;
  if (m_transfer.dragging() && !m_transfer.dragDropped()) {
    if (event.type == PointerEvent::Type::Axis) {
      m_dispatcher.pointerAxis(
          m_dragPointerX, m_dragPointerY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete,
          event.axisValue120, event.axisLines, event.axisGestureSerial);
      m_transfer.updateDragTarget();
    }
    return;
  }
  if (m_transfer.dragDropped() && (m_dragPending || m_draggingFiles)) cancelFileGesture();
  if (event.type == PointerEvent::Type::Axis && (m_keyboardModifiers & KeyMod::Ctrl) && m_grid && !m_renamePath) {
    float gx = 0, gy = 0;
    Node::absolutePosition(m_grid, gx, gy);
    if (x >= gx && x < gx + m_grid->width() && y >= gy && y < gy + m_grid->height()) {
      const double delta = event.axisValue120 != 0 ? event.axisValue120
          : event.axisDiscrete != 0                ? event.axisDiscrete
                                                   : event.axisValue;
      if (delta != 0 && event.axis == 0) {
        adjustViewScale(m_viewScale + (delta < 0 ? filemanager::kFileViewZoomStep : -filemanager::kFileViewZoomStep));
      }
      return;
    }
  }
  auto rowAt = [&]() -> std::optional<std::size_t> {
    if (!m_placesScroll)
      return std::nullopt;
    float vx = 0, vy = 0;
    Node::absolutePosition(m_placesScroll, vx, vy);
    if (x < vx || y < vy || x >= vx + m_placesScroll->width() || y >= vy + m_placesScroll->height())
      return std::nullopt;
    for (std::size_t i = 0; i < m_placeRows.size(); ++i) {
      const auto& [row, place] = m_placeRows[i];
      bool visible = true;
      for (Node* node = row; node; node = node->parent())
        if (!node->visible()) {
          visible = false;
          break;
        }
      if (!visible)
        continue;
      float rx = 0, ry = 0;
      Node::absolutePosition(row, rx, ry);
      if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height())
        return i;
    }
    return std::nullopt;
  };
  if (m_reorderId) {
    const auto source =
        std::ranges::find_if(m_placeRows, [this](const auto& pair) { return pair.second.identity == *m_reorderId; });
    if (event.type == PointerEvent::Type::Motion) {
      if (std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F)
        m_reordering = true;
      m_reorderBefore.reset();
      if (m_reordering && source != m_placeRows.end()) {
        const auto& place = source->second;
        m_reorderBefore = placeInsertionAt(
            x, y,
            place.personal      ? "Personal"
                : place.network ? "Network"
                                : "Devices",
            false
        );
      }
      showPlaceInsertion(m_reorderBefore);
      if (m_pointerSerial && m_reordering)
        m_wayland.setCursorShape(
            m_pointerSerial,
            m_reorderBefore ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
        );
      m_surface->requestRedraw();
      return;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_reordering && m_reorderBefore)
        reorderPlace(*m_reorderId, *m_reorderBefore);
      else if (!m_reordering && source != m_placeRows.end()) {
        const auto place = source->second;
        if (place.mounted)
          navigate(place.path);
      }
      m_reorderId.reset();
      m_reorderBefore.reset();
      m_reordering = false;
      showPlaceInsertion(std::nullopt);
      m_dispatcher.cancelPointerCapture();
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      m_surface->requestRedraw();
      return;
    }
    return;
  }
  if (m_dragPending || m_draggingFiles) {
    if (event.type == PointerEvent::Type::Axis) {
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      auto motion = event;
      motion.type = PointerEvent::Type::Motion;
      handlePointer(motion);
      return;
    }
    if (event.type == PointerEvent::Type::Motion) {
      if (!m_draggingFiles && std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F) {
        m_draggingFiles = true;
        m_dragSources = selectedPaths();
        if (m_dragSources.empty()) { cancelFileGesture(); return; }
        m_lastPointerIndex.reset();
        m_dispatcher.pointerLeave();
        TooltipManager::instance().forceDestroy();
        try {
          auto payload = filemanager::encodeFileTransfer(m_dragSources, false);
          // Clipboard cut metadata is not drag action negotiation.
          payload.erase("application/x-kde-cutselection");
          payload.erase("x-special/gnome-copied-files");
          m_dragPointerX = x;
          m_dragPointerY = y;
          clearNativeDragPreview();
          m_nativePreviewSurface = wl_compositor_create_surface(m_wayland.compositor());
          if (m_transfer.startDrag(std::move(payload), m_surface->wlSurface(), m_dragPressSerial,
                                   nullptr, m_nativePreviewSurface)) {
            scheduleNativeDragPreview();
            return;
          }
          clearNativeDragPreview();
        } catch (const std::exception& error) {
          setStatus(std::string("Cannot start file drag: ") + error.what());
          cancelFileGesture();
          return;
        }
      }
      if (m_draggingFiles) {
        clearDragFeedback();
        m_dragPointerX = x;
        m_dragPointerY = y;
        const bool folders = std::ranges::all_of(m_dragSources, [](const auto& source) {
          std::error_code error;
          return std::filesystem::is_directory(source, error);
        });
        m_pinBefore = folders ? placeInsertionAt(x, y, "Personal", true) : std::nullopt;
        showPlaceInsertion(m_pinBefore);
        auto destination = m_pinBefore ? std::nullopt : dropDestination(x, y);
        if (destination)
          destination = filemanager::checkedDropTarget(m_dragSources, *destination);
        if (destination) {
          if (auto row = rowAt()) {
            m_dropPlace = m_placeRows[*row].first;
            m_dropPlace->setSelected(true);
          } else if (auto index = m_grid->indexAtSurface(x, y)) {
            m_dropIndex = index;
            m_grid->notifyItemChanged(*index);
          }
        }
        m_dragTargetValid = m_pinBefore.has_value() || destination.has_value();
        setStatus(
            m_pinBefore ? "Pin folders in Places · Escape cancels"
                        : std::string(
                              ((m_keyboardModifiers & KeyMod::Ctrl)
                               || (destination
                                   && !(m_keyboardModifiers & KeyMod::Shift)
                                   && filemanager::copyDropByDefault(m_dragSources, *destination)))
                                  ? "Copy "
                                  : "Move "
                          )
                    + std::to_string(m_dragSources.size())
                    + " items"
                    + (destination ? " to " + destination->string() : " — no drop target")
                    + " · Escape cancels"
        );
        if (m_pointerSerial)
          m_wayland.setCursorShape(
              m_pointerSerial,
              m_dragTargetValid ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
          );
        m_surface->requestUpdate();
      }
      return;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_draggingFiles) {
        const bool folders = std::ranges::all_of(m_dragSources, [](const auto& source) {
          std::error_code error;
          return std::filesystem::is_directory(source, error);
        });
        const auto pin = folders ? placeInsertionAt(x, y, "Personal", true) : std::nullopt;
        auto destination = pin ? std::nullopt : dropDestination(x, y);
        if (pin)
          pinFolders(m_dragSources, *pin);
        else if (destination && !m_dragSources.empty() && m_deviceOperations.empty() && !m_operations.replaying()) {
          const auto target = filemanager::checkedDropTarget(m_dragSources, *destination);
          if (target) {
            const bool copy = (m_keyboardModifiers & KeyMod::Ctrl)
                || (!(m_keyboardModifiers & KeyMod::Shift) && filemanager::copyDropByDefault(m_dragSources, *target));
            if (copy)
              m_operations.copy(m_dragSources, target->string());
            else
              m_operations.move(m_dragSources, target->string());
          } else
            setStatus("Cannot drop items into themselves or their own subfolders");
        } else
          setStatus("Drop cancelled");
      } else if (m_dragIndex && *m_dragIndex < m_entries.size()) {
        if (!(m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift))) {
          m_selectedPaths = {m_entries[*m_dragIndex].absPath.string()};
          m_selectedPath = m_entries[*m_dragIndex].absPath;
          m_grid->notifyDataChanged();
        }
        if (!(m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift))) {
          m_pointerClickTime = event.time;
          activatePointerItem(*m_dragIndex);
        } else
          m_lastPointerIndex.reset();
      }
      m_dragPending = false;
      m_draggingFiles = false;
      m_dragIndex.reset();
      m_dragSources.clear();
      m_pinBefore.reset();
      showPlaceInsertion(std::nullopt);
      if (m_dragPreview)
        m_dragPreview->setVisible(false);
      clearDragFeedback();
      m_dispatcher.cancelPointerCapture();
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      return;
    }
  }
  if (m_sidebarDivider != nullptr && m_sidebar != nullptr && m_sidebarDivider->visible()) {
    float dividerX = 0, dividerY = 0;
    Node::absolutePosition(m_sidebarDivider, dividerX, dividerY);
    const bool overDivider = x >= dividerX - 1.5F
        && x <= dividerX + m_sidebarDivider->width() + 1.5F
        && y >= dividerY
        && y <= dividerY + m_sidebarDivider->height();
    const bool feedback = overDivider || m_resizingSidebar;
    if (overDivider
        && !m_resizingSidebar
        && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_sidebarDivider->setFill(colorSpecFromRole(ColorRole::Primary, 0.62F));
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      m_surface->requestRedraw();
      return;
    }
    m_sidebarDivider->setFill(
        feedback ? colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : 0.62F) : clearColorSpec()
    );
    if (m_resizingSidebar || overDivider) {
      m_sidebarDivider->setFill(colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : 0.62F));
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed && overDivider) {
      m_dispatcher.cancelPointerCapture();
      m_resizingSidebar = true;
      m_sidebarDragOffset = x;
      m_sidebarDragWidth = m_sidebar->width();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Motion) {
      const float maxWidth =
          std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300));
      // Both coordinates are surface-local logical pixels. Apply one delta from
      // the press, independent of layout origin, render scale and intermediate clamps.
      m_sidebarWidth = std::clamp(m_sidebarDragWidth + x - m_sidebarDragOffset, kSidebarMinWidth, maxWidth);
      m_sidebar->setMinWidth(m_sidebarWidth);
      m_sidebar->setMaxWidth(m_sidebarWidth);
      for (const auto& capacity : m_placeCapacities)
        if (capacity.bar)
          capacity.bar->setSize(std::max(80.0F, m_sidebarWidth - 28), 4);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingSidebar = false;
      m_configService.setStateString("files", "sidebar_width", std::to_string(m_sidebarWidth));
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (m_grid && m_grid->rubberBandActive()) {
    if (event.type == PointerEvent::Type::Motion)
      m_grid->updateRubberBand(x, y);
    else if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed)
      m_grid->endRubberBand();
    else if (event.type == PointerEvent::Type::Axis)
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
    return;
  }
  if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed) {
    if (auto row = rowAt()) {
      const auto& place = m_placeRows[*row].second;
      {
        m_reorderId = place.identity;
        m_dragStartX = x;
        m_dragStartY = y;
        m_reordering = false;
        m_dispatcher.cancelPointerCapture();
        return;
      }
    }
    if (!m_browsingTrash && m_grid && !m_renamePath && m_deviceOperations.empty()) {
      if (auto index = m_grid->indexAtSurface(x, y)) {
        m_dragIndex = index;
        m_dragStartX = x;
        m_dragStartY = y;
        m_dragPending = true;
        m_dragPressSerial = event.serial;
        const bool preserve = m_selectedPaths.contains(m_entries[*index].absPath.string())
            && !(m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift));
        m_restoringSelection = true;
        if (!preserve)
          m_grid->setSelectedIndex(std::nullopt, false);
        m_restoringSelection = preserve;
        m_grid->setSelectedIndex(index, false);
        m_restoringSelection = false;
        m_dispatcher.setFocus(m_grid->focusArea());
        return;
      }
    }
  }
  if (event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && event.pressed
      && m_grid
      && !m_renamePath) {
    m_rubberBaseline = m_selectedPaths;
    m_rubberSelectedPath = m_selectedPath;
    m_rubberAnchor = m_selectionAnchor;
    m_rubberModifiers = m_keyboardModifiers;
    if (m_grid->beginRubberBand(x, y)) {
      m_dispatcher.pointerLeave();
      TooltipManager::instance().forceDestroy();
      m_selectionTick = std::chrono::steady_clock::now();
      m_selectionTimer.startRepeating(std::chrono::milliseconds(16), [this] {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - m_selectionTick).count();
        m_selectionTick = now;
        if (m_grid)
          m_grid->advanceRubberBandScroll(std::min(seconds, 0.05F));
        if (m_surface)
          m_surface->requestLayout();
      });
      m_lastPointerIndex.reset();
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_grid->focusArea());
      return;
    }
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    if (event.button == BTN_LEFT || event.button == BTN_RIGHT) {
      if (event.pressed && event.button == BTN_LEFT)
        m_pointerClickTime = event.time;
      m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      // The virtual canvas only covers populated rows. The remainder of the
      // grid viewport still belongs to the directory background.
      if (event.button == BTN_RIGHT && event.pressed && m_grid && !m_contextMenuPopup->isOpen()) {
        float gridX = 0.0F;
        float gridY = 0.0F;
        Node::absolutePosition(m_grid, gridX, gridY);
        if (x >= gridX && x < gridX + m_grid->width() && y >= gridY && y < gridY + m_grid->height())
          openEmptyContextMenu(x, y);
      }
    }
    break;
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
}

void FileManager::handleKeyboard(const KeyboardEvent& event) {
  if (!event.pressed || event.preedit || !m_surface || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
    return;
  if (event.sym == XKB_KEY_Escape) {
    m_quitConfirmation = false;
    if (m_contextMenuPopup && m_contextMenuPopup->isOpen()) {
      m_contextMenuPopup->close();
      return;
    }
    if (m_renamePath) {
      cancelRename();
      return;
    }
    if (m_inlineEditArea) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
      return;
    }
    if (m_editingLocation) {
      setLocationEditMode(false);
      return;
    }
    if (m_transfer.dragging() && !m_transfer.dragDropped()) {
      m_transfer.cancelDrag();
      return;
    }
    if (m_dragPending
        || m_draggingFiles
        || m_reorderId
        || m_resizingFileColumn
        || m_fileColumnDrag
        || m_resizingSidebar
        || m_resizingStatus
        || (m_grid && m_grid->rubberBandActive())) {
      if (m_grid)
        m_grid->endRubberBand(true);
      if (m_resizingFileColumn || m_fileColumnDrag) {
        m_dispatcher.cancelPointerCapture();
        cancelFileHeaderGesture();
      }
      m_dragPending = m_draggingFiles = m_reordering = m_resizingSidebar = m_resizingStatus = false;
      m_dragIndex.reset();
      m_dragSources.clear();
      m_reorderId.reset();
      m_reorderBefore.reset();
      m_pinBefore.reset();
      if (m_dragPreview)
        m_dragPreview->setVisible(false);
      clearDragFeedback();
      return;
    }
    if (!m_selectedPaths.empty())
      changeSelection(SelectionChange::None);
    return;
  }
  if (m_locationInput != nullptr && m_editingLocation
      && m_dispatcher.focusedArea() == m_locationInput->inputArea()) {
    if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
      navigate(filemanager::expandHomePath(m_locationInput->value(), g_get_home_dir()));
      setLocationEditMode(false);
    } else {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }
    return;
  }
  if (!m_deviceOperations.empty()
      && (event.sym == XKB_KEY_Delete
          || event.sym == XKB_KEY_F2
          || ((event.modifiers & KeyMod::Ctrl) && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_n)))) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (m_dragPending
      || m_draggingFiles
      || m_reorderId
      || m_resizingFileColumn
      || m_fileColumnDrag
      || m_resizingSidebar
      || (m_grid && m_grid->rubberBandActive()))
    return;
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_q) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 30007});
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Shift)) == KeyMod::Ctrl && event.sym == XKB_KEY_n) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 30001});
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_l) {
    setLocationEditMode(true);
    return;
  }
  if (m_inlineEditArea != nullptr && m_dispatcher.focusedArea() == m_inlineEditArea) {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl
      && (event.sym == XKB_KEY_a || event.sym == XKB_KEY_A)) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = kSelectAll});
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl
      && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = (event.modifiers & KeyMod::Shift) ? kRedo : kUndo});
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_c) {
    copyToDesktopClipboard(false);
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_x) {
    copyToDesktopClipboard(true);
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_v) {
    pasteClipboard();
    return;
  }
  if ((event.modifiers & KeyMod::Alt) && event.sym == XKB_KEY_Return && m_selectedPath) {
    if (m_selectedPaths.size() != 1) {
      setStatus("Properties supports one selected item at a time");
      return;
    }
    openProperties(*m_selectedPath);
    return;
  }
  if (event.sym == XKB_KEY_F2) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 4});
    return;
  }
  if (event.sym == XKB_KEY_n && (event.modifiers & (KeyMod::Ctrl | KeyMod::Shift)) == (KeyMod::Ctrl | KeyMod::Shift)) {
    if (m_browsingTrash)
      return;
    activateContextMenuEntry(ContextMenuControlEntry{.id = 111});
    return;
  }
  if (event.sym == XKB_KEY_Delete) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = (event.modifiers & KeyMod::Shift) ? 6 : 5});
    return;
  }
  if (event.sym == XKB_KEY_BackSpace) {
    if (m_browsingTrash && m_path == std::filesystem::path("trash:/"))
      return;
    if (m_path.has_parent_path() && m_path.parent_path() != m_path)
      navigate(m_path.parent_path());
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl) {
    switch (event.sym) {
    case XKB_KEY_plus:
    case XKB_KEY_KP_Add:
      activateContextMenuEntry({.id = kZoomIn});
      return;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      activateContextMenuEntry({.id = kZoomOut});
      return;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
      activateContextMenuEntry({.id = kResetZoom});
      return;
    default:
      break;
    }
  }
  if (event.sym == XKB_KEY_r && (event.modifiers & KeyMod::Ctrl) != 0U) {
    activateContextMenuEntry({.id = 13});
    return;
  }
  if (m_grid == nullptr)
    return;
  if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
    activateSelected();
    return;
  }
  const auto current = m_grid->selectedIndex();
  const auto selected = current.value_or(0);
  if (event.sym == XKB_KEY_Down && !current && !m_entries.empty())
    m_grid->setSelectedIndex(0);
  else if (event.sym == XKB_KEY_Down && selected + 1 < m_entries.size())
    m_grid->setSelectedIndex(selected + 1);
  else if (event.sym == XKB_KEY_Up && selected > 0)
    m_grid->setSelectedIndex(selected - 1);
  else
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}
