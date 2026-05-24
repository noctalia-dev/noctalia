#pragma once

#include "render/core/render_styles.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"

class TextureManager;

class GraphNode : public Node {
public:
  GraphNode() : Node(NodeType::Graph) {}
  ~GraphNode() override;

  GraphNode(const GraphNode&) = delete;
  GraphNode& operator=(const GraphNode&) = delete;

  [[nodiscard]] const GraphStyle& style() const noexcept { return m_style; }
  [[nodiscard]] TextureId textureId() const noexcept { return m_texture.id; }
  [[nodiscard]] int textureWidth() const noexcept { return m_texWidth; }

  void setLineColor1(const Color& color) {
    if (m_style.lineColor1 == color)
      return;
    m_style.lineColor1 = color;
    markPaintDirty();
  }

  void setLineColor2(const Color& color) {
    if (m_style.lineColor2 == color)
      return;
    m_style.lineColor2 = color;
    markPaintDirty();
  }

  void setLineColor3(const Color& color) {
    if (m_style.lineColor3 == color)
      return;
    m_style.lineColor3 = color;
    markPaintDirty();
  }

  void setCount1(float count) {
    if (m_style.count1 == count)
      return;
    m_style.count1 = count;
    markPaintDirty();
  }

  void setCount2(float count) {
    if (m_style.count2 == count)
      return;
    m_style.count2 = count;
    markPaintDirty();
  }

  void setCount3(float count) {
    if (m_style.count3 == count)
      return;
    m_style.count3 = count;
    markPaintDirty();
  }

  void setScroll1(float scroll) {
    m_style.scroll1 = scroll;
    markPaintDirty();
  }

  void setScroll2(float scroll) {
    m_style.scroll2 = scroll;
    markPaintDirty();
  }

  void setScroll3(float scroll) {
    m_style.scroll3 = scroll;
    markPaintDirty();
  }

  void setLineWidth(float width) {
    if (m_style.lineWidth == width)
      return;
    m_style.lineWidth = width;
    markPaintDirty();
  }

  void setGraphFillOpacity(float opacity) {
    if (m_style.graphFillOpacity == opacity)
      return;
    m_style.graphFillOpacity = opacity;
    markPaintDirty();
  }

  void setAaSize(float size) {
    if (m_style.aaSize == size)
      return;
    m_style.aaSize = size;
    markPaintDirty();
  }

  // Upload data to the backend texture. Must be called while a render context is current.
  // primary/secondary arrays contain normalized [0..1] values.
  // Pass nullptr and 0 for unused channels.
  void setData(
      TextureManager& textures, const float* primary, int primaryCount, const float* secondary, int secondaryCount,
      const float* tertiary = nullptr, int tertiaryCount = 0
  );

private:
  GraphStyle m_style;
  TextureManager* m_textureManager = nullptr;
  TextureHandle m_texture;
  int m_texWidth = 0;
  int m_texCapacity = 0;
};
