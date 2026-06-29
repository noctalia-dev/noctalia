#pragma once

#include "scripting/plugin_catalog.h"
#include "ui/controls/grid_tile.h"

#include <string>

class Flex;
class Glyph;
class Image;
class Label;
class Renderer;
class AsyncTextureCache;

namespace settings {

  class PluginStoreTile : public GridTile {
  public:
    explicit PluginStoreTile(float scale);

    void bind(
        const scripting::CatalogEntry& entry, std::string_view source, bool onDisk, bool hovered,
        const std::string& thumbnailPath, Renderer* renderer, AsyncTextureCache* textureCache
    );

  private:
    float m_scale;
    Image* m_thumbnail = nullptr;
    Flex* m_iconContainer = nullptr;
    Glyph* m_icon = nullptr;
    Label* m_nameLabel = nullptr;
    Label* m_versionLabel = nullptr;
    Flex* m_badge = nullptr;
    Label* m_badgeLabel = nullptr;
    Label* m_descLabel = nullptr;
    Label* m_authorLabel = nullptr;
    Glyph* m_addedGlyph = nullptr;

    std::string m_boundThumbnailPath;
  };

} // namespace settings
