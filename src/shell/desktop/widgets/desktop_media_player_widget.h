#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <string>
#include <unordered_set>

class Button;
class Flex;
class HttpClient;
class Image;
class Label;
class MprisService;

class DesktopMediaPlayerWidget : public DesktopWidget {
public:
  DesktopMediaPlayerWidget(MprisService* mpris, HttpClient* httpClient, bool vertical, ColorSpec color, bool shadow);

  void create() override;
  [[nodiscard]] bool wantsSecondTicks() const override { return true; }
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void layoutHorizontal(Renderer& renderer, float scale);
  void layoutVertical(Renderer& renderer, float scale);
  void layoutButtons(Renderer& renderer, float scale);
  void sync(Renderer& renderer);
  [[nodiscard]] std::string resolveArtworkPath() const;
  void applyShadow();

  MprisService* m_mpris;
  HttpClient* m_httpClient;
  bool m_vertical;
  ColorSpec m_color;
  bool m_shadow;

  Image* m_artwork = nullptr;
  Label* m_title = nullptr;
  Label* m_artist = nullptr;
  Flex* m_controls = nullptr;
  Button* m_prev = nullptr;
  Button* m_playPause = nullptr;
  Button* m_next = nullptr;

  std::string m_lastTitle;
  std::string m_lastArtist;
  std::string m_lastArtUrl;
  std::string m_lastPlaybackStatus;
  std::unordered_set<std::string> m_pendingArtDownloads;
};
