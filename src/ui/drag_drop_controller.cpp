#include "ui/drag_drop_controller.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/controls/drag_source.h"
#include "ui/controls/drop_zone.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace {

  bool isAncestorOrSelf(const Node* ancestor, const Node* node) {
    if (ancestor == nullptr || node == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  Node* safePreviewTarget(DragSource& source, Node* requested, const Node* overlayRoot) {
    if (requested == nullptr || overlayRoot == nullptr) {
      return requested;
    }

    // A render proxy lives below overlayRoot. Its source must therefore never
    // be overlayRoot or one of overlayRoot's ancestors, otherwise rendering the
    // source would reach the proxy again and recurse indefinitely. Keep the
    // highest requested ancestor that remains on the content-only branch.
    Node* safe = nullptr;
    for (Node* current = &source; current != nullptr; current = current->parent()) {
      if (isAncestorOrSelf(current, overlayRoot)) {
        break;
      }
      safe = current;
      if (current == requested) {
        break;
      }
    }
    return safe;
  }

} // namespace

DragDropController::~DragDropController() {
  cancel();
  for (auto* source : m_sources) {
    source->detachController(this);
  }
  for (auto* zone : m_dropZones) {
    zone->detachController(this);
  }
}

void DragDropController::setScale(float scale) noexcept { m_scale = std::max(scale, 0.01f); }

void DragDropController::setOverlayRoot(Node* root) {
  if (m_overlayRoot == root) {
    return;
  }
  cancel();
  m_overlayRoot = root;
}

void DragDropController::arm(DragSource& source, float localX, float localY) {
  cancel();
  if (!source.enabled() || source.dragType().empty() || source.payload().empty()) {
    return;
  }
  m_state = State::Armed;
  m_source = &source;
  m_dragType = source.dragType();
  m_payload = source.payload();
  m_previewTarget = safePreviewTarget(source, source.previewTarget(), m_overlayRoot);
  m_previewTargetOpacity = m_previewTarget != nullptr ? m_previewTarget->opacity() : 1.0f;
  m_previewTargetParticipatesInLayout = m_previewTarget == nullptr || m_previewTarget->participatesInLayout();
  scenePosition(source, localX, localY, m_startSceneX, m_startSceneY);
  float sourceSceneX = 0.0f;
  float sourceSceneY = 0.0f;
  Node::mapToScene(m_previewTarget != nullptr ? m_previewTarget : &source, 0.0f, 0.0f, sourceSceneX, sourceSceneY);
  m_pointerOffsetX = m_startSceneX - sourceSceneX;
  m_pointerOffsetY = m_startSceneY - sourceSceneY;
}

void DragDropController::motion(DragSource& source, float localX, float localY) {
  if (m_source != &source || m_state == State::Idle) {
    return;
  }
  float sceneX = 0.0f;
  float sceneY = 0.0f;
  scenePosition(source, localX, localY, sceneX, sceneY);
  if (m_state == State::Armed) {
    const float dx = sceneX - m_startSceneX;
    const float dy = sceneY - m_startSceneY;
    if (std::hypot(dx, dy) < Style::dragStartThreshold * m_scale) {
      return;
    }
    m_state = State::Dragging;
    source.setDragging(true);
    createPreview(sceneX, sceneY);
  }
  updatePreview(sceneX, sceneY);
  updateTarget(sceneX, sceneY);
}

void DragDropController::release(DragSource& source, float localX, float localY) {
  if (m_source != &source || m_state == State::Idle) {
    return;
  }
  if (m_state != State::Dragging) {
    cancel();
    return;
  }

  float sceneX = 0.0f;
  float sceneY = 0.0f;
  scenePosition(source, localX, localY, sceneX, sceneY);
  updateTarget(sceneX, sceneY);

  std::string callback;
  std::string payload;
  std::string targetValue;
  if (m_target != nullptr) {
    callback = m_target->onDrop();
    payload = m_payload;
    targetValue = m_targetValue;
  }
  clearState(true);
  if (!callback.empty() && m_dropCallback) {
    m_dropCallback(std::move(callback), std::move(payload), std::move(targetValue));
  }
}

void DragDropController::cancel() { clearState(true); }

void DragDropController::sourceChanged(DragSource* source) {
  if (source != nullptr && source == m_source) {
    cancel();
  }
}

void DragDropController::zoneChanged(DropZone* zone) {
  if (zone != nullptr && zone == m_target) {
    cancel();
  }
}

void DragDropController::registerSource(DragSource* source) {
  if (source != nullptr) {
    m_sources.insert(source);
  }
}

void DragDropController::registerDropZone(DropZone* zone) {
  if (zone != nullptr) {
    m_dropZones.insert(zone);
  }
}

void DragDropController::unregisterSource(DragSource* source) {
  m_sources.erase(source);
  if (source == nullptr || source != m_source) {
    return;
  }
  // Destructor context: the preview target is the source or one of its
  // ancestors, i.e. inside the chain being torn down right now, so restoring
  // its opacity/layout state here is unsafe. Every surviving-tree path cancels
  // through the reconciler or host before the node is destroyed; only detach
  // the proxy and drop the pointers.
  m_previewTarget = nullptr;
  clearTarget();
  clearPreview();
  m_source = nullptr;
  m_state = State::Idle;
  m_dragType.clear();
  m_payload.clear();
}

void DragDropController::unregisterDropZone(DropZone* zone) {
  m_dropZones.erase(zone);
  if (zone == nullptr || zone != m_target) {
    return;
  }
  m_target = nullptr;
  m_targetValue.clear();
  clearState(true);
}

void DragDropController::cancelIfParticipantIn(const Node* subtree) {
  if (subtree == nullptr) {
    return;
  }
  const auto belongsToSubtree = [subtree](const Node* participant) {
    for (const Node* node = participant; node != nullptr; node = node->parent()) {
      if (node == subtree) {
        return true;
      }
    }
    return false;
  };
  if (belongsToSubtree(m_source) || belongsToSubtree(m_target) || belongsToSubtree(m_previewTarget)) {
    cancel();
  }
}

void DragDropController::scenePosition(
    const DragSource& source, float localX, float localY, float& sceneX, float& sceneY
) const {
  Node::mapToScene(source.inputArea(), localX, localY, sceneX, sceneY);
}

DropZone* DragDropController::targetAt(float sceneX, float sceneY) const {
  if (m_source == nullptr) {
    return nullptr;
  }
  Node* root = m_source;
  while (root->parent() != nullptr) {
    root = root->parent();
  }
  // A thin insertion marker can expose a larger drag-only target without
  // changing layout or intercepting ordinary clicks. When expanded targets
  // overlap, the closest marker wins, which leaves effectively no dead band
  // while still making the insertion point deterministic. Proximity zones are
  // resolved before ancestor drop zones so a reorder marker inside a category
  // wins over the category's catch-all drop surface.
  DropZone* closest = nullptr;
  float closestDistance = std::numeric_limits<float>::max();
  std::size_t closestDepth = 0;
  for (DropZone* zone : m_dropZones) {
    if (zone == nullptr
        || zone->controller() != this
        || !zone->visible()
        || !zone->hitTestVisible()
        || !zone->enabled()
        || !zone->accepts(m_dragType)
        || zone->hitSlop() <= 0.0f) {
      continue;
    }

    bool insideAncestors = true;
    std::size_t depth = 0;
    for (Node* ancestor = zone->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
      ++depth;
      if (!ancestor->visible() || !ancestor->hitTestVisible() || !ancestor->containsScenePoint(sceneX, sceneY)) {
        insideAncestors = false;
        break;
      }
    }
    if (!insideAncestors) {
      continue;
    }

    float localX = 0.0f;
    float localY = 0.0f;
    (void)Node::mapFromScene(zone, sceneX, sceneY, localX, localY);
    const float slop = zone->hitSlop();
    if (localX < -slop || localX >= zone->width() + slop || localY < -slop || localY >= zone->height() + slop) {
      continue;
    }

    const float dx = localX < 0.0f ? -localX : std::max(0.0f, localX - zone->width());
    const float dy = localY < 0.0f ? -localY : std::max(0.0f, localY - zone->height());
    const float distance = dx * dx + dy * dy;
    if (distance < closestDistance || (distance == closestDistance && depth > closestDepth)) {
      closest = zone;
      closestDistance = distance;
      closestDepth = depth;
    }
  }
  if (closest != nullptr) {
    return closest;
  }

  for (Node* node = Node::hitTestStrict(root, sceneX, sceneY); node != nullptr; node = node->parent()) {
    auto* zone = dynamic_cast<DropZone*>(node);
    if (zone != nullptr && zone->controller() == this && zone->enabled() && zone->accepts(m_dragType)) {
      return zone;
    }
  }
  return nullptr;
}

void DragDropController::updateTarget(float sceneX, float sceneY) {
  DropZone* next = targetAt(sceneX, sceneY);
  if (next == m_target) {
    return;
  }
  clearTarget();
  m_target = next;
  if (m_target != nullptr) {
    m_targetValue = m_target->value();
    const float draggedHeight =
        m_previewTarget != nullptr ? m_previewTarget->height() : (m_source != nullptr ? m_source->height() : 0.0f);
    m_target->setDragOver(true, draggedHeight);
  }
}

void DragDropController::clearTarget() {
  if (m_target != nullptr) {
    m_target->setDragOver(false);
    m_target = nullptr;
  }
  m_targetValue.clear();
}

void DragDropController::createPreview(float sceneX, float sceneY) {
  if (m_preview != nullptr || m_overlayRoot == nullptr || m_source == nullptr || m_previewTarget == nullptr) {
    return;
  }
  auto preview = std::make_unique<RenderProxyNode>(m_previewTarget);
  preview->setFrameSize(m_previewTarget->width(), m_previewTarget->height());
  preview->setOpacity(0.96f);
  preview->setScale(1.01f);
  preview->setZIndex(std::numeric_limits<std::int32_t>::max());
  m_preview = static_cast<RenderProxyNode*>(m_overlayRoot->addChild(std::move(preview)));
  if (m_source->liftFromLayout()) {
    m_previewTarget->setOpacity(0.0f);
    m_previewTarget->setParticipatesInLayout(false);
  } else {
    m_previewTarget->setOpacity(m_previewTargetOpacity * 0.28f);
  }
  updatePreview(sceneX, sceneY);
}

void DragDropController::updatePreview(float sceneX, float sceneY) {
  if (m_preview == nullptr || m_overlayRoot == nullptr) {
    return;
  }
  float localX = 0.0f;
  float localY = 0.0f;
  if (Node::mapFromScene(m_overlayRoot, sceneX - m_pointerOffsetX, sceneY - m_pointerOffsetY, localX, localY)) {
    m_preview->setPosition(localX, localY);
  }
}

void DragDropController::clearPreview() {
  if (m_preview == nullptr) {
    if (m_previewTarget != nullptr) {
      m_previewTarget->setParticipatesInLayout(m_previewTargetParticipatesInLayout);
      m_previewTarget->setOpacity(m_previewTargetOpacity);
    }
    m_previewTarget = nullptr;
    return;
  }
  m_preview->setSource(nullptr);
  if (Node* parent = m_preview->parent(); parent != nullptr) {
    (void)parent->removeChild(m_preview);
  }
  m_preview = nullptr;
  if (m_previewTarget != nullptr) {
    m_previewTarget->setParticipatesInLayout(m_previewTargetParticipatesInLayout);
    m_previewTarget->setOpacity(m_previewTargetOpacity);
  }
  m_previewTarget = nullptr;
}

void DragDropController::clearState(bool restoreSourceVisual) {
  clearTarget();
  clearPreview();
  if (restoreSourceVisual && m_source != nullptr) {
    m_source->setDragging(false);
  }
  m_state = State::Idle;
  m_source = nullptr;
  m_dragType.clear();
  m_payload.clear();
}
