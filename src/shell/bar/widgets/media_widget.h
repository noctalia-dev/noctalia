#pragma once

#include "shell/bar/widget.h"

#include <memory>
#include <string>
#include <unordered_set>

class Image;
class InputArea;
class HttpClient;
class Glyph;
class Label;
class MprisService;
class Renderer;
struct MprisPlayerInfo;
struct wl_output;

enum class MediaTitleScrollMode : std::uint8_t {
  None,
  Always,
  OnHover,
};

class MediaWidget : public Widget {
public:
  MediaWidget(
      MprisService* mpris, HttpClient* httpClient, wl_output* output, float maxWidth, float minWidth, float artSize,
      MediaTitleScrollMode titleScrollMode, bool hideWhenNoMedia = false, bool albumArtOnly = false,
      bool hideAlbumArt = false
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void applyTitleScrollMode(bool titleVisible);
  void syncState(Renderer& renderer);
  void syncWidgetVisibility(bool hasMedia);
  [[nodiscard]] static std::string buildDisplayText(const MprisPlayerInfo& player);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  float m_maxWidth = 220.0f;
  float m_minWidth = 80.0f;
  float m_artSize = 16.0f;
  MediaTitleScrollMode m_titleScrollMode = MediaTitleScrollMode::None;
  bool m_hideWhenNoMedia = false;
  bool m_albumArtOnly = false;
  bool m_hideAlbumArt = false;
  InputArea* m_area = nullptr;
  Image* m_art = nullptr;
  Glyph* m_emptyGlyph = nullptr;
  Label* m_label = nullptr;

  std::string m_lastText;
  std::string m_lastArtUrl;
  std::string m_lastPlaybackStatus;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
