#pragma once

#include "filemanager/browser_tab.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/places_sidebar.h"
#include "filemanager/tab_manager.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/native_data_transfer.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Button;
class FileIcon;
class FileManagerTestAccess;
class Flex;
class Label;
class Renderer;
class VirtualGridView;

namespace filemanager {

  class FileTransferController {
  public:
    using PasteRequest = filemanager::PasteRequest;
    using PasteSelection = filemanager::PasteSelection;

    struct DropValidation {
      std::vector<std::string> sources;
      std::filesystem::path rawDestination;
      std::optional<std::filesystem::path> target;
      bool copyByDefault = true;
    };

    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<Flex*()> sceneRoot;
      std::function<VirtualGridView*()> grid;
      std::function<bool()> isWindowOpen;
      std::function<bool()> isQuitting;
      std::function<bool()> isRenaming;
      std::function<std::uint32_t()> keyboardModifiers;
      std::function<std::uint32_t()> pointerSerial;
      std::function<std::vector<std::string>()> selectedPathsOrdered;
      std::function<std::optional<std::filesystem::path>()> selectedPath;
      std::function<void(TabId tabId, std::unordered_set<std::string> paths, std::filesystem::path primary)>
          onApplyPasteSelection;
      std::function<void(std::size_t index, std::uint32_t clickTime)> onClickUnmovedDragItem;
      std::function<void()> onCancelPlaceReorder;
      std::function<void()> onClearLastPointerIndex;
      std::function<void(const PointerEvent&)> onSynthesizeMotion;
      std::function<void()> onUpdateNavigationControls;
      std::function<void()> onUpdateSelectionSummary;
      std::function<void()> onCutStateChanged;
      std::function<void(std::string)> onStatus;
      std::function<void()> onMaybeStopMainLoop;
    };

    FileTransferController(
        WaylandConnection& wayland, RenderContext& renderContext, InputDispatcher& dispatcher,
        NativeDataTransfer& transfer, FileOperationManager& operations, TabManager& tabs,
        PlacesSidebarController& places, Callbacks callbacks
    );
    ~FileTransferController();

    void setupDragHandlers();
    void shutdown();
    void detachUi();

    bool onOperationFinished(const FileOperationManager::Operation& operation);
    void onBeforePathChange(BrowserTab& tab);
    void onEnumerateStarted(BrowserTab& tab);
    void registerPasteRequest(std::uint64_t opId, PasteRequest request);

    void pasteClipboard();
    void copyToDesktopClipboard(bool cut);
    void writeDesktopClipboard(std::string payload, const char* mime);
    void setCutPaths(std::vector<std::string> paths);
    bool cancelCutSelection();
    void syncCutStateFromClipboard();
    [[nodiscard]] bool isPathCut(const std::filesystem::path& path) const;
    [[nodiscard]] bool hasActiveCutItems() const noexcept { return !m_activeCutPaths.empty(); }

    bool recordPasteCompletion(const FileOperationManager::Operation& operation);
    void applyPendingPasteSelection(BrowserTab& tab);
    void applyPendingPasteSelection() { applyPendingPasteSelection(m_tabs.activeTab()); }
    void finishPendingTransferReveal(Renderer& renderer);

    [[nodiscard]] std::optional<std::filesystem::path> dropDestination(float x, float y, bool incoming = false) const;
    [[nodiscard]] bool dragSourcesAreFolders() const;
    void cancelDropValidation();
    void validateDropAsync(
        TabId tabId, std::vector<std::string> sources, std::filesystem::path destination, std::uint64_t navigation,
        std::uint64_t dragSession, std::function<void(DropValidation)> onComplete
    );
    void requestHoverDropValidation(const std::vector<std::string>& sources, const std::filesystem::path& destination);

    std::uint32_t updateProtocolDrag(wl_surface* surface, float x, float y, std::uint32_t actions);
    NativeDataTransfer::DropReceiver receiveProtocolDrop(std::uint32_t action);

    void cancelFileGesture();
    void clearNativeDragPreview();
    void scheduleNativeDragPreview();
    void updateDragPreview(Renderer& renderer, bool nativeIcon = false);
    void clearDragFeedback();

