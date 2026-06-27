#include "render/scene/node.h"

#include "core/ui_phase.h"
#include "render/animation/animation_manager.h"
#include "render/core/mat3.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace {

  Mat3 localTransform(const Node* node) {
    const float cx = node->width() * 0.5f;
    const float cy = node->height() * 0.5f;
    return Mat3::translation(node->x(), node->y())
        * Mat3::translation(cx, cy)
        * Mat3::rotation(node->rotation())
        * Mat3::scale(node->scaleX(), node->scaleY())
        * Mat3::translation(-cx, -cy);
  }

  Mat3 computeWorldTransform(const Node* node) {
    Mat3 world = Mat3::identity();
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      world = localTransform(current) * world;
    }
    return world;
  }

} // namespace

LayoutConstraints LayoutConstraints::unconstrained() noexcept { return {}; }

LayoutConstraints LayoutConstraints::available(float width, float height) noexcept {
  LayoutConstraints constraints;
  constraints.setMaxWidth(width);
  constraints.setMaxHeight(height);
  return constraints;
}

LayoutConstraints LayoutConstraints::exact(float width, float height) noexcept {
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  constraints.setExactHeight(height);
  return constraints;
}

void LayoutConstraints::setMaxWidth(float width) noexcept {
  maxWidth = std::max(0.0f, width);
  hasMaxWidth = true;
}

void LayoutConstraints::setMaxHeight(float height) noexcept {
  maxHeight = std::max(0.0f, height);
  hasMaxHeight = true;
}

void LayoutConstraints::setExactWidth(float width) noexcept {
  minWidth = std::max(0.0f, width);
  setMaxWidth(minWidth);
}

void LayoutConstraints::setExactHeight(float height) noexcept {
  minHeight = std::max(0.0f, height);
  setMaxHeight(minHeight);
}

bool LayoutConstraints::hasExactWidth() const noexcept { return hasMaxWidth && minWidth == maxWidth; }

bool LayoutConstraints::hasExactHeight() const noexcept { return hasMaxHeight && minHeight == maxHeight; }

float LayoutConstraints::constrainWidth(float width) const noexcept {
  float constrained = std::max(width, minWidth);
  if (hasMaxWidth) {
    constrained = std::min(constrained, maxWidth);
  }
  return constrained;
}

float LayoutConstraints::constrainHeight(float height) const noexcept {
  float constrained = std::max(height, minHeight);
  if (hasMaxHeight) {
    constrained = std::min(constrained, maxHeight);
  }
  return constrained;
}

LayoutSize LayoutConstraints::constrain(LayoutSize size) const noexcept {
  return LayoutSize{.width = constrainWidth(size.width), .height = constrainHeight(size.height)};
}

Node::Node(NodeType type) : m_type(type) {}

Node::~Node() {
  // Ensure any animation still targeting this node is cancelled before the node goes away.
  // Call sites that pass `this` as the animation owner get lifetime safety for free.
  if (m_animationManager != nullptr) {
    m_animationManager->cancelForOwner(this);
  }
}

void Node::layout(Renderer& renderer) {
  uiAssertNotRendering("Node::layout");
  doLayout(renderer);
}

LayoutSize Node::measure(Renderer& renderer, const LayoutConstraints& constraints) {
  uiAssertNotRendering("Node::measure");
  return doMeasure(renderer, constraints);
}

void Node::arrange(Renderer& renderer, const LayoutRect& rect) {
  uiAssertNotRendering("Node::arrange");
  m_arranging = true;
  m_sizeAssignedByLayout = true;
  doArrange(renderer, rect);
  m_arranging = false;
  m_sizeAssignedByLayout = true;
}

void Node::invalidateGpuResources(Renderer& renderer, std::uint64_t generation) {
  uiAssertNotRendering("Node::invalidateGpuResources");
  if (m_gpuResourceGeneration != generation) {
    doInvalidateGpuResources(renderer);
    m_gpuResourceGeneration = generation;
  }
  for (auto& child : m_children) {
    child->invalidateGpuResources(renderer, generation);
  }
}

bool Node::containsScenePoint(float sceneX, float sceneY) const {
  float localX = 0.0f;
  float localY = 0.0f;
  return pointInsideNode(this, sceneX, sceneY, localX, localY, false);
}

void Node::doLayout(Renderer& renderer) {
  for (auto& child : m_children) {
    if (child->visible()) {
      child->layout(renderer);
    }
  }
}

LayoutSize Node::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  doLayout(renderer);
  return constraints.constrain(LayoutSize{.width = width(), .height = height()});
}

void Node::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setSize(rect.width, rect.height);
  doLayout(renderer);
}

void Node::doInvalidateGpuResources(Renderer& renderer) { (void)renderer; }

bool Node::containsLocalPoint(float localX, float localY, bool includeHitOutset) const {
  if (!includeHitOutset) {
    return localX >= 0.0f && localX < width() && localY >= 0.0f && localY < height();
  }
  const HitTestOutset outset = hitTestOutset();
  return localX >= -outset.left
      && localX < width() + outset.right
      && localY >= -outset.top
      && localY < height() + outset.bottom;
}

