#include "shell/settings/display/display_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace settings::display {

  namespace {

    constexpr double kTouchTolerance = 5.0;

    [[nodiscard]] int clampRange(int desired, int otherPos, int otherSize, int dragSize) {
      return std::max(otherPos - dragSize + 1, std::min(desired, otherPos + otherSize - 1));
    }

  } // namespace

  std::map<std::string, OutputSize> predictedSizes(
      const std::vector<compositors::display::OutputState>& outputs,
      const std::map<std::string, compositors::display::OutputState>& target
  ) {
    std::map<std::string, OutputSize> sizes;
    for (const auto& out : outputs) {
      const auto cfgIt = target.find(out.name);
      const auto& cfg = cfgIt != target.end() ? cfgIt->second : out;
      if (!cfg.enabled) {
        sizes[out.name] = {0, 0};
        continue;
      }
      int physW = 1920;
      int physH = 1080;
      if (!cfg.modeStr.empty()) {
        const auto at = cfg.modeStr.find('@');
        const auto modePart = cfg.modeStr.substr(0, at);
        const auto xPos = modePart.find('x');
        if (xPos != std::string::npos) {
          physW = std::stoi(modePart.substr(0, xPos));
          physH = std::stoi(modePart.substr(xPos + 1));
        }
      } else if (!out.modes.empty()) {
        const auto idx = std::clamp(out.currentModeIndex, 0, static_cast<int>(out.modes.size()) - 1);
        physW = out.modes[static_cast<std::size_t>(idx)].width;
        physH = out.modes[static_cast<std::size_t>(idx)].height;
      }
      if (compositors::display::rotatedTransform(cfg.transform)) {
        std::swap(physW, physH);
      }
      const double scale = cfg.scale > 0.0 ? cfg.scale : 1.0;
      sizes[out.name] = {static_cast<int>(std::floor(physW / scale)), static_cast<int>(std::floor(physH / scale))};
    }
    return sizes;
  }

  bool isTouching(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (std::abs(ax + aw - bx) <= kTouchTolerance && ay < by + bh && ay + ah > by)
      return true;
    if (std::abs(ax - (bx + bw)) <= kTouchTolerance && ay < by + bh && ay + ah > by)
      return true;
    if (std::abs(ay + ah - by) <= kTouchTolerance && ax < bx + bw && ax + aw > bx)
      return true;
    if (std::abs(ay - (by + bh)) <= kTouchTolerance && ax < bx + bw && ax + aw > bx)
      return true;
    return false;
  }

  void normalizeLayout(
      const std::vector<compositors::display::OutputState>& outputs,
      std::map<std::string, compositors::display::OutputState>& target
  ) {
    const auto sizes = predictedSizes(outputs, target);
    std::map<std::string, std::pair<int, int>> positions;
    std::vector<std::string> names;
    for (const auto& [name, cfg] : target) {
      if (cfg.enabled) {
        positions[name] = {cfg.x, cfg.y};
        names.push_back(name);
      }
    }
    if (names.size() <= 1) {
      return;
    }

    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 5) {
      changed = false;
      iterations++;

      std::vector<std::vector<std::string>> components;
      for (const auto& name : names) {
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < components.size(); ++i) {
          for (const auto& other : components[i]) {
            const auto& [ax, ay] = positions[name];
            const auto& [bx, by] = positions[other];
            if (isTouching(ax, ay, sizes.at(name).w, sizes.at(name).h, bx, by, sizes.at(other).w, sizes.at(other).h)) {
              found.push_back(i);
              break;
            }
          }
        }
        if (found.empty()) {
          components.push_back({name});
        } else {
          auto& targetComp = components[found[0]];
          targetComp.push_back(name);
          for (std::size_t j = 1; j < found.size(); ++j) {
            targetComp.insert(targetComp.end(), components[found[j]].begin(), components[found[j]].end());
            components[found[j]].clear();
          }
          components.erase(
              std::remove_if(components.begin(), components.end(), [](const auto& c) { return c.empty(); }),
              components.end()
          );
        }
      }

      if (components.size() > 1) {
        const auto& mainComp = components[0];
        double bestDist = std::numeric_limits<double>::max();
        std::size_t bestComp = 0;
        int bestDx = 0;
        int bestDy = 0;
        for (std::size_t i = 1; i < components.size(); ++i) {
          for (const auto& targetName : components[i]) {
            for (const auto& mainNode : mainComp) {
              const auto& [tx, ty] = positions[targetName];
              const auto& ts = sizes.at(targetName);
              const auto& [mx, my] = positions[mainNode];
              const auto& ms = sizes.at(mainNode);
              const int candidates[4][2] = {
                  {mx + ms.w, clampRange(ty, my, ms.h, ts.h)},
                  {mx - ts.w, clampRange(ty, my, ms.h, ts.h)},
                  {clampRange(tx, mx, ms.w, ts.w), my + ms.h},
                  {clampRange(tx, mx, ms.w, ts.w), my - ts.h},
              };
              for (const auto& candidate : candidates) {
                const double dist = std::pow(candidate[0] - tx, 2.0) + std::pow(candidate[1] - ty, 2.0);
                if (dist < bestDist) {
                  bestDist = dist;
                  bestComp = i;
                  bestDx = candidate[0] - tx;
                  bestDy = candidate[1] - ty;
                }
              }
            }
          }
        }
        for (const auto& node : components[bestComp]) {
          positions[node].first += bestDx;
          positions[node].second += bestDy;
        }
        changed = true;
      }
    }

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto& [name, pos] : positions) {
      minX = std::min(minX, pos.first);
      minY = std::min(minY, pos.second);
    }

    for (auto& [name, cfg] : target) {
      if (const auto posIt = positions.find(name); posIt != positions.end()) {
        cfg.x = posIt->second.first - minX;
        cfg.y = posIt->second.second - minY;
      }
    }
  }

} // namespace settings::display
