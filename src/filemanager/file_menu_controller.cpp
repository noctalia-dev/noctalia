#include "filemanager/file_menu_controller.h"

#include "config/config_service.h"
#include "core/files/resource_paths.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/property_operations.h"
#include "render/render_context.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <gio/gdesktopappinfo.h>
#include <linux/input-event-codes.h>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace filemanager {

  namespace {

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
          OfficeType{
              114, "Document…", "application/vnd.oasis.opendocument.text", "templates/filemanager/office/blank.odt"
          },
          OfficeType{
              115, "Spreadsheet…", "application/vnd.oasis.opendocument.spreadsheet",
              "templates/filemanager/office/blank.ods"
          },
          OfficeType{
              116, "Presentation…", "application/vnd.oasis.opendocument.presentation",
              "templates/filemanager/office/blank.odp"
          }
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
        children.insert(
            children.end(), std::make_move_iterator(officeEntries.begin()), std::make_move_iterator(officeEntries.end())
        );
      }
      children.push_back({.separator = true});
      if (symlinkDirectory) {
        children.push_back(
            {.id = menu_id::kSelectionSymlink,
             .label = *symlinkDirectory ? "Symlink to Folder" : "Symlink to File",
             .actionGlyph = "link"}
        );
      }
      children.push_back({.id = menu_id::kManualSymlink, .label = "Symlink…", .actionGlyph = "link"});
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

  } // namespace

  FileMenuController::FileMenuController(
      WaylandConnection& wayland, RenderContext& renderContext, AnimationManager& menuAnimations,
      InputDispatcher& dispatcher, FileManagerSettings& settings, FileOperationManager& operations,
      TabManager& tabs, FileListHeaderController& listHeader, PlacesSidebarController& places,
      ChecksumDialogController& checksumDialog, NativeDataTransfer& transfer, Callbacks callbacks
  )
      : m_wayland(wayland),
        m_renderContext(renderContext),
        m_menuAnimations(menuAnimations),
        m_dispatcher(dispatcher),
        m_settings(settings),
        m_operations(operations),
        m_tabs(tabs),
        m_listHeader(listHeader),
        m_places(places),
        m_checksumDialog(checksumDialog),
        m_transfer(transfer),
        m_callbacks(std::move(callbacks)) {}

  FileMenuController::~FileMenuController() {
    m_menuAnimations.cancel(m_menuUnderlineAnimation);
    closeAll();
  }

  void FileMenuController::loadInitialPreference() { m_showApplicationMenuBar = m_settings.showApplicationMenuBar(); }

  void FileMenuController::initializePopups(
      ConfigService& configService, LayerPopupHostRegistry& pickerHosts,
      std::function<void(const ContextMenuControlEntry&)> onActivate
  ) {
    m_applicationPicker = std::make_unique<SearchPickerPopup>();
    m_applicationPicker->initialize(m_wayland, configService, m_renderContext, pickerHosts);

    m_contextMenuPopup = std::make_unique<ContextMenuPopup>(m_wayland, m_renderContext);
    m_contextMenuPopup->setHighlightOnOpen(false);
    m_contextMenuPopup->setOnActivate(std::move(onActivate));
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
  }

  void FileMenuController::attachMenuBarUi(Flex& appBar, float toolbarHeight) {
    auto menuItems = std::make_unique<Flex>();
    menuItems->setAlign(FlexAlign::Center);
    menuItems->setGap(Style::spaceXs);
    m_appMenuItems = menuItems.get();

    auto fileMenu = std::make_unique<Button>();
    fileMenu->setText("File");
    fileMenu->setVariant(ButtonVariant::Tab);
    fileMenu->setControlHeight(toolbarHeight);
    fileMenu->setPadding(0.0F, Style::spaceSm);
    m_fileMenuButton = fileMenu.get();
    fileMenu->setOnClick([this] {
      float x = 0, y = 0;
      Node::absolutePosition(m_fileMenuButton, x, y);
      openApplicationMenu(x, y + m_fileMenuButton->height());
    });
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
      const auto menuIndex = i + 1;
      auto item = std::make_unique<Button>();
      item->setText(menuNames[i]);
      item->setVariant(ButtonVariant::Tab);
      item->setFontSize(Style::fontSizeCaption);
      item->setControlHeight(toolbarHeight);
      item->setPadding(0.0F, Style::spaceSm);
      m_applicationMenuButtons[menuIndex] = item.get();
      if (menuIndex <= menu_id::kHelpMenuIndex) {
        item->setOnClick([this, index = menuIndex] {
          const auto* menu = m_applicationMenuButtons[index];
          float x = 0, y = 0;
          Node::absolutePosition(menu, x, y);
          openApplicationMenu(x, y + menu->height(), index);
        });
      } else {
        item->setOnClick([] {});
      }
      item->setOnEnter([this, index = menuIndex] {
        m_applicationMenuHovered = index;
        updateApplicationMenuUnderline();
      });
      item->setOnLeave([this, index = menuIndex] {
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
    appBar.addChild(std::move(menuItems));

    auto compactMenu = std::make_unique<Button>();
    compactMenu->setVariant(ButtonVariant::Ghost);
    compactMenu->setControlHeight(toolbarHeight);
    compactMenu->setPadding(5.0F);
    compactMenu->setGlyph("menu-2");
    compactMenu->setTooltip("Open application menu");
    m_compactMenuButton = compactMenu.get();
    compactMenu->setOnClick([this] {
      float x = 0, y = 0;
      Node::absolutePosition(m_compactMenuButton, x, y);
      openApplicationMenu(x, y + m_compactMenuButton->height());
    });
    appBar.addChild(std::move(compactMenu));

    if (m_showApplicationMenuBar) {
      m_compactMenuButton->setVisible(false);
      m_compactMenuButton->setParticipatesInLayout(false);
    } else {
      m_appMenuItems->setVisible(false);
      m_appMenuItems->setParticipatesInLayout(false);
    }
  }

  void FileMenuController::detachUi() {
    m_menuAnimations.cancel(m_menuUnderlineAnimation);
    m_menuUnderlineAnimation = 0;
    m_appMenuItems = nullptr;
    m_fileMenuButton = nullptr;
    m_applicationMenuButtons.fill(nullptr);
    m_applicationMenuUnderline = nullptr;
    m_compactMenuButton = nullptr;
    m_applicationMenuOpen.reset();
    m_applicationMenuHovered.reset();
  }

  void FileMenuController::closeAll() {
    closeContextMenu();
    closeApplicationPicker();
  }

  void FileMenuController::closeContextMenu() {
    if (m_contextMenuPopup)
      m_contextMenuPopup->close();
  }

  void FileMenuController::closeApplicationPicker() {
    if (m_applicationPicker)
      m_applicationPicker->close();
  }

  bool FileMenuController::isContextMenuOpen() const { return m_contextMenuPopup && m_contextMenuPopup->isOpen(); }

  bool FileMenuController::isApplicationPickerOpen() const {
    return m_applicationPicker && m_applicationPicker->isOpen();
  }

  bool FileMenuController::hasOpenPopup() const { return isContextMenuOpen() || isApplicationPickerOpen(); }

  bool FileMenuController::handleAppPickerPointer(const PointerEvent& event) {
    return m_applicationPicker && m_applicationPicker->onPointerEvent(event);
  }

  bool FileMenuController::handlePointer(const PointerEvent& event) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    const bool modalOpen = m_callbacks.isModalOpen && m_callbacks.isModalOpen();
    if (m_contextMenuPopup
        && m_contextMenuPopup->isOpen()
        && event.type == PointerEvent::Type::Button
        && event.pressed
        && !m_contextMenuPopup->ownsSurface(event.surface)) {
      if (m_applicationMenuOpen
          && !m_applicationMenuCollapsed
          && !modalOpen
          && surface
          && event.surface == surface->wlSurface()
          && event.button == BTN_LEFT) {
        for (std::size_t i = 0; i < m_applicationMenuButtons.size(); ++i) {
          auto* button = m_applicationMenuButtons[i];
          if (i == *m_applicationMenuOpen
              || !button
              || !button->containsScenePoint(static_cast<float>(event.sx), static_cast<float>(event.sy)))
            continue;
          float x = 0.0F;
          float y = 0.0F;
          Node::absolutePosition(button, x, y);
          m_dispatcher.cancelPointerCapture();
          if (m_callbacks.setLastInputSerial)
            m_callbacks.setLastInputSerial(event.serial);
          if (openApplicationMenu(x, y + button->height(), i))
            return true;
          break;
        }
      }
      m_contextMenuPopup->close();
      m_dispatcher.cancelPointerCapture();
      return true;
    }
    return m_contextMenuPopup && m_contextMenuPopup->isOpen() && m_contextMenuPopup->onPointerEvent(event);
  }

  bool FileMenuController::handleKeyboard(const KeyboardEvent& event) {
    if (m_applicationPicker
        && m_applicationPicker->isOpen()
        && m_wayland.lastKeyboardSurface() == m_applicationPicker->wlSurface()) {
      m_applicationPicker->onKeyboardEvent(event);
      return true;
    }
    if (m_contextMenuPopup && m_contextMenuPopup->isOpen()) {
      m_contextMenuPopup->onKeyboardEvent(event);
      return true;
    }
    return false;
  }

  void FileMenuController::syncResponsiveMenuBar(int surfaceWidth) {
    if (m_appMenuItems && m_compactMenuButton) {
      const bool collapse = !m_showApplicationMenuBar || surfaceWidth < 640;
      if (collapse != m_applicationMenuCollapsed) {
        m_applicationMenuCollapsed = collapse;
        m_appMenuItems->setVisible(!collapse);
        m_appMenuItems->setParticipatesInLayout(!collapse);
        m_compactMenuButton->setVisible(collapse);
        m_compactMenuButton->setParticipatesInLayout(collapse);
        updateApplicationMenuUnderline();
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
      }
    }
  }

  void FileMenuController::toggleMenuBarVisibility() {
    m_showApplicationMenuBar = !m_showApplicationMenuBar;
    persistApplicationMenuPreference();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void FileMenuController::persistApplicationMenuPreference() {
    if (!m_settings.setShowApplicationMenuBar(m_showApplicationMenuBar) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save menu preference");
  }

  void FileMenuController::updateApplicationMenuUnderline() {
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
          m_applicationMenuUnderline->setPosition(
              std::lerp(m_menuUnderlineStartX, targetX, progress), m_applicationMenuUnderline->y()
          );
          m_applicationMenuUnderline->setFrameSize(std::lerp(m_menuUnderlineStartWidth, targetWidth, progress), 2.0F);
          m_applicationMenuUnderline->setOpacity(std::lerp(startOpacity, targetOpacity, progress));
        },
        {}, this
    );
  }

  ContextMenuControlEntry FileMenuController::newItemMenu() const {
    const auto& tab = m_tabs.activeTab();
    const auto caps = tab.capabilities();
    const auto selection = m_callbacks.symlinkSelection ? m_callbacks.symlinkSelection() : std::nullopt;
    auto entry = createNewMenuEntry(selection ? std::optional(selection->second) : std::nullopt);
    if (tab.hasActiveAdminSession()) {
      const bool allowBasic =
          caps.canCreateBasic && !tab.session.browsingTrash() && tab.session.currentDirectoryWritable();
      entry.enabled = allowBasic;
      for (auto& child : entry.submenuEntries)
        child.enabled = allowBasic && (child.id == 111 || child.id == 112);
    }
    return entry;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::fileMenuEntries() const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const auto selectedPath = m_callbacks.selectedPath ? m_callbacks.selectedPath() : std::nullopt;
    const auto& selectedPaths = m_callbacks.selectedPaths();
    const auto selectedEntry =
        selectedPath ? std::ranges::find(session.entries(), *selectedPath, &FileEntry::absPath)
                     : session.entries().end();
    const bool selectedDirectory = selectedEntry != session.entries().end() && selectedEntry->isDir;
    bool selectedDirectorySaved = false;
    if (selectedDirectory)
      selectedDirectorySaved = m_places.isPersonalPlaceSaved(selectedEntry->absPath);
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kNewTab,
         .label = "New Tab",
         .shortcut = "Ctrl+T",
         .actionIconNames = {"tab-new-symbolic", "tab-new"},
         .actionGlyph = "plus"},
        {.id = 30001,
         .label = "New Window",
         .enabled = !session.browsingTrash(),
         .shortcut = "Ctrl+N",
         .actionIconNames = {"window-new-symbolic", "window-new"},
         .actionGlyph = "app-window"},
        newItemMenu(),
        {.separator = true},
        {.id = 30002,
         .label = "Open",
         .enabled = tab.hasActiveAdminSession() ? selectedDirectory : selectedEntry != session.entries().end(),
         .shortcut = "Enter",
         .actionIconNames = {"folder-open-symbolic", "folder-open"},
         .actionGlyph = "folder-open"},
        {.id = menu_id::kFileOpenInNewTab,
         .label = "Open in New Tab",
         .enabled = !session.browsingTrash() && selectedDirectory,
         .actionIconNames = {"tab-new-symbolic", "tab-new"},
         .actionGlyph = "plus"},
        {.id = 30003,
         .label = "Open With…",
         .enabled = caps.canLaunchApps
             && !session.browsingTrash()
             && selectedPaths.size() == 1
             && selectedEntry != session.entries().end()
             && !selectedEntry->isDir},
        {.id = 30004,
         .label = "Open in New Window",
         .enabled = !session.browsingTrash() && selectedDirectory,
         .actionIconNames = {"window-new-symbolic", "window-new"},
         .actionGlyph = "app-window"},
        {.separator = true},
        {.id = 30005,
         .label = "Add to Places",
         .enabled = caps.canPinToPlaces && !session.browsingTrash() && selectedDirectory && !selectedDirectorySaved,
         .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
         .actionGlyph = "bookmark-plus"},
        {.separator = true},
        {.id = 30006,
         .label = "Properties",
         .enabled = caps.canOpenProperties
             && !session.browsingTrash()
             && selectedPaths.size() == 1
             && selectedEntry != session.entries().end(),
         .shortcut = "Alt+Enter",
         .actionIconNames = {"document-properties-symbolic", "document-properties"},
         .actionGlyph = "file-info"},
        {.separator = true},
        {.id = menu_id::kReopenClosedTab,
         .label = "Reopen Closed Tab",
         .enabled = m_tabs.canReopenClosedTab(),
         .shortcut = "Ctrl+Shift+T"},
        {.id = menu_id::kCloseTab, .label = "Close Tab", .shortcut = "Ctrl+W"},
        {.id = 30007, .label = "Quit", .shortcut = "Ctrl+Q"},
    };
    for (auto& entry : entries)
      if (entry.id == 110)
        entry.enabled =
            (caps.canCreate || caps.canCreateBasic) && !session.browsingTrash() && session.currentDirectoryWritable();
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::editMenuEntries() const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const auto& selectedPaths = m_callbacks.selectedPaths();
    const bool hasSelection = !session.loading() && !selectedPaths.empty();
    const bool oneItem = hasSelection && selectedPaths.size() == 1;
    const bool canSelect = m_callbacks.canChangeSelection && m_callbacks.canChangeSelection();
    const bool canMutate = caps.canMutate && !m_places.hasActiveDeviceOperations() && !m_operations.replaying();
    const bool canPaste = (caps.canMutate || caps.canPasteAsAdministrator)
        && !m_places.hasActiveDeviceOperations()
        && !m_operations.replaying();
    const bool canRename = caps.canRename && !m_places.hasActiveDeviceOperations() && !m_operations.replaying();
    const bool modalOpen = m_callbacks.isModalOpen && m_callbacks.isModalOpen();
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const std::size_t pendingClipboard = m_callbacks.pendingClipboardReads ? m_callbacks.pendingClipboardReads() : 0;
    const bool canReplay =
        caps.canUndo && !m_places.hasActiveDeviceOperations() && !m_operations.replaying() && !modalOpen && !renaming
        && !m_transfer.busy() && pendingClipboard == 0;
    const bool local = !session.browsingTrash();
    const bool canCreate = caps.canCreate && local && canMutate && session.currentDirectoryWritable();
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kUndo,
         .label = m_operations.undoLabel(),
         .enabled = canReplay && m_operations.canUndo(),
         .shortcut = "Ctrl+Z",
         .actionGlyph = "arrow-back-up"},
        {.id = menu_id::kRedo,
         .label = m_operations.redoLabel(),
         .enabled = canReplay && m_operations.canRedo(),
         .shortcut = "Ctrl+Shift+Z",
         .actionGlyph = "arrow-forward-up"},
        {.separator = true},
        {.id = 2,
         .label = "Cut",
         .enabled = caps.canMutate && caps.canCopyItems && local && hasSelection && !m_places.hasActiveDeviceOperations(),
         .shortcut = "Ctrl+X",
         .actionGlyph = "scissors"},
        {.id = 1,
         .label = "Copy",
         .enabled = caps.canCopyItems && local && hasSelection && !m_places.hasActiveDeviceOperations(),
         .shortcut = "Ctrl+C",
         .actionGlyph = "copy"},
        {.id = 3, .label = "Paste", .enabled = local && canPaste, .shortcut = "Ctrl+V", .actionGlyph = "U+EA6F"},
        {.id = 59, .label = "Copy Location", .enabled = caps.canCopyItems && local && oneItem, .actionGlyph = "link"},
        {.separator = true},
        {.id = menu_id::kSelectAll,
         .label = "Select All",
         .enabled = canSelect && selectedPaths.size() < session.entries().size(),
         .shortcut = "Ctrl+A"},
        {.id = menu_id::kDeselectAll, .label = "Deselect All", .enabled = canSelect && hasSelection},
        {.id = menu_id::kInvertSelection, .label = "Invert Selection", .enabled = canSelect && !session.entries().empty()},
        {.separator = true},
    };
    if (session.browsingTrash()) {
      const bool restoring =
          canMutate && hasSelection && std::ranges::none_of(m_operations.operations(), [](const auto& operation) {
            return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
          });
      entries.push_back({.id = 70, .label = "Restore", .enabled = restoring});
      entries.push_back({.id = 71, .label = "Restore to…", .enabled = restoring});
      return entries;
    }
    entries.insert(
        entries.end(),
        {
            {.id = menu_id::kDuplicate, .label = "Duplicate", .enabled = canCreate && hasSelection, .actionGlyph = "copy"},
            {.id = 4, .label = "Rename", .enabled = canRename && hasSelection, .shortcut = "F2", .actionGlyph = "pencil"},
            {.id = 5,
             .label = "Move to Wastebin",
             .enabled = caps.canTrash && canMutate && hasSelection,
             .shortcut = "Delete",
             .actionGlyph = "trash"},
            {.id = 6, .label = "Delete Permanently", .enabled = canMutate && hasSelection, .shortcut = "Shift+Delete"},
            {.separator = true},
        }
    );
    if (const auto selection =
            (canCreate && m_callbacks.symlinkSelection) ? m_callbacks.symlinkSelection() : std::nullopt) {
      entries.push_back(
          {.id = menu_id::kSelectionSymlink,
           .label = selection->second ? "Symlink to Folder" : "Symlink to File",
           .actionGlyph = "link"}
      );
    }
    entries.push_back({.id = menu_id::kManualSymlink, .label = "Symlink…", .enabled = canCreate, .actionGlyph = "link"});
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::viewMenuEntries() const {
    const auto& session = m_tabs.activeTab().session;
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    std::vector<ContextMenuControlEntry> columns;
    for (auto column : filemanager::kFileColumns) {
      const auto index = columnIndex(column);
      columns.push_back(
          {.id = menu_id::kFirstColumnAction + static_cast<int>(index),
           .label = std::string(filemanager::kFileColumnLabels[index]),
           .enabled = column != FileColumn::Name,
           .checkmark = true,
           .toggleState = m_listHeader.columnLayout().visible[index] ? 1 : 0}
      );
    }
    return {
        {.id = menu_id::kListView,
         .label = "List View",
         .radio = true,
         .toggleState = m_listHeader.gridMode() ? 0 : 1,
         .actionGlyph = "layout-list"},
        {.id = menu_id::kGridView,
         .label = "Grid View",
         .radio = true,
         .toggleState = m_listHeader.gridMode() ? 1 : 0,
         .actionGlyph = "layout-grid"},
        {.id = menu_id::kShowColumns,
         .label = "Show Columns",
         .hasSubmenu = true,
         .submenuEntries = std::move(columns),
         .actionGlyph = "columns-3"},
        {.separator = true},
        filemanager::fileSortByMenuEntry(
            m_listHeader.sortField(), m_listHeader.sortOrder(), session.foldersFirst(), session.hiddenLast()
        ),
        {.id = 50, .label = "Show Hidden Files", .checkmark = true, .toggleState = session.showHidden() ? 1 : 0},
        {.separator = true},
        {.id = menu_id::kZoomIn,
         .label = "Zoom In",
         .enabled = !renaming && m_listHeader.viewScale() < filemanager::kFileViewMaxScale,
         .shortcut = "Ctrl++",
         .actionGlyph = "zoom-in"},
        {.id = menu_id::kZoomOut,
         .label = "Zoom Out",
         .enabled = !renaming && m_listHeader.viewScale() > filemanager::kFileViewMinScale,
         .shortcut = "Ctrl+-",
         .actionGlyph = "zoom-out"},
        {.id = menu_id::kResetZoom,
         .label = "Reset Zoom",
         .enabled = !renaming && m_listHeader.viewScale() != filemanager::kFileViewDefaultScale,
         .shortcut = "Ctrl+0",
         .actionGlyph = "zoom-reset"},
        {.separator = true},
        {.id = 13, .label = "Refresh", .shortcut = "Ctrl+R", .actionGlyph = "refresh"},
        {.id = menu_id::kAlwaysShowTabBar,
         .label = "Always Show Tab Bar",
         .checkmark = true,
         .toggleState = (m_callbacks.alwaysShowTabBar && m_callbacks.alwaysShowTabBar()) ? 1 : 0},
        {.id = menu_id::kOpenTabsInBackground,
         .label = "Open New Tabs in Background",
         .checkmark = true,
         .toggleState = (m_callbacks.openTabsInBackground && m_callbacks.openTabsInBackground()) ? 1 : 0},
        {.id = menu_id::kShowMenuBar,
         .label = "Show Menu Bar",
         .checkmark = true,
         .toggleState = m_showApplicationMenuBar ? 1 : 0},
    };
  }

  std::vector<std::string> FileMenuController::externalToolTargets(external_tools::Tool tool) const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    if (session.browsingTrash() || !tab.capabilities().canRunExternalTools || session.loading())
      return {};
    if (tool == external_tools::Tool::Compare) {
      const auto& selectedPaths = m_callbacks.selectedPaths();
      return selectedPaths.size() == 2 && m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered()
                                                                           : std::vector<std::string>{};
    }
    return {session.path().string()};
  }

  std::vector<ContextMenuControlEntry> FileMenuController::toolsMenuEntries() {
    namespace tools = external_tools;
    const auto& tab = m_tabs.activeTab();
    const auto caps = tab.capabilities();
    const auto applications = tools::discover();
    constexpr std::array labels{"Open Terminal Here", "Analyse Disk Usage…", "Compare Files…"};
    constexpr std::array fallbackGlyphs{"terminal-2", "chart-pie", "file-diff"};
    const auto iconsFor = [](GDesktopAppInfo* app) {
      auto names = applicationIconNames(G_APP_INFO(app));
      if (GIcon* icon = g_app_info_get_icon(G_APP_INFO(app)); G_IS_FILE_ICON(icon)) {
        if (char* path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)))) {
          names.emplace_back(path);
          g_free(path);
        }
      }
      return names;
    };

    m_externalTools.clear();
    std::vector<ContextMenuControlEntry> rows;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      const auto tool = static_cast<tools::Tool>(i);
      const auto& apps = applications[i];
      ContextMenuControlEntry entry{
          .id = menu_id::kFirstToolMenuRow + static_cast<int>(i),
          .label = labels[i],
          .enabled = caps.canRunExternalTools && !apps.choices.empty() && tools::validTargets(tool, externalToolTargets(tool)),
          .hasSubmenu = true,
          .attachSubmenuContent = true,
          .splitSubmenu = true,
          .gutterFallbackGlyph = fallbackGlyphs[i],
      };
      if (apps.preferred)
        entry.gutterIconNames = iconsFor(apps.preferred.get());
      if (entry.enabled) {
        for (const auto& choice : apps.choices) {
          const auto id = menu_id::kFirstExternalToolApp + static_cast<int>(m_externalTools.size());
          m_externalTools.emplace_back(tool, choice);
          if (choice.app == apps.preferred) {
            entry.id = id;
            entry.activateMainSection = true;
          }
          entry.submenuEntries.push_back({
              .id = id,
              .label = applicationDisplayName(G_APP_INFO(choice.app.get())),
              .gutterIconNames = iconsFor(choice.app.get()),
              .gutterFallbackGlyph = fallbackGlyphs[i],
          });
        }
      }
      rows.push_back(std::move(entry));
    }
    const auto checksums = m_callbacks.checksumMenuEntries ? m_callbacks.checksumMenuEntries()
                                                           : ChecksumDialogController::menuEntries(false);
    rows.push_back({.separator = true});
    rows.insert(rows.end(), checksums.begin(), checksums.end());
    rows.push_back({.separator = true});
    rows.push_back({
        .id = menu_id::kActAsAdministrator,
        .label = "Act as Administrator",
        .enabled = !tab.session.browsingTrash() && !tab.hasActiveAdminSession() && !tab.adminSession.isAuthenticating(),
        .actionIconNames = {"security-high-symbolic", "dialog-password-symbolic"},
        .actionGlyph = "shield",
    });
    return rows;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::helpMenuEntries() const {
    return HelpDialogController::menuEntries();
  }

  std::optional<std::vector<ContextMenuControlEntry>>
  FileMenuController::applicationMenuEntries(std::size_t menuIndex) {
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
    case menu_id::kToolsMenuIndex:
      entries = toolsMenuEntries();
      break;
    case menu_id::kHelpMenuIndex:
      entries = helpMenuEntries();
      break;
    default:
      return std::nullopt;
    }
    if (m_applicationMenuCollapsed && menuIndex == 0) {
      entries.insert(
          entries.end(),
          {
              {.separator = true},
              {.id = menu_id::kCompactEditMenu,
               .label = "Edit",
               .hasSubmenu = true,
               .submenuEntries = editMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactViewMenu,
               .label = "View",
               .hasSubmenu = true,
               .submenuEntries = viewMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactToolsMenu,
               .label = "Tools",
               .hasSubmenu = true,
               .submenuEntries = toolsMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactHelpMenu,
               .label = "Help",
               .hasSubmenu = true,
               .submenuEntries = helpMenuEntries(),
               .attachSubmenuContent = true},
          }
      );
    }
    return entries;
  }

  bool FileMenuController::openApplicationMenu(float x, float y, std::size_t menuIndex) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return false;
    auto entries = applicationMenuEntries(menuIndex);
    if (!entries)
      return false;
    m_contextMenuTarget.reset();
    const auto output = m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(*entries),
            .minMenuWidth = 240.0F,
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
    if (m_contextMenuPopup->isOpen())
      m_applicationMenuOpen = menuIndex;
    updateApplicationMenuUnderline();
    return m_contextMenuPopup->isOpen();
  }

  void FileMenuController::openContextMenu(std::size_t index, float x, float y) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (index >= session.entries().size() || !m_contextMenuPopup || !surface)
      return;
    m_contextMenuTarget = session.entries()[index].absPath;
    m_openWithApplications.clear();
    const std::string path = session.entries()[index].absPath.string();
    if (!m_callbacks.selectedPaths().contains(path) && m_callbacks.onSelectSingleForContextMenu)
      m_callbacks.onSelectSingleForContextMenu(index);

    const bool targetIsDir = session.entries()[index].isDir;
    std::vector<ContextMenuControlEntry> entries{
        {.id = 10,
         .label = "Open",
         .enabled = !tab.hasActiveAdminSession() || targetIsDir,
         .actionIconNames = {"folder-open-symbolic", "folder-open"},
         .actionGlyph = "folder-open"},
        {.id = 1,
         .label = "Copy",
         .enabled = caps.canCopyItems,
         .shortcut = "Ctrl+C",
         .actionIconNames = {"edit-copy-symbolic", "edit-copy"},
         .actionGlyph = "copy"},
        {.id = 2,
         .label = "Cut",
         .enabled = caps.canMutate && caps.canCopyItems,
         .shortcut = "Ctrl+X",
         .actionIconNames = {"edit-cut-symbolic", "edit-cut"},
         .actionGlyph = "scissors"},
        {.id = 3,
         .label = "Paste",
         .enabled = caps.canMutate || caps.canPasteAsAdministrator,
         .shortcut = "Ctrl+V",
         .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
         .actionGlyph = "U+EA6F"},
        {.id = 4,
         .label = "Rename",
         .enabled = caps.canRename,
         .shortcut = "F2",
         .actionIconNames = {"document-edit-symbolic", "document-edit"},
         .actionGlyph = "pencil"},
        {.id = 5,
         .label = "Move to Trash",
         .enabled = caps.canTrash && caps.canMutate,
         .shortcut = "Delete",
         .actionIconNames = {"user-trash-symbolic", "user-trash"},
         .actionGlyph = "trash"},
        {.id = 6, .label = "Delete permanently", .enabled = caps.canMutate, .shortcut = "Shift+Delete"}
    };
    if (session.browsingTrash())
      entries = {
          {.id = 10,
           .label = "Open",
           .actionIconNames = {"folder-open-symbolic", "folder-open"},
           .actionGlyph = "folder-open"},
          {.id = 70, .label = "Restore"},
          {.id = 71, .label = "Restore to…"},
      };
    else {
      if (!targetIsDir && caps.canLaunchApps) {
        std::vector<ContextMenuControlEntry> alternatives;
        std::string mimeStorage = session.entries()[index].type;
        if (mimeStorage.empty()) {
          if (gchar* guessed =
                  g_content_type_guess(session.entries()[index].absPath.filename().c_str(), nullptr, 0, nullptr)) {
            mimeStorage = guessed;
            g_free(guessed);
          }
        }
        if (!mimeStorage.empty()) {
          const char* mime = mimeStorage.c_str();
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
      } else if (!targetIsDir) {
        entries.insert(
            entries.begin() + 1,
            ContextMenuControlEntry{.id = 80, .label = "Open With", .enabled = false, .hasSubmenu = true}
        );
      }
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kOpenFolderInNewTab,
             .label = "Open in New Tab",
             .actionIconNames = {"tab-new-symbolic", "tab-new"},
             .actionGlyph = "plus"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = 60,
             .label = "Open in New Window",
             .actionIconNames = {"window-new-symbolic", "window-new"},
             .actionGlyph = "app-window"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kOpenFolderAsAdministrator,
             .label = "Open as Administrator",
             .enabled = !tab.hasActiveAdminSession() && !tab.adminSession.isAuthenticating(),
             .actionIconNames = {"security-high-symbolic", "dialog-password-symbolic"},
             .actionGlyph = "shield"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = 20,
             .label = "Add to Places",
             .enabled = caps.canPinToPlaces,
             .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
             .actionGlyph = "bookmark-plus"}
        );
      entries.push_back(
          {.id = 59,
           .label = "Copy Location",
           .enabled = caps.canCopyItems,
           .actionIconNames = {"insert-link-symbolic", "insert-link"},
           .actionGlyph = "link"}
      );
      if (m_callbacks.selectedPaths().size() == 1)
        entries.push_back(
            {.id = 22,
             .label = "Properties",
             .enabled = caps.canOpenProperties,
             .shortcut = "Alt+Enter",
             .actionIconNames = {"document-properties-symbolic", "document-properties"},
             .actionGlyph = "file-info"}
        );
    }
    const bool canCreate = caps.canCreate && !session.browsingTrash() && session.currentDirectoryWritable();
    const bool canCreateAny =
        (caps.canCreate || caps.canCreateBasic) && !session.browsingTrash() && session.currentDirectoryWritable();
    if (canCreateAny)
      entries.push_back(newItemMenu());
    filemanager::appendFileMenuEntries(
        entries,
        session.browsingTrash()              ? filemanager::FileMenuContext::Trash
            : session.entries()[index].isDir ? filemanager::FileMenuContext::Folder
                                             : filemanager::FileMenuContext::File,
        m_listHeader.gridMode(), session.showHidden(), m_listHeader.sortField(), m_listHeader.sortOrder(),
        session.foldersFirst(), session.hiddenLast()
    );
    filemanager::groupFileMenuEntries(
        entries,
        session.browsingTrash()              ? filemanager::FileMenuContext::Trash
            : session.entries()[index].isDir ? filemanager::FileMenuContext::Folder
                                             : filemanager::FileMenuContext::File
    );
    if (!session.browsingTrash()) {
      entries.insert(
          std::ranges::find(entries, 4, &ContextMenuControlEntry::id),
          {.id = menu_id::kDuplicate,
           .label = "Duplicate",
           .enabled =
               canCreate && !session.loading() && !m_places.hasActiveDeviceOperations() && !m_operations.replaying(),
           .actionGlyph = "copy"}
      );
    }
    if (!session.browsingTrash() && !session.entries()[index].isDir) {
      const auto checksums = m_callbacks.checksumMenuEntries ? m_callbacks.checksumMenuEntries()
                                                             : ChecksumDialogController::menuEntries(false);
      entries.push_back({.separator = true});
      entries.insert(entries.end(), checksums.begin(), checksums.end());
    }
    auto* output = m_wayland.lastPointerOutput();
    if (!output)
      output = m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = entries,
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0,
            .fitAvailableHeight = !session.browsingTrash(),
        }
    );
  }

  void FileMenuController::openEmptyContextMenu(float x, float y) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return;
    m_contextMenuTarget.reset();
    if (m_callbacks.onClearSelectionForEmptyMenu)
      m_callbacks.onClearSelectionForEmptyMenu();
    std::vector<ContextMenuControlEntry> entries{
        {.id = 3,
         .label = "Paste here",
         .enabled = caps.canMutate || caps.canPasteAsAdministrator,
         .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
         .actionGlyph = "U+EA6F"}
    };
    if (session.browsingTrash())
      entries = {{.id = 72, .label = "Empty Wastebin…"}};
    else {
      if ((caps.canCreate || caps.canCreateBasic) && session.currentDirectoryWritable())
        entries.push_back(newItemMenu());
      entries.push_back(
          {.id = 21,
           .label = "Add this folder to Places",
           .enabled = caps.canPinToPlaces,
           .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
           .actionGlyph = "bookmark-plus"}
      );
    }
    filemanager::appendFileMenuEntries(
        entries, session.browsingTrash() ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty,
        m_listHeader.gridMode(), session.showHidden(), m_listHeader.sortField(), m_listHeader.sortOrder(),
        session.foldersFirst(), session.hiddenLast()
    );
    filemanager::groupFileMenuEntries(
        entries, session.browsingTrash() ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty
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
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
  }

  void FileMenuController::showPlacesMenu(std::vector<ContextMenuControlEntry> entries, float x, float y) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface || entries.empty())
      return;
    const float menuHeight = static_cast<float>(entries.size()) * 40.0F + 20.0F;
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(entries),
            .maxMenuWidth = static_cast<float>(surface->width()),
            .anchor =
                PopupAnchorRect{
                    static_cast<std::int32_t>(
                        std::clamp(x, 0.0F, std::max(0.0F, static_cast<float>(surface->width()) - 220.0F))
                    ),
                    static_cast<std::int32_t>(
                        std::clamp(y, 0.0F, std::max(0.0F, static_cast<float>(surface->height()) - menuHeight))
                    ),
                    1, 1
                },
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(),
                    .output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output,
                    .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
  }

  void FileMenuController::openColumnHeaderMenu(std::vector<ContextMenuControlEntry> entries, float x, float y) {
    openTabContextMenu(std::move(entries), x, y);
  }

  void FileMenuController::openTabContextMenu(std::vector<ContextMenuControlEntry> entries, float x, float y) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return;
    auto* output = m_wayland.lastPointerOutput();
    if (!output)
      output = m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(entries),
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
  }

  void FileMenuController::openApplicationChooser() {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    if (!m_applicationPicker || !m_contextMenuTarget || session.browsingTrash() || !tab.capabilities().canLaunchApps)
      return;
    const auto targetEntry = std::ranges::find(session.entries(), *m_contextMenuTarget, &FileEntry::absPath);
    m_applicationChooserMime = targetEntry != session.entries().end() ? targetEntry->type : std::string{};
    if (m_applicationChooserMime.empty()) {
      if (gchar* guessed = g_content_type_guess(m_contextMenuTarget->filename().c_str(), nullptr, 0, nullptr)) {
        m_applicationChooserMime = guessed;
        g_free(guessed);
      }
    }
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
    m_applicationPicker->setOnSelectWithOption([this, tabId = tab.id, target = *m_contextMenuTarget](
                                                  const std::string& value, bool setAsDefault
                                              ) {
      const auto* origin = m_tabs.findTab(tabId);
      if (!origin || !origin->capabilities().canLaunchApps)
        return;
      std::size_t index = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
      if (error != std::errc{}
          || end != value.data() + value.size()
          || index >= m_applicationChooserApps.size()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("The selected application is no longer available");
        return;
      }
      GAppInfo* app = m_applicationChooserApps[index].get();
      if (setAsDefault) {
        GError* defaultError = nullptr;
        if (!filemanager::savePropertyDefaultApplication(app, m_applicationChooserMime, &defaultError)) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(defaultError ? defaultError->message : "Could not save the default application");
          g_clear_error(&defaultError);
        }
      }
      GError* launchError = nullptr;
      if (!launchApplicationForFile(app, target, &launchError)) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(
              launchError ? launchError->message : "Could not open this file with the selected application"
          );
      } else if (!setAsDefault) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("");
      }
      g_clear_error(&launchError);
    });
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
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
        .nativeParent = surface ? surface->xdgToplevel() : nullptr,
        .nativeParentSurface = surface ? surface->wlSurface() : nullptr
    };
    if (!m_applicationChooserMime.empty())
      request.optionToggleLabel = "Set as default for this file type";
    m_applicationPicker->open(std::move(request));
  }

  bool FileMenuController::launchOpenWithApp(std::size_t index) {
    if (!m_tabs.activeTab().capabilities().canLaunchApps
        || index >= m_openWithApplications.size()
        || !m_contextMenuTarget)
      return false;
    GError* error = nullptr;
    if (!launchApplicationForFile(m_openWithApplications[index].get(), *m_contextMenuTarget, &error)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(error ? error->message : "Could not open this file with the selected application");
    } else if (m_callbacks.onStatus) {
      m_callbacks.onStatus("");
    }
    g_clear_error(&error);
    return true;
  }

  bool FileMenuController::launchExternalTool(std::size_t index) {
    if (!m_tabs.activeTab().capabilities().canRunExternalTools || index >= m_externalTools.size())
      return false;
    const auto [tool, choice] = m_externalTools[index];
    const auto targets = externalToolTargets(tool);
    GError* error = nullptr;
    if (external_tools::launch(choice, tool, targets, &error)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("");
    } else if (m_callbacks.onStatus) {
      m_callbacks.onStatus(error ? error->message : "Could not launch the selected tool");
    }
    g_clear_error(&error);
    return true;
  }

  bool FileMenuController::launchApplicationForFile(GAppInfo* app, const std::filesystem::path& path, GError** error) {
    if (!app || path.empty()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No application or file was selected.");
      return false;
    }
    GFile* file = g_file_new_for_path(path.c_str());
    GList files{file, nullptr, nullptr};
    char* uri = g_file_get_uri(file);
    GList uris{uri, nullptr, nullptr};
    const gboolean launched = g_app_info_supports_files(app) ? g_app_info_launch(app, &files, nullptr, error)
                                                             : g_app_info_launch_uris(app, &uris, nullptr, error);
    g_free(uri);
    g_object_unref(file);
    return launched == TRUE;
  }

} // namespace filemanager
