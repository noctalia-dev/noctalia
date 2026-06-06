#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/dialogs/layer_popup_host.h"
#include "ui/popup_chrome.h"
#include "wayland/popup_surface.h"

#include <cstdint>
#include <memory>
#include <optional>

class Box;
class ConfigService;
class InputArea;
class Node;
class RectNode;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
struct wl_output;
struct xdg_surface;

// Shared base for the three xdg_popup-backed dialog popups (GlyphPicker,
// ColorPicker, FileDialog). Owns the PopupSurface, the scene scaffolding
// (sceneRoot + bg + content node), the InputDispatcher, the animation
// manager, and all the boilerplate around prepareFrame / pointer event
// dispatch / pointer mapping / popup parent attach. Subclasses provide only
// the sheet construction + sheet-specific layout call + the facade-specific
// cancel/accept routing through a small set of virtual hooks.
//
// Base does NOT inherit any presenter interface; subclasses mix those in.
// Base does NOT know the sheet or result type — each subclass keeps its own
// typed pointer privately and routes results through `accept(T)` calls that
// invoke `closeAfterAccept()` followed by their facade's `complete(T)`.
//
// Lifetime: subclass destructors MUST call `destroyPopup()` first thing.
// The base destructor only asserts the popup is already torn down — it does
// not invoke `onSheetClose()` itself, because at base-destruction time the
// subclass vtable has already been replaced and the typed sheet pointer is
// already gone.
class DialogPopupHost {
public:
  virtual ~DialogPopupHost();

  DialogPopupHost(const DialogPopupHost&) = delete;
  DialogPopupHost& operator=(const DialogPopupHost&) = delete;

  // Pointer/keyboard event dispatch for the popup. Returns true (pointer)
  // when the event was consumed. Subclass presenter overrides should
  // forward verbatim to these.
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  // Surface invalidation helpers. Forward to the underlying PopupSurface.
  void requestLayout();
  void requestRedraw();
  void requestUpdateOnly();

  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] bool isInitializing() const noexcept { return m_openInProgress; }
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] xdg_surface* xdgSurface() const noexcept;
  [[nodiscard]] std::uint32_t width() const noexcept;
  [[nodiscard]] std::uint32_t height() const noexcept;

