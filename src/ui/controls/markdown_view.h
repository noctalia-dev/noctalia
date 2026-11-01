#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class AsyncTextureCache;
class Image;
class Label;
class Renderer;

// Where a MarkdownView loads the images a document references. Without one, images render as their
// alt text.
struct MarkdownImageSource {
  Renderer* renderer = nullptr;
  // Decodes off the main thread when set; null decodes synchronously.
  AsyncTextureCache* textureCache = nullptr;
  // Maps an image reference to a local file: nullopt when this source cannot load the reference
  // (it renders as alt text), an empty path while the file is still being fetched.
  std::function<std::optional<std::string>(const std::string& src)> resolve;
  // Fired after an image finished loading, so the host relayouts the document around it.
  std::function<void()> onLayoutNeeded;
};

class MarkdownView : public Flex {
public:
  // Applies to the next setMarkdown call.
  void setImageSource(MarkdownImageSource source);
  void setMarkdown(const std::string& markdown, float scale, float fontScale = 1.0F);
  void clear();
  void trackWrappableLabel(Label* label) { m_wrappableLabels.push_back(label); }

  // Parser hooks: resolveImage asks the image source, addImage appends a block image for a reference
  // resolveImage accepted.
  [[nodiscard]] std::optional<std::string> resolveImage(const std::string& src) const;
  void addImage(std::string src, const std::string& path);

  // Re-resolves images whose files were still being fetched; call when the image source got a file.
  void refreshPendingImages();

protected:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

private:
  struct ImageEntry {
    Flex* row = nullptr;
    Image* image = nullptr;
    std::string src;
    bool pending = false;
  };

  void loadImage(ImageEntry& entry, const std::string& path);
  void applyWidth(float width);

  float m_scale = 1.0F;
  MarkdownImageSource m_imageSource;
  std::vector<Label*> m_wrappableLabels;
  std::vector<ImageEntry> m_images;
};
