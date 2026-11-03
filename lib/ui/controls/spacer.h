#pragma once

#include "render/scene/node.h"

class Spacer : public Node {
public:
  Spacer();

protected:
  void doLayout(Renderer& renderer) override;
};