    void beginFileDragPress(std::size_t index, float x, float y, std::uint32_t serial);
    bool handleFileDragPointer(const PointerEvent& event, float x, float y);
    void handlePointerLeaveDuringDrag();
    void handleAxisDuringProtocolDrag(const PointerEvent& event);
    void cancelLocalDragState();

    [[nodiscard]] bool isDraggingFiles() const noexcept { return m_draggingFiles; }
    [[nodiscard]] bool isDragPending() const noexcept { return m_dragPending; }
    [[nodiscard]] bool hasDragPreview() const noexcept { return m_dragPreview != nullptr; }
    [[nodiscard]] std::size_t pendingClipboardReads() const noexcept { return m_pendingClipboardReads; }
    [[nodiscard]] const std::optional<std::size_t>& dropIndex() const noexcept { return m_dropIndex; }
    [[nodiscard]] const std::optional<std::filesystem::path>& pendingPasteRevealPath() const noexcept {
      return m_tabs.activeTab().pendingPasteRevealPath;
    }
    [[nodiscard]] const std::optional<std::uint64_t>& pendingDropScrollGeneration() const noexcept {
      return m_tabs.activeTab().pendingDropScrollGeneration;
    }
    void clearDropPlace() noexcept { m_dropPlace = nullptr; }

  private:
    friend class ::FileManagerTestAccess;

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    InputDispatcher& m_dispatcher;
    NativeDataTransfer& m_transfer;
    FileOperationManager& m_operations;
    TabManager& m_tabs;
    PlacesSidebarController& m_places;
    Callbacks m_callbacks;
    std::function<void(NativeDataTransfer::ReadCallback)> m_readPasteSelectionForTesting;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    std::unordered_map<std::uint64_t, PasteRequest> m_pasteRequests;
    std::unordered_map<std::uint64_t, std::uint64_t> m_cutOperations;
    std::unordered_map<std::uint64_t, std::vector<std::string>> m_cutOperationSources;
    std::optional<std::uint64_t> m_consumedCutSelection;
    std::unordered_set<std::string> m_activeCutPaths;
    std::vector<std::string> m_activeCutOrdered;
    std::uint64_t m_externalCutQueryGeneration = 0;
    std::unordered_map<std::uint64_t, NativeDataTransfer::FinishDrop> m_dropFinishes;
    std::size_t m_pendingClipboardReads = 0;

    std::optional<std::string> m_pinBefore;
    float m_dragStartX = 0;
    float m_dragStartY = 0;
    bool m_draggingFiles = false;
    bool m_dragPending = false;
    std::uint32_t m_dragPressSerial = 0;
    bool m_protocolDragInside = false;
    std::optional<std::filesystem::path> m_protocolDropDestination;
    std::vector<std::string> m_protocolDropSources;
    std::optional<std::size_t> m_dragIndex;
    std::vector<std::string> m_dragSources;
    std::unique_ptr<RenderTarget> m_nativePreviewTarget;
    wl_surface* m_nativePreviewSurface = nullptr;
    bool m_nativePreviewQueued = false;
    std::uint64_t m_nativePreviewGeneration = 0;
    Flex* m_dragPreview = nullptr;
    FileIcon* m_dragPreviewIcon = nullptr;
    Label* m_dragPreviewLabel = nullptr;
    Label* m_dragPreviewAction = nullptr;
    float m_dragPointerX = 0;
    float m_dragPointerY = 0;
    bool m_dragTargetValid = false;
    Button* m_dropPlace = nullptr;
    std::optional<std::size_t> m_dropIndex;

    std::optional<DropValidation> m_dropValidation;
    std::optional<std::pair<std::vector<std::string>, std::filesystem::path>> m_dropValidationPending;
    GCancellable* m_dropValidationCancellable = nullptr;
    std::uint64_t m_dropValidationGeneration = 0;
    std::uint64_t m_dragSessionGeneration = 0;
    std::function<
        bool(const std::vector<std::string>&, const std::filesystem::path&, std::function<void(DropValidation)>)>
        m_dropValidationHook;
  };

} // namespace filemanager