bool Node::pointInsideNode(
    const Node* node, float sceneX, float sceneY, float& localX, float& localY, bool includeHitOutset
) {
  if (node == nullptr) {
    return false;
  }

  const Mat3 inverse = computeWorldTransform(node).inverse();
  const Vec2 local = inverse.transformPoint(sceneX, sceneY);
  localX = local.x;
  localY = local.y;
  return node->containsLocalPoint(localX, localY, includeHitOutset);
}

void Node::setPosition(float x, float y) {
  if (m_x == x && m_y == y) {
    return;
  }
  m_x = x;
  m_y = y;
  markPaintDirty();
}

void Node::setSize(float width, float height) {
  if (!m_arranging) {
    m_sizeAssignedByLayout = false;
  }
  if (m_width == width && m_height == height) {
    return;
  }
  m_width = width;
  m_height = height;
  markLayoutDirty();
}

void Node::setFrameSize(float width, float height) {
  if (!m_arranging) {
    m_sizeAssignedByLayout = false;
  }
  if (m_width == width && m_height == height) {
    return;
  }
  m_width = width;
  m_height = height;
  markPaintDirty();
}

void Node::setRotation(float radians) {
  if (m_rotation == radians) {
    return;
  }
  m_rotation = radians;
  markPaintDirty();
}

void Node::setScale(float scale) { setScale(scale, scale); }

void Node::setScale(float scaleX, float scaleY) {
  if (m_scaleX == scaleX && m_scaleY == scaleY) {
    return;
  }
  m_scaleX = scaleX;
  m_scaleY = scaleY;
  markPaintDirty();
}

void Node::setOpacity(float opacity) {
  if (m_opacity == opacity) {
    return;
  }
  m_opacity = opacity;
  markPaintDirty();
}

void Node::setFlexGrow(float grow) {
  if (m_flexGrow == grow) {
    return;
  }
  m_flexGrow = grow;
  markLayoutDirty();
}

void Node::setVisible(bool visible) {
  if (m_visible == visible) {
    return;
  }
  m_visible = visible;
  markLayoutDirty();
}

void Node::setParticipatesInLayout(bool participatesInLayout) {
  if (m_participatesInLayout == participatesInLayout) {
    return;
  }
  uiAssertSceneMutationAllowed("Node::setParticipatesInLayout");
  m_participatesInLayout = participatesInLayout;
  markLayoutDirty();
}

void Node::setClipChildren(bool clipChildren) {
  if (m_clipChildren == clipChildren) {
    return;
  }
  m_clipChildren = clipChildren;
  markPaintDirty();
}

void Node::setHitTestVisible(bool hitTestVisible) { m_hitTestVisible = hitTestVisible; }

void Node::setHitTestOutset(const HitTestOutset& outset) {
  const HitTestOutset clamped{
      .left = std::max(0.0f, outset.left),
      .top = std::max(0.0f, outset.top),
      .right = std::max(0.0f, outset.right),
      .bottom = std::max(0.0f, outset.bottom),
  };
  if (m_hitTestOutset.left == clamped.left
      && m_hitTestOutset.top == clamped.top
      && m_hitTestOutset.right == clamped.right
      && m_hitTestOutset.bottom == clamped.bottom) {
    return;
  }
  m_hitTestOutset = clamped;
}

void Node::setZIndex(std::int32_t zIndex) {
  if (m_zIndex == zIndex) {
    return;
  }
  m_zIndex = zIndex;
  markPaintDirty();
}

void Node::setExcludeSubtreeFromTabOrder(bool exclude) noexcept { m_excludeSubtreeFromTabOrder = exclude; }

void Node::setAnimationManager(AnimationManager* mgr) {
  m_animationManager = mgr;
  for (auto& child : m_children) {
    child->setAnimationManager(mgr);
  }
}

void Node::setPopupContext(SelectPopupContext* ctx) {
  m_popupContext = ctx;
  for (auto& child : m_children) {
    child->setPopupContext(ctx);
  }
}

void Node::setInvalidationCallback(std::function<void(NodeInvalidation)> callback) {
  m_invalidationCallback = std::move(callback);
}

Node* Node::addChild(std::unique_ptr<Node> child) {
  uiAssertSceneMutationAllowed("Node::addChild");
  child->m_parent = this;
  if (m_animationManager != nullptr && child->m_animationManager == nullptr) {
    child->setAnimationManager(m_animationManager);
  }
  if (m_popupContext != nullptr && child->m_popupContext == nullptr) {
    child->setPopupContext(m_popupContext);
  }
  auto* raw = child.get();
  m_children.push_back(std::move(child));
  markLayoutDirty();
  return raw;
}

Node* Node::insertChildAt(std::size_t index, std::unique_ptr<Node> child) {
  uiAssertSceneMutationAllowed("Node::insertChildAt");
  child->m_parent = this;
  if (m_animationManager != nullptr && child->m_animationManager == nullptr) {
    child->setAnimationManager(m_animationManager);
  }
  if (m_popupContext != nullptr && child->m_popupContext == nullptr) {
    child->setPopupContext(m_popupContext);
  }
  auto* raw = child.get();
  if (index >= m_children.size()) {
    m_children.push_back(std::move(child));
  } else {
    m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
  }
  markLayoutDirty();
  return raw;
}

