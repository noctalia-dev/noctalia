#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/core/wallpaper_types.h"
#include "render/scene/node.h"
#include "render/scene/wallpaper_node.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <string>

class Box;

enum class WallpaperTransitionDirection {
  Forward,
  Reverse,
};

struct WallpaperInstance {
  std::uint32_t outputName = 0;
  struct wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::string connectorName;
  std::string description;

  std::unique_ptr<LayerSurface> surface;
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  Box* fillNode = nullptr;
  WallpaperNode* wallpaperNode = nullptr;

  // Wallpaper state
  std::string currentPath;
  std::string pendingPath;
  std::string queuedPath;
  WallpaperSourceKind currentSourceKind = WallpaperSourceKind::Image;
  WallpaperSourceKind nextSourceKind = WallpaperSourceKind::Image;
  Color currentColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  Color nextColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  TextureHandle currentTexture;
  TextureHandle nextTexture;

  // Transition state
  float transitionTime = 0.0f;
  bool transitioning = false;
  WallpaperTransitionDirection transitionDirection = WallpaperTransitionDirection::Forward;
  AnimationManager::Id transitionAnimId = 0;
  WallpaperTransition activeTransition = WallpaperTransition::Fade;
  TransitionParams transitionParams;
};
