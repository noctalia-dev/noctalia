#pragma once

#include "notification/notification_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "system/icon_resolver.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ConfigService;
class Glyph;
class HttpClient;
class Input;
class InputArea;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;

class NotificationToast {
public:
  enum class RevealDirection { FromLeft, FromRight, FromTop, FromBottom };

  NotificationToast();
  ~NotificationToast();

  NotificationToast(const NotificationToast&) = delete;
  NotificationToast& operator=(const NotificationToast&) = delete;

  void initialize(
      WaylandConnection& wayland, ConfigService* config, NotificationManager* notifications,
      RenderContext* renderContext, HttpClient* httpClient = nullptr
  );
  void onConfigReload();
  void onOutputChange();
  void requestLayout();
  void requestRedraw();

  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  // Per-notification visual state (shared across all instances)
  struct PopupEntry {
    uint32_t notificationId = 0;
    std::string appName;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
    std::optional<std::string> icon;
    std::optional<NotificationImageData> imageData;
    Urgency urgency = Urgency::Normal;
    int displayDurationMs = 0; // -1 = persistent (no auto-dismiss)
    int32_t rawTimeoutMs = 0;  // raw DBus timeout; >0 means manager has an auto-expire timer we must coordinate with
    float remainingProgress = 1.0f;
    float y = -1.0f; // stable top position while visible; negative = queued/off-screen
    float height = 0.0f;
    // Planned toast chrome (refreshEntryGeometry); buildCard must match these for placement vs paint.
    int toastSummaryLines = 2;
    int toastBodyLines = 0;
    bool exiting = false;
    bool hovered = false; // pointer is currently over the card on some instance
    bool replyInputFocused = false;
  };

  // Per-output instance (each has its own surface, scene, animations)
  struct Instance {
    wl_output* output = nullptr;
    std::int32_t scale = 1;

    std::unique_ptr<LayerSurface> surface;
    // Declaration order matters: sceneRoot must be destroyed before `animations`,
    // because ~Node() calls cancelForOwner() on its AnimationManager.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
    bool rebuildRequested = false;

    // Per-entry visual nodes for this instance
    struct CardState {
      Node* cardNode = nullptr;
      Node* cardContent = nullptr;
      Node* cardForeground = nullptr;
      Node* cardBg = nullptr;
      Node* appIconNode = nullptr;
      Label* appNameLabel = nullptr;
      Label* summaryLabel = nullptr;
      Label* bodyLabel = nullptr;
      ProgressBar* progressBar = nullptr;
      Glyph* closeGlyph = nullptr;
      Node* actionsRowNode = nullptr;
      Node* inlineReplyRowNode = nullptr;
      Input* inlineReplyInput = nullptr;
      AnimationManager::Id countdownAnimId = 0;
      AnimationManager::Id entryAnimId = 0;
      AnimationManager::Id slideAnimId = 0;
      AnimationManager::Id exitAnimId = 0;
      bool replyMode = false;
    };
    std::vector<CardState> cards;
    float lastPointerX = 0.0f;
    float lastPointerY = 0.0f;
  };

  void onNotificationEvent(const Notification& n, NotificationEvent event);
  void addPopup(const Notification& n);
  void dismissPopup(std::size_t index);
  void requestClose(uint32_t notificationId, CloseReason reason);
  void removePopup(uint32_t notificationId);
  void finishRemoval(uint32_t notificationId);
  void updateInputRegion(Instance& inst) const;
  void enterInlineReplyMode(uint32_t notificationId);
  void submitInlineReply(uint32_t notificationId, const std::string& replyText);
  void syncKeyboardInteractivity(Instance& inst) const;
  static void clearInlineReplyFocus(Instance& inst);
  [[nodiscard]] static bool isInlineReplyInputArea(const Instance& inst, const InputArea* area);
  [[nodiscard]] static bool pointerHitsInlineReplyInput(const Instance& inst, const Node* hit);
  [[nodiscard]] static bool inputAreaBelongsToCard(const Instance::CardState& card, const InputArea* area);

  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, uint32_t width, uint32_t height);
  InputArea* buildCard(
      const PopupEntry& entry, Node** outCardContent, Node** outCardForeground, Label** outAppName, Label** outSummary,
      Label** outBody, Node** outBg, Node** outAppIcon, ProgressBar** outProgress, Glyph** outCloseGlyph,
      Node** outActionsRow, Node** outInlineReplyRow, Input** outInlineReplyInput
  );
  void applyCardReveal(Instance::CardState& cs, float reveal, float y, float cardHeight) const;
  [[nodiscard]] float cardReveal(const Instance::CardState& cs, float cardHeight) const;
  void addCardToInstance(Instance& inst, std::size_t entryIndex);
  void removeCardFromInstance(Instance& inst, std::size_t entryIndex);
  void syncEntryVisibility(std::size_t entryIndex);
  void dismissCardFromInstance(Instance& inst, std::size_t entryIndex);

  PopupEntry* findEntry(uint32_t notificationId);
  Instance::CardState* findCardState(Instance& inst, uint32_t notificationId);
  void pauseCountdowns(uint32_t notificationId);
  void resumeCountdowns(uint32_t notificationId);
  void revealQueuedEntries();
  void evictOverlappingEntries(std::size_t anchorIndex);
  [[nodiscard]] bool hasPlacement(const PopupEntry& entry) const;
  [[nodiscard]] bool
  canKeepPlacement(const PopupEntry& entry, std::optional<uint32_t> ignoreNotificationId = std::nullopt) const;
  [[nodiscard]] bool fitsOnSurface(const PopupEntry& entry, float surfaceHeight) const;
  [[nodiscard]] float entryHeight(const PopupEntry& entry) const;
  [[nodiscard]] std::string notificationPosition() const;
  [[nodiscard]] std::string notificationLayer() const;
  [[nodiscard]] std::vector<std::string> notificationMonitors() const;
  [[nodiscard]] bool shouldRenderOnOutput(const WaylandOutput& output) const;
  [[nodiscard]] bool isBottomStacking() const;
  [[nodiscard]] RevealDirection revealDirection() const;
  void refreshEntryGeometry(PopupEntry& entry) const;
  [[nodiscard]] float layoutBottomForSurfaceHeight(float surfaceHeight) const;
  [[nodiscard]] float maxPlacementBottom() const;
  [[nodiscard]] float entryOffsetFromPlacementBottom(const PopupEntry& entry) const;
  [[nodiscard]] float entryYForSurface(const PopupEntry& entry, float surfaceHeight) const;
  void alignBottomStackToPlacementBottom();
  [[nodiscard]] std::optional<float>
  findPlacementY(float entryHeight, std::optional<uint32_t> ignoreNotificationId = std::nullopt) const;
  [[nodiscard]] uint32_t surfaceHeightForOutput(wl_output* output) const;
  [[nodiscard]] std::string resolveNotificationIconPath(const PopupEntry& entry);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  NotificationManager* m_notifications = nullptr;
  RenderContext* m_renderContext = nullptr;
  HttpClient* m_httpClient = nullptr;

  std::vector<PopupEntry> m_entries;
  std::vector<std::unique_ptr<Instance>> m_instances;
  int m_callbackToken = -1;
  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_remoteIconCache;
  std::unordered_set<std::string> m_pendingRemoteIconDownloads;
  std::unordered_set<std::string> m_failedRemoteIconDownloads;
  std::string m_lastPosition;
  std::string m_lastLayer;
  std::vector<std::string> m_lastMonitorSelectors;
};
