#pragma once

#include "filemanager/file_operation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Button;
class ConfigService;
class DialogBackgroundArtwork;
class FileManagerTestAccess;
class Flex;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class RenderContext;
class Renderer;
class WaylandConnection;

namespace filemanager {

  class FileManagerSettings;

  enum class ConflictDialogKind : std::uint8_t {
    File,
    Directory,
    RestoreDirectory,
    TypeMismatch,
    SelfConflict,
  };

  struct ConflictItemInfo {
    std::string name;
    std::string metadata;
    std::filesystem::path path;
    bool isDirectory = false;
  };

  struct ConflictDialogData {
    ConflictDialogKind kind = ConflictDialogKind::File;
    ConflictItemInfo incoming;
    ConflictItemInfo existing;
    std::string comparisonSummary;
  };

  class ModalDialogHost {
  public:
    struct CustomContent {
      std::function<void(Flex& root, float dialogWidth)> buildFields;
      std::function<void(int result, Button* button)> bindChoiceButton;
      std::function<void()> afterBuild;
      std::function<InputArea*()> initialFocus;
      std::function<void()> onThemeUpdated;
      std::function<void(Renderer&)> onPrepareLayout;
      std::function<bool(const KeyboardEvent&)> onKeyEvent;
      std::function<bool(int response)> interceptResponse;
      std::function<void()> onBeforeClose;
      std::function<void()> onDismiss;
      bool useMeasuredMinHeight = false;
      bool fillContent = false;
      std::uint32_t preferredWidth = 0;
      std::uint32_t preferredHeight = 0;
      std::uint32_t minWidth = 0;
      std::uint32_t minHeight = 0;
    };

    struct Callbacks {
      std::function<ToplevelSurface*()> defaultParent;
      std::function<bool()> isMainWindowOpen;
      std::function<ToplevelSurface*()> mainSurface;
      std::function<ToplevelSurface*()> progressSurface;
      std::function<void()> syncProgressWindow;
      std::function<void(std::string)> setStatus;
    };

    ModalDialogHost(
        WaylandConnection& wayland, RenderContext& renderContext, ConfigService& configService,
        FileManagerSettings& settings, Callbacks callbacks
    );
    ~ModalDialogHost();

    void showDialog(
        std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
        std::function<void(int)> response, ToplevelSurface* parent = nullptr, bool applyToAllCheckbox = false,
        bool alternativeName = false, std::optional<CustomContent> customContent = std::nullopt
    );
    void textPrompt(
        std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
    );
    enum class SymlinkField : std::uint8_t { None, Name, Target };
    struct SymlinkError {
      std::string message;
      SymlinkField field = SymlinkField::None;
    };
    void showSymlinkDialog(
        std::function<std::optional<SymlinkError>(const std::string& name, const std::string& target)> onSubmit
    );
    void closeDialog(int response);
    void forceCloseImmediate();
    void updateTheme();
    void handlePointer(const PointerEvent& event);
    void handleKeyboard(const KeyboardEvent& event);
    void requestLayout();
    void setFocus(InputArea* area);
    void registerCard(Flex* card);

    [[nodiscard]] FileOperationManager::ConflictResolver makeConflictResolver();
    [[nodiscard]] static std::string sanitizeConflictDisplayText(std::string_view text);

    [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
    [[nodiscard]] bool closeQueued() const noexcept { return m_closeQueued; }
    [[nodiscard]] bool backgroundConflictPending() const noexcept { return m_backgroundConflictPending; }
    [[nodiscard]] ToplevelSurface* surface() const noexcept { return m_surface.get(); }
    [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }
    [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept {
      return m_surface && surface == m_surface->wlSurface();
    }

  private:
    friend class ::FileManagerTestAccess;

    void buildDialog(float dialogWidth);
    void prepareFrame(bool needsUpdate, bool needsLayout);
    [[nodiscard]] bool submitSymlinkDialog();

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    ConfigService& m_configService;
    FileManagerSettings& m_settings;
    Callbacks m_callbacks;

    std::unique_ptr<ToplevelSurface> m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Flex> m_root;
    std::vector<Flex*> m_cards;
    std::function<void(int)> m_response;
    std::mutex m_conflictMutex;
    std::string m_title;
    std::string m_message;
    bool m_warningStyle = false;
    bool m_resizable = false;
    std::string m_sizeStateKey;
    std::uint32_t m_preferredWidth = 0;
    std::uint32_t m_preferredHeight = 0;
    std::array<InputArea*, 8> m_resizeEdges{};
    InputArea* m_moveHandle = nullptr;
    Flex* m_titleRow = nullptr;
    Flex* m_spacer = nullptr;
    Flex* m_footer = nullptr;
    Node* m_divider = nullptr;
    Image* m_deleteWarning = nullptr;
    DialogBackgroundArtwork* m_topAccent = nullptr;
    DialogBackgroundArtwork* m_cornerAccent = nullptr;
    std::optional<ConflictDialogData> m_conflict;
    std::function<bool(const ConflictDialogData&, bool hasRemaining, std::function<void(int, std::string)>)>
        m_conflictDialogHook;
    std::vector<std::pair<std::string, int>> m_choices;
    bool m_closeQueued = false;
    bool m_applyToAllCheckbox = false;
    bool m_applyToAllChecked = false;
    bool m_alternativeNameEnabled = false;
    std::string m_alternativeName;
    std::filesystem::path m_destination;
    Input* m_alternativeNameInput = nullptr;
    bool m_textPrompt = false;
    bool m_passwordPrompt = false;
    bool m_backgroundConflictPending = false;
    bool m_symlinkDialogActive = false;
    std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> m_symlinkSubmitCallback;
    Input* m_symlinkName = nullptr;
    Input* m_symlinkTarget = nullptr;
    Label* m_symlinkError = nullptr;
    std::optional<CustomContent> m_customContent;
    int m_resultPending = -1;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