std::unique_ptr<Node> Node::removeChild(Node* child) {
  uiAssertSceneMutationAllowed("Node::removeChild");
  auto it = std::ranges::find_if(m_children, [child](const auto& ptr) { return ptr.get() == child; });

  if (it == m_children.end()) {
    return nullptr;
  }

  auto removed = std::move(*it);
  m_children.erase(it);
  removed->m_parent = nullptr;
  markLayoutDirty();
  return removed;
}

void Node::markPaintDirty() { propagatePaintDirty(); }

void Node::markLayoutDirty() {
  propagateLayoutDirty();
  propagatePaintDirty();
}

void Node::propagatePaintDirty() {
  if (m_paintDirty) {
    return;
  }
  m_paintDirty = true;
  if (m_parent != nullptr) {
    m_parent->propagatePaintDirty();
  } else {
    notifyInvalidated(NodeInvalidation::Paint);
  }
}

void Node::propagateLayoutDirty() {
  if (m_layoutDirty) {
    return;
  }
  m_layoutDirty = true;
  if (m_parent != nullptr) {
    m_parent->propagateLayoutDirty();
  } else {
    notifyInvalidated(NodeInvalidation::Layout);
  }
}

void Node::notifyInvalidated(NodeInvalidation invalidation) {
  if (m_invalidationCallback) {
    m_invalidationCallback(invalidation);
  }
}

void Node::clearDirty() {
  m_paintDirty = false;
  m_layoutDirty = false;
  for (auto& child : m_children) {
    if (child->paintDirty() || child->layoutDirty()) {
      child->clearDirty();
    }
  }
}

Node* Node::hitTest(Node* root, float x, float y) { return hitTestImpl(root, x, y); }

Node* Node::hitTestImpl(Node* node, float px, float py) {
  if (node == nullptr || !node->m_visible || !node->m_hitTestVisible) {
    return nullptr;
  }

  float localX = 0.0f;
  float localY = 0.0f;
  const bool inside = pointInsideNode(node, px, py, localX, localY, true);

  if (node->m_clipChildren && !inside) {
    return nullptr;
  }

  // Fast path: children are already in zIndex order (the common case). Skip
  // allocating/sorting a side vector and iterate the child list directly in
  // reverse. Only fall back to the sorted copy when there's an out-of-order
  // pair, which removes a per-node heap allocation from every pointer event.
  auto& children = node->m_children;
  bool childrenSorted = true;
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i]->zIndex() < children[i - 1]->zIndex()) {
      childrenSorted = false;
      break;
    }
  }

  // Traverse children in reverse (topmost first).
  // Children are allowed to overflow parent bounds (needed for menus/popovers).
  if (childrenSorted) {
    for (const auto& child : std::views::reverse(children)) {
      auto* hit = hitTestImpl(child.get(), px, py);
      if (hit != nullptr) {
        return hit;
      }
    }
  } else {
    std::vector<Node*> orderedChildren;
    orderedChildren.reserve(children.size());
    for (auto& child : children) {
      orderedChildren.push_back(child.get());
    }
    std::ranges::stable_sort(orderedChildren, [](const Node* a, const Node* b) { return a->zIndex() < b->zIndex(); });
    for (Node* child : std::views::reverse(orderedChildren)) {
      auto* hit = hitTestImpl(child, px, py);
      if (hit != nullptr) {
        return hit;
      }
    }
  }

  return inside ? node : nullptr;
}

void Node::absolutePosition(const Node* node, float& outX, float& outY) {
  const Vec2 topLeft = computeWorldTransform(node).transformPoint(0.0f, 0.0f);
  outX = topLeft.x;
  outY = topLeft.y;
}

bool Node::mapFromScene(const Node* node, float sceneX, float sceneY, float& outLocalX, float& outLocalY) {
  if (node == nullptr) {
    outLocalX = 0.0f;
    outLocalY = 0.0f;
    return false;
  }

  return pointInsideNode(node, sceneX, sceneY, outLocalX, outLocalY, false);
}

void Node::transformedBounds(
    const Node* node, const Mat3& world, float& outLeft, float& outTop, float& outRight, float& outBottom
) {
  const Vec2 corners[] = {
      world.transformPoint(0.0f, 0.0f),
      world.transformPoint(node->width(), 0.0f),
      world.transformPoint(node->width(), node->height()),
      world.transformPoint(0.0f, node->height()),
  };

  outLeft = corners[0].x;
  outTop = corners[0].y;
  outRight = outLeft;
  outBottom = outTop;

  for (const Vec2 corner : corners) {
    outLeft = std::min(outLeft, corner.x);
    outTop = std::min(outTop, corner.y);
    outRight = std::max(outRight, corner.x);
    outBottom = std::max(outBottom, corner.y);
  }
}

void Node::transformedBounds(const Node* node, float& outLeft, float& outTop, float& outRight, float& outBottom) {
  transformedBounds(node, computeWorldTransform(node), outLeft, outTop, outRight, outBottom);
}
