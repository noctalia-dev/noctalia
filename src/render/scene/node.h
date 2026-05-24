#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct Mat3;

class AnimationManager;
class Renderer;
class SelectPopupContext;

enum class NodeType : std::uint8_t {
  Base,
  Rect,
  Text,
  Image,
  Glyph,
  Spinner,
  ScreenCorner,
  AudioSpectrum,
  Effect,
  Graph,
  Wallpaper,
};

enum class NodeInvalidation : std::uint8_t {
  Paint,
  Layout,
};

struct LayoutSize {
  float width = 0.0f;
  float height = 0.0f;
};

struct LayoutRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct HitTestOutset {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct LayoutConstraints {
  float minWidth = 0.0f;
  float minHeight = 0.0f;
  float maxWidth = 0.0f;
  float maxHeight = 0.0f;
  bool hasMaxWidth = false;
  bool hasMaxHeight = false;

  static LayoutConstraints unconstrained() noexcept;
  static LayoutConstraints available(float width, float height) noexcept;
  static LayoutConstraints exact(float width, float height) noexcept;

  void setMaxWidth(float width) noexcept;
  void setMaxHeight(float height) noexcept;
  void setExactWidth(float width) noexcept;
  void setExactHeight(float height) noexcept;

  [[nodiscard]] bool hasExactWidth() const noexcept;
  [[nodiscard]] bool hasExactHeight() const noexcept;
  [[nodiscard]] float constrainWidth(float width) const noexcept;
  [[nodiscard]] float constrainHeight(float height) const noexcept;
  [[nodiscard]] LayoutSize constrain(LayoutSize size) const noexcept;
};

class Node {
public:
  explicit Node(NodeType type = NodeType::Base);
  virtual ~Node();

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  [[nodiscard]] NodeType type() const noexcept { return m_type; }

  [[nodiscard]] float x() const noexcept { return m_x; }
  [[nodiscard]] float y() const noexcept { return m_y; }
  [[nodiscard]] float width() const noexcept { return m_width; }
  [[nodiscard]] float height() const noexcept { return m_height; }
  [[nodiscard]] float rotation() const noexcept { return m_rotation; }
  [[nodiscard]] float scale() const noexcept { return m_scale; }
  [[nodiscard]] float opacity() const noexcept { return m_opacity; }
  [[nodiscard]] float flexGrow() const noexcept { return m_flexGrow; }
  [[nodiscard]] bool visible() const noexcept { return m_visible; }
  [[nodiscard]] bool participatesInLayout() const noexcept { return m_participatesInLayout; }
  [[nodiscard]] bool paintDirty() const noexcept { return m_paintDirty; }
  [[nodiscard]] bool layoutDirty() const noexcept { return m_layoutDirty; }
  [[nodiscard]] bool clipChildren() const noexcept { return m_clipChildren; }
  [[nodiscard]] bool hitTestVisible() const noexcept { return m_hitTestVisible; }
  [[nodiscard]] HitTestOutset hitTestOutset() const noexcept { return m_hitTestOutset; }
  [[nodiscard]] bool sizeAssignedByLayout() const noexcept { return m_sizeAssignedByLayout; }
  [[nodiscard]] bool arrangingByLayout() const noexcept { return m_arranging; }
  [[nodiscard]] std::int32_t zIndex() const noexcept { return m_zIndex; }
  [[nodiscard]] Node* parent() const noexcept { return m_parent; }
  [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const noexcept { return m_children; }

  void setPosition(float x, float y);
  virtual void setSize(float width, float height);
  void setFrameSize(float width, float height);
  void setRotation(float radians);
  void setScale(float scale);
  void setOpacity(float opacity);
  void setFlexGrow(float grow);
  void setVisible(bool visible);
  void setParticipatesInLayout(bool participatesInLayout);
  void setClipChildren(bool clipChildren);
  void setHitTestVisible(bool hitTestVisible);
  void setHitTestOutset(const HitTestOutset& outset);
  void setZIndex(std::int32_t zIndex);

  Node* addChild(std::unique_ptr<Node> child);
  // Insert at a specific vector position to control Flex layout order (not rendering order — use zIndex for that).
  Node* insertChildAt(std::size_t index, std::unique_ptr<Node> child);
  std::unique_ptr<Node> removeChild(Node* child);

  void setAnimationManager(AnimationManager* mgr);
  [[nodiscard]] AnimationManager* animationManager() const noexcept { return m_animationManager; }
  void setPopupContext(SelectPopupContext* ctx);
  [[nodiscard]] SelectPopupContext* popupContext() const noexcept { return m_popupContext; }
  void setInvalidationCallback(std::function<void(NodeInvalidation)> callback);
  void layout(Renderer& renderer);
  [[nodiscard]] LayoutSize measure(Renderer& renderer, const LayoutConstraints& constraints);
  void arrange(Renderer& renderer, const LayoutRect& rect);
  [[nodiscard]] bool containsScenePoint(float sceneX, float sceneY) const;

  void setUserData(void* data) noexcept { m_userData = data; }
  [[nodiscard]] void* userData() const noexcept { return m_userData; }

  static Node* hitTest(Node* root, float x, float y);
  static void absolutePosition(const Node* node, float& outX, float& outY);
  static bool mapFromScene(const Node* node, float sceneX, float sceneY, float& outLocalX, float& outLocalY);
  static void transformedBounds(const Node* node, float& outLeft, float& outTop, float& outRight, float& outBottom);
  static void transformedBounds(
      const Node* node, const Mat3& world, float& outLeft, float& outTop, float& outRight, float& outBottom
  );

  void markPaintDirty();
  void markLayoutDirty();
  void clearDirty();

protected:
  virtual void doLayout(Renderer& renderer);
  virtual LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints);
  virtual void doArrange(Renderer& renderer, const LayoutRect& rect);

private:
  static Node* hitTestImpl(Node* node, float px, float py);
  NodeType m_type;
  float m_x = 0.0f;
  float m_y = 0.0f;
  float m_width = 0.0f;
  float m_height = 0.0f;
  float m_rotation = 0.0f;
  float m_scale = 1.0f;
  float m_opacity = 1.0f;
  float m_flexGrow = 0.0f;
  bool m_visible = true;
  bool m_participatesInLayout = true;
  bool m_paintDirty = true;
  bool m_layoutDirty = true;
  bool m_clipChildren = false;
  bool m_hitTestVisible = true;
  HitTestOutset m_hitTestOutset{};
  bool m_sizeAssignedByLayout = false;
  bool m_arranging = false;
  std::int32_t m_zIndex = 0;
  void* m_userData = nullptr;
  AnimationManager* m_animationManager = nullptr;
  SelectPopupContext* m_popupContext = nullptr;
  std::function<void(NodeInvalidation)> m_invalidationCallback;
  Node* m_parent = nullptr;
  std::vector<std::unique_ptr<Node>> m_children;

  void propagatePaintDirty();
  void propagateLayoutDirty();
  void notifyInvalidated(NodeInvalidation invalidation);
};