protected:
  DialogPopupHost();

  // Called once during subclass `initialize` to capture the shared deps.
  void initializeBase(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext);
  void initializeBase(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts
  );

  // Build the PopupSurface, attach to the host, run xdg_popup initialize.
  // Subclass `openXxx()` calls this after constructing any pre-buildScene
  // state it needs (e.g. FileDialog constructs `m_dialog` first so it can
  // query `preferredWidth/Height`). Returns true on success; on failure,
  // automatically calls `destroyPopup()`.
  [[nodiscard]] bool openPopup(std::uint32_t width, std::uint32_t height);

  // Build the PopupSurface as a child of an xdg parent. Uses the same scene/
  // input/prepareFrame plumbing as openPopup() but bypasses LayerPopupHostRegistry
  // parent resolution.
  [[nodiscard]] bool openPopupAsChild(
      PopupSurfaceConfig config, xdg_surface* parentXdgSurface, wl_surface* parentWlSurface, wl_output* output
  );

  // Tear the popup down — endAttachedPopup, invoke `onSheetClose()` hook,
  // reset the scene tree, drop the PopupSurface. Safe to call repeatedly.
  void destroyPopup();

  // Subclass `accept(T)` calls this before invoking its facade's
  // `complete(T)`. Wraps `destroyPopup()` so the sheet teardown happens
  // before the facade is notified.
  void closeAfterAccept();

  // Tear the popup down and notify the facade via `cancelToFacade()`.
  // Subclasses route their sheet's Cancel button through this. Also fires
  // automatically on Escape and on compositor-initiated dismissal.
  void cancel();

  // Effective UI scale (clamped to a safe minimum). Reads
  // ConfigService::config().shell.uiScale.
  [[nodiscard]] float uiScale() const;

  [[nodiscard]] InputDispatcher& inputDispatcher() noexcept { return m_inputDispatcher; }
  [[nodiscard]] AnimationManager& animations() noexcept { return m_animations; }
  [[nodiscard]] WaylandConnection* wayland() const noexcept { return m_wayland; }
  [[nodiscard]] ConfigService* config() const noexcept { return m_config; }
  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }

  // Construct the standard PopupSurfaceConfig with the shared constraint
  // flags, anchor/gravity NONE, grab=true, and the parent context's
  // centering offset. Subclasses use this when wiring the surface in
  // `openPopup`-equivalent code if they need a custom config.
  [[nodiscard]] PopupSurfaceConfig
  defaultPopupConfig(const LayerPopupParentContext& parent, std::uint32_t width, std::uint32_t height) const;

  /// Snap `m_sceneRoot` / `m_contentNode` to `m_surface` dimensions. `layoutScene` calls this before and after
  /// `layoutSheet`; subclasses that resize inside `layoutSheet` must call it once after `resize()` before
  /// laying out sheet nodes so measurements use the updated inner size.
  void syncSceneGeometryFromSurface();

  // ── Hooks ────────────────────────────────────────────────────────
  //
  // Pure virtual:
  //   populateContent  : add the sheet to `contentParent`. Wire the sheet's
  //                      onCancel/onApply callbacks here. For the FileDialog
  //                      pattern (sheet constructed earlier in openXxx),
  //                      this is where create()/onOpen()/releaseRoot()
  //                      happen.
  //   layoutSheet      : run the sheet's layout pass at the given content
  //                      width/height (already after padding). Glyph/Color:
  //                      `m_sheet->setSize(w,h); m_sheet->layout(*ctx);`.
  //                      File: `m_dialog->layout(*ctx, w, h);`.
  //   cancelToFacade   : invoke `XxxDialog::cancelIfPending()` (or the
  //                      facade-specific equivalent).
  //   initialFocusArea : InputArea to focus after buildScene; nullptr if
  //                      the sheet does not request initial focus.
  //
  // Optional (defaulted):
  //   computePadding   : padding around the content node, in logical px.
  //                      Default is `uiScale * 12.0f`. FileDialog overrides
  //                      to derive from `m_dialog->hasDecoration()` and
  //                      `m_dialog->contentScale()`.
  //   runUpdatePhase   : hook for the prepareFrame Update phase. Default
  //                      no-op. FileDialog overrides to run
  //                      `m_dialog->update(*ctx)` inside a UiPhaseScope
  //                      (the base wraps the call in the phase scope).
  //   preDispatchKeyboard : called before InputDispatcher dispatch.
  //                      Returns true to short-circuit. FileDialog
  //                      overrides to forward to
  //                      `m_dialog->handleGlobalKey(...)`.
  //   onSheetClose     : called inside `destroyPopup()` after the parent
  //                      attach has been ended and before the scene tree is
  //                      reset. Default no-op. FileDialog overrides to
  //                      `m_dialog->onClose()` and reset its `unique_ptr`.
  virtual void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) = 0;
  virtual void layoutSheet(float contentWidth, float contentHeight) = 0;
  virtual void cancelToFacade() = 0;
  [[nodiscard]] virtual InputArea* initialFocusArea() = 0;

  [[nodiscard]] virtual float computePadding(float scale) const { return scale * 12.0f; }
  virtual void runUpdatePhase() {}
  [[nodiscard]] virtual bool preDispatchKeyboard(const KeyboardEvent& /*event*/) { return false; }
  virtual void onSheetClose() {}

  // Owned shared state. Subclasses don't manipulate these directly outside
  // of the `populateContent` / `layoutSheet` hooks where appropriate.
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  LayerPopupHostRegistry* m_popupHosts = nullptr;

  std::unique_ptr<PopupSurface> m_surface;
  popup_chrome::Geometry m_chrome;
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  Box* m_bgNode = nullptr;
  RectNode* m_panelShadow = nullptr;
  Node* m_contentNode = nullptr;
  InputDispatcher m_inputDispatcher;
  bool m_attachedToHost = false;
  wl_surface* m_parentSurface = nullptr;
  bool m_pointerInside = false;
  bool m_openInProgress = false;
  bool m_closeRequestedDuringOpen = false;

private:
  // Tear down any popup parented to this one (e.g. a glyph/color/file picker
  // opened from inside this popup) before this surface is destroyed. xdg-shell
  // requires popups be destroyed topmost-first. Resolved via the active-host
  // registry because the child may be an app-owned singleton this popup has no
  // direct handle to.
  void closeChildPopups();

  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(std::uint32_t width, std::uint32_t height);
  void layoutScene(float width, float height);
  [[nodiscard]] bool mapPointerEvent(const PointerEvent& event, float& localX, float& localY) const noexcept;
  [[nodiscard]] wl_surface* resolveEventSurface(const PointerEvent& event) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> resolveParentContext() const;
  void syncPointerStateFromCurrentPosition();
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  void markDirtyTail();
};
