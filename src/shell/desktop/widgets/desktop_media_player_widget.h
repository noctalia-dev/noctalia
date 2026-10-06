#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

class Button;
class AudioVisualizer;
class Flex;
class HttpClient;
class Image;
class Label;
class MprisService;
class PipeWireSpectrum;

class DesktopMediaPlayerWidget : public DesktopWidget {
public:
  struct Options {
    bool vertical = false;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool hideWhenNoMedia = false;
    bool albumArtBackground = false;
    int albumArtBlur = 24;
    bool audioVisualizer = false;
  };

  DesktopMediaPlayerWidget(MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, Options options);
  ~DesktopMediaPlayerWidget() override;

  void create() override;
  void layout(Renderer& renderer) override;
  [[nodiscard]] bool wantsSecondTicks() const override { return true; }
  [[nodiscard]] bool needsFrameTick() const override;
  void onFrameTick(float deltaMs, Renderer& renderer) override;
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
  void layoutBoxed(Renderer& renderer, float width, float height);
  void layoutButtons(Renderer& renderer, float scale, bool vertical);
  void layoutDecorations();
  void refreshArtworkTextures(Renderer& renderer);
  void sync(Renderer& renderer);
  void syncSpectrum();
  void updateSpectrumSubscription();
  void updateVisualizerColor();
  void applyShadow();
  [[nodiscard]] bool hasActiveMedia() const;
  [[nodiscard]] bool shouldBeVisible() const;
  bool applyVisibility();
  void setVisibilityCollapsed(bool collapsed);

  MprisService* m_mpris;
  HttpClient* m_httpClient;
  PipeWireSpectrum* m_spectrum;
  bool m_vertical;
  ColorSpec m_color;
  bool m_shadow;
  bool m_hideWhenNoMedia = false;
  bool m_albumArtBackground = false;
  int m_albumArtBlur = 24;
  bool m_audioVisualizerEnabled = false;
  bool m_editorPreview = false;
  bool m_visible = true;
  bool m_visibilityInitialized = false;

  Image* m_artwork = nullptr;
  Image* m_backgroundArtwork = nullptr;
  AudioVisualizer* m_audioVisualizer = nullptr;
  Label* m_title = nullptr;
  Label* m_artist = nullptr;
  Flex* m_controls = nullptr;
  Button* m_prev = nullptr;
  Button* m_playPause = nullptr;
  Button* m_next = nullptr;

  std::string m_lastTitle;
  std::string m_lastArtist;
  std::string m_lastArtUrl;
  std::string m_artPath;
  std::string m_lastPlaybackStatus;
  bool m_lastCanGoPrevious = false;
  bool m_lastCanGoNext = false;
  std::uint64_t m_spectrumListenerId = 0;
  bool m_pendingSpectrumUpdate = false;
  float m_visualizerX = 0.0F;
  float m_visualizerY = 0.0F;
  float m_visualizerWidth = 0.0F;
  float m_visualizerHeight = 0.0F;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
