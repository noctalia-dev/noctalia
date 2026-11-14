#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/shared_texture_cache.h"
#include "render/scene/node.h"
#include "shell/render/wallpaper_node.h"
#include "shell/render/wallpaper_types.h"
#include "shell/wayland/layer_surface.h"

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
  Color currentColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  Color nextColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  SharedTextureCache::Lease currentImage;
  SharedTextureCache::Lease nextImage;
  // Image still decoding; it starts the change to loadingPath once its texture is uploaded.
  SharedTextureCache::Lease loadingImage;
  std::string loadingPath;

  // Transition state
  float transitionTime = 0.0F;
  bool transitioning = false;
  WallpaperTransitionDirection transitionDirection = WallpaperTransitionDirection::Forward;
  AnimationManager::Id transitionAnimId = 0;
  WallpaperTransition activeTransition = WallpaperTransition::Fade;
  TransitionParams transitionParams;

  // Sync batch coordination (syncTransitions): member of the current change batch, waiting for
  // every member's image to be ready before all transitions start together. syncReady means the
  // image is decoded and pending; syncSkipped means this instance will not transition in this batch.
  bool syncMember = false;
  bool syncReady = false;
  bool syncSkipped = false;
};
