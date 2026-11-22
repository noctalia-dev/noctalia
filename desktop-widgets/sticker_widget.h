#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/node.h"

#include <functional>
#include <optional>
#include <string>

class Image;

class StickerWidget : public Node {
public:
  StickerWidget();

  bool setSourceFile(Renderer& renderer, const std::string& path);
  // The host retains ownership; dimensions are available before a pending texture arrives.
  void setExternalTexture(Renderer& renderer, TextureHandle texture, int imageWidth, int imageHeight);
  void clear(Renderer& renderer);
  void setAsyncReadyCallback(std::function<void()> callback);
  void setContentScale(float scale);

  bool hasImage() const noexcept;
  bool hasPendingContent() const noexcept override;
  float contentScale() const noexcept { return m_contentScale; }

private:
  void doLayout(Renderer& renderer) override;
  float aspectRatio() const noexcept;

  Image* m_image = nullptr;
  std::optional<float> m_externalAspect;
  float m_contentScale = 1.0F;
  std::function<void()> m_asyncReadyCallback;
};
