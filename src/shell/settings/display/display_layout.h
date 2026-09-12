#pragma once

#include "compositors/display_backend.h"

#include <map>
#include <string>
#include <vector>

namespace settings::display {

  struct OutputSize {
    int w = 0;
    int h = 0;
  };

  // Predicted logical sizes from pending mode/transform/scale; disabled outputs are 0x0.
  [[nodiscard]] std::map<std::string, OutputSize> predictedSizes(
      const std::vector<compositors::display::OutputState>& outputs,
      const std::map<std::string, compositors::display::OutputState>& target
  );

  [[nodiscard]] bool isTouching(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);

  // Moves disconnected components so every enabled output is edge-adjacent to the layout,
  // rebases to non-negative coordinates, and stamps the predicted sizes into the target.
  void normalizeLayout(
      const std::vector<compositors::display::OutputState>& outputs,
      std::map<std::string, compositors::display::OutputState>& target
  );

} // namespace settings::display
