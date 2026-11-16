#pragma once

#include "config/config_service.h"
#include "filemanager/folder_size.h"
#include "filemanager/property_operations.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Flex;
class Button;
class Label;
class Node;
class FileIcon;
class Checkbox;
class Spinner;
class Separator;
class Input;
class Select;
class SelectDropdownPopup;
class FileOperationManager;
class WaylandConnection;
class PropertiesSurface;
namespace filemanager {
  struct Place;
}
class InputArea;

class PropertiesWindow {
public:
  PropertiesWindow(
      WaylandConnection& wayland, RenderContext& renderer, ConfigService& config, FileOperationManager& operations,
      std::function<void(std::string)> copyText
  );
  ~PropertiesWindow();
  bool open(const std::filesystem::path& path, ToplevelSurface* parent);
  bool openVolume(const filemanager::Place& place, ToplevelSurface* parent);
  void close();
  void prepareFrame(bool update, bool layout);
  void handleKeyboard(const KeyboardEvent& event);
  void handlePointer(const PointerEvent& event);
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;

private:
  struct Section {
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
  };
  bool openTarget(const std::filesystem::path& path, ToplevelSurface* parent, const filemanager::Place* volume);
  bool m_volumeOnly = false;
  void rebuildTab();
  void ensureFolderSize();
  void cancelFolderSize();
  void updateFolderSizeRows();
  void refreshMetadata();
  void loadChoices();
  void confirmRename(bool applyOtherChanges = false);
  void applyPendingChanges();
  void chooseApplication(Button* anchor);
  void applyDefaultApplication();
  void finishChanges(bool retainPending = false);
  void deferClose(bool discard = false);
  void queryReady(GFileInfo* info, const std::string& error);
  void queryFilesystemMetadata();
  void queryMountCapacity();
  void filesystemReady(GFileInfo* info);
  void syncFilesystemSections();
  void updatePermissionControls();
  void applyPermissions();
  [[nodiscard]] bool permissionsPending() const;
  void updateFooter();
  WaylandConnection& m_wayland;
  RenderContext& m_renderer;
  ConfigService& m_config;
  FileOperationManager& m_operations;
  std::function<void(std::string)> m_copyText;
  Input* m_nameInput = nullptr;
  std::array<Select*, 2> m_ownershipSelects{};
  std::string m_nameDraft;
  bool m_renaming = false;
  bool m_continueAfterRename = false;
  bool m_tooltipOwned = false;
  std::string m_applySummary;
  bool m_itemAvailable = false;
  bool m_mountTarget = false;
  std::array<InputArea*, 8> m_resizeEdges{};
  std::unique_ptr<SelectDropdownPopup> m_dropdown;
  std::unique_ptr<ContextMenuPopup> m_copyMenu;
  std::unordered_map<InputArea*, Input*> m_readOnlyInputs;
  std::uint64_t m_copyTargetRevision = 0;
  filemanager::PropertyAccounts m_accounts;
  std::vector<std::shared_ptr<GAppInfo>> m_applications;
  std::shared_ptr<GAppInfo> m_defaultApplication;
  std::shared_ptr<GAppInfo> m_pendingApplication;
  std::string m_mime;
  std::string m_pendingOwner;
  std::string m_pendingGroup;
  std::string m_renameError;
  std::string m_applicationError;
  std::string m_permissionError;
  AnimationManager m_animations;
  GCancellable* m_folderCancel = nullptr;
  filemanager::FolderSizeRequest m_folderRequest;
  filemanager::FolderSizeResult m_folderResult;
  std::uint64_t m_folderRevision = 0;
  Spinner* m_sizeSpinner = nullptr;
  Button* m_sizeRefreshButton = nullptr;
  Button* m_sizeStopButton = nullptr;
  bool m_folderScanning = false;
  bool m_folderComplete = false;
  bool m_folderDetailsExpanded = false;
  std::uint32_t m_uid = 0;
  std::uint32_t m_gid = 0;
  std::uint64_t m_operation = 0;
  bool m_choicesLoading = false;
  bool m_refreshing = false;
  bool m_recursive = false;
  bool m_retainPending = false;
  std::set<std::string> m_expandedMetadata;
  Checkbox* m_recursiveCheck = nullptr;
  std::unique_ptr<PropertiesSurface> m_surface;
  std::unique_ptr<Flex> m_root;
  Flex* m_body = nullptr;
  FileIcon* m_icon = nullptr;
  Button* m_closeButton = nullptr;
  Button* m_applyButton = nullptr;
  Separator* m_footerSeparator = nullptr;
  Label* m_footerStatus = nullptr;
  bool m_discardRequested = false;
  std::array<std::array<Checkbox*, 3>, 3> m_permissionChecks{};
  std::vector<std::string> m_iconNames;
  std::string m_displayName;
  std::string m_permissionMessage;
  std::uint32_t m_originalMode = 0;
  std::uint32_t m_editedMode = 0;
  std::uint64_t m_inode = 0;
  std::uint32_t m_device = 0;
  bool m_permissionEditable = false;
  bool m_modeAvailable = false;
  bool m_applying = false;
  float m_capacity = -1.0F;
  std::array<Button*, 3> m_tabButtons{};
  InputDispatcher m_input;
  std::filesystem::path m_path;
  std::vector<Section> m_general;
  std::vector<Section> m_permissions;
  std::vector<Section> m_details;
  Section m_filesystem;
  std::string m_filesystemId;
  int m_tab = 0;
  int m_renderedTab = -1;
  bool m_contentDirty = true;
  bool m_loading = true;
  bool m_directory = false;
  std::uint32_t m_preferredWidth = 500;
  std::uint32_t m_preferredHeight = 620;
  std::uint64_t m_generation = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
