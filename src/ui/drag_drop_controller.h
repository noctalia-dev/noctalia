#pragma once

#include <functional>
#include <string>
#include <unordered_set>

class DragSource;
class DropZone;
class Node;
class RenderProxyNode;

class DragDropController {
public:
  using DropCallback = std::function<void(std::string callback, std::string payload, std::string target)>;

  enum class State {
    Idle,
    Armed,
    Dragging,
  };

  ~DragDropController();

  void setDropCallback(DropCallback callback) { m_dropCallback = std::move(callback); }
  void setScale(float scale) noexcept;
  void setOverlayRoot(Node* root);

  void arm(DragSource& source, float localX, float localY);
  void motion(DragSource& source, float localX, float localY);
  void release(DragSource& source, float localX, float localY);
  void cancel();

  void sourceChanged(DragSource* source);
  void zoneChanged(DropZone* zone);
  void registerSource(DragSource* source);
  void registerDropZone(DropZone* zone);
  void unregisterSource(DragSource* source);
  void unregisterDropZone(DropZone* zone);
  void cancelIfParticipantIn(const Node* subtree);

  [[nodiscard]] State state() const noexcept { return m_state; }
  [[nodiscard]] DragSource* activeSource() const noexcept { return m_source; }
  [[nodiscard]] DropZone* currentTarget() const noexcept { return m_target; }

private:
  void scenePosition(const DragSource& source, float localX, float localY, float& sceneX, float& sceneY) const;
  [[nodiscard]] DropZone* targetAt(float sceneX, float sceneY) const;
  void updateTarget(float sceneX, float sceneY);
  void clearTarget();
  void createPreview(float sceneX, float sceneY);
  void updatePreview(float sceneX, float sceneY);
  void clearPreview();
  void clearState(bool restoreSourceVisual);

  DropCallback m_dropCallback;
  State m_state = State::Idle;
  DragSource* m_source = nullptr;
  DropZone* m_target = nullptr;
  std::string m_dragType;
  std::string m_payload;
  std::string m_targetValue;
  float m_scale = 1.0f;
  float m_startSceneX = 0.0f;
  float m_startSceneY = 0.0f;
  float m_pointerOffsetX = 0.0f;
  float m_pointerOffsetY = 0.0f;
  Node* m_overlayRoot = nullptr;
  RenderProxyNode* m_preview = nullptr;
  Node* m_previewTarget = nullptr;
  float m_previewTargetOpacity = 1.0f;
  bool m_previewTargetParticipatesInLayout = true;
  std::unordered_set<DragSource*> m_sources;
  std::unordered_set<DropZone*> m_dropZones;
};
