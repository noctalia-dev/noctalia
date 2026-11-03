#pragma once

#include "render/core/mat3.h"
#include "render/scene/node.h"

class RenderBackend;

struct CustomDrawContext {
  // Surface size in logical pixels.
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  // Buffer pixels per logical pixel.
  float pixelScaleX = 1.0F;
  float pixelScaleY = 1.0F;
  Mat3 transform = Mat3::identity();
  // Node opacity multiplied by every ancestor's.
  float opacity = 1.0F;
  float width = 0.0F;
  float height = 0.0F;
};

// A node that paints itself through the backend instead of a built-in primitive. RenderContext
// calls draw() during Render with the clip already applied; children render afterwards as usual.
// Shader state lives in a RenderExtension fetched with RenderBackend::extension<T>().
class CustomDrawNode : public Node {
public:
  CustomDrawNode() : Node(NodeType::CustomDraw) {}

  virtual void draw(RenderBackend& backend, const CustomDrawContext& context) const = 0;
};
