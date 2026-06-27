#pragma once

#include "render/scene/node.h"

#include <cstdint>

struct Color;
enum class ScreenCornerPosition : std::uint8_t;
class ScreenCornerNode;

class ScreenCorner : public Node {
public:
  ScreenCorner();

  void setColor(const Color& color);
  void setCorner(ScreenCornerPosition position);
  void setExponent(float exponent);
  void setSoftness(float softness);

  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);

private:
  ScreenCornerNode* m_corner = nullptr;
};
