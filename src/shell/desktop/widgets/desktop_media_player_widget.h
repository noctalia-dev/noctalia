#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <memory>
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
  struct Options {
    bool vertical = false;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool hideWhenNoMedia = false;
  };

  DesktopMediaPlayerWidget(MprisService* mpris, HttpClient* httpClient, Options options);
  ~DesktopMediaPlayerWidget() override;

  void create() override;
  [[nodiscard]] bool wantsSecondTicks() const override { return true; }
  void setEditorPreview(bool enabled) noexcept override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void layoutHorizontal(Renderer& renderer, float scale);
  void layoutVertical(Renderer& renderer, float scale);
  void layoutButtons(Renderer& renderer, float scale);
  void sync(Renderer& renderer);
  void applyShadow();
  [[nodiscard]] bool hasActiveMedia() const;
  [[nodiscard]] bool shouldBeVisible() const;
  bool applyVisibility();
  void setVisibilityCollapsed(bool collapsed);

  MprisService* m_mpris;
  HttpClient* m_httpClient;
  bool m_vertical;
  ColorSpec m_color;
  bool m_shadow;
  bool m_hideWhenNoMedia = false;
  bool m_editorPreview = false;
  bool m_visible = true;
  bool m_visibilityInitialized = false;

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
  bool m_lastCanGoPrevious = false;
  bool m_lastCanGoNext = false;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
