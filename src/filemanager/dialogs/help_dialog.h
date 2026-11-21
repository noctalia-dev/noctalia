#pragma once

#include "filemanager/dialogs/modal_dialog_host.h"
#include "ui/controls/context_menu.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Button;
class FileManagerTestAccess;
class Flex;
class Image;
class Renderer;
class ScrollView;
class ToplevelSurface;
class WaylandConnection;
struct KeyboardEvent;

namespace filemanager {

  class HelpDialogController {
  public:
    static constexpr int kKeyboardShortcutsMenuId = 30201;
    static constexpr int kReportBugMenuId = 30202;
    static constexpr int kGitHubMenuId = 30203;
    static constexpr int kAboutMenuId = 30204;

    static constexpr std::string_view kReportBugUrl = "https://github.com/Nythera-Dev/Nythera/issues";
    static constexpr std::string_view kRepositoryUrl = "https://github.com/Nythera-Dev/Nythera";
    static constexpr std::string_view kAppIconAssetPath = "concepts/nythera-files-icon.png";
    static constexpr std::string_view kLicenceAssetPath = "LICENSE";

    struct ShortcutEntry {
      std::string_view action;
      std::string_view shortcut;
    };

    struct ShortcutGroup {
      std::string_view title;
      std::span<const ShortcutEntry> entries;
    };

    struct AboutInfo {
      std::string appName;
      std::string version;
      std::string description;
      std::string licence;
      std::string copyright;
      std::string credits;
      std::string creditsCopyright;
      std::string repositoryUrl;
      std::filesystem::path iconPath;
      std::filesystem::path licencePath;
    };

    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<void(std::string)> onStatus;
      std::function<bool(const std::string& url)> openUrl;
    };

    HelpDialogController(ModalDialogHost& dialogHost, WaylandConnection& wayland, Callbacks callbacks);
    ~HelpDialogController();

    [[nodiscard]] static std::vector<ContextMenuControlEntry> menuEntries();
    [[nodiscard]] static std::span<const ShortcutGroup> shortcutGroups() noexcept;
    [[nodiscard]] static AboutInfo aboutInfo();

    bool handleMenuAction(int menuId);
    void showShortcuts();
    void showAbout();
    bool openReportBug();
    bool openRepository();
    void dismiss();
    [[nodiscard]] bool isOpen() const noexcept { return m_open && m_dialogHost.isOpen(); }

  private:
    friend class ::FileManagerTestAccess;

    [[nodiscard]] ModalDialogHost::CustomContent makeShortcutsCustomContent();
    [[nodiscard]] ModalDialogHost::CustomContent makeAboutCustomContent();
    void buildShortcutsFields(Flex& root, float width);
    [[nodiscard]] bool handleShortcutsKey(const KeyboardEvent& event);
    void buildAboutFields(Flex& root, float width);
    void prepareAboutLayout(Renderer& renderer);
    bool openExternalUrl(std::string_view url);
    void clearState() noexcept;

    ModalDialogHost& m_dialogHost;
    WaylandConnection& m_wayland;
    Callbacks m_callbacks;
    bool m_open = false;
    ScrollView* m_shortcutsScroll = nullptr;
    Button* m_shortcutsClose = nullptr;
    Image* m_aboutIcon = nullptr;
    Button* m_repositoryButton = nullptr;
    Button* m_aboutClose = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
