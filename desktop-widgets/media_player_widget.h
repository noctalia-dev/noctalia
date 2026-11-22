#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <functional>
#include <string>

class AudioVisualizer;
class Button;
class Flex;
class Image;
class Label;
class DesktopMediaPlayer : public Node {
public:
  struct Options {
    bool vertical = false;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool albumArtBackground = false;
    bool audioVisualizer = false;
    bool hasSpectrum = false;
    bool operator==(const Options&) const = default;
  };
  DesktopMediaPlayer();
  void setOptions(const Options& options);
  void setPlayback(const std::string& title, const std::string& artist, bool playing, bool previous, bool next);
  void setActions(std::function<void()> previous, std::function<void()> playPause, std::function<void()> next);
  void attachDecorations(Node& parent, std::function<void()> asyncReady);
  void layoutDecorations(Node& parent, float cornerRadius);
  void setFontFamily(const std::string& family);
  void setContentScale(float scale);
  float contentScale() const noexcept { return m_contentScale; }
  void setViewport(float width, float height);
  Image& artwork() noexcept { return *m_artwork; }
  Image& backgroundArtwork() noexcept { return *m_backgroundArtwork; }
  AudioVisualizer& visualizer() noexcept { return *m_audioVisualizer; }

private:
  void doLayout(Renderer& renderer) override;
  void layoutHorizontal(Renderer& renderer, float scale);
  void layoutVertical(Renderer& renderer, float scale);
  void layoutBoxed(Renderer& renderer, float width, float height);
  void layoutButtons(Renderer& renderer, float scale, bool vertical);
  void updateVisualizerColor();
  void applyShadow();
  bool m_vertical = false;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  bool m_shadow = true;
  bool m_albumArtBackground = false;
  bool m_audioVisualizerEnabled = false;
  bool m_hasSpectrum = false;
  float m_contentScale = 1.0F;
  float m_baseScale = 1.0F;
  float m_viewWidth = 0.0F;
  float m_viewHeight = 0.0F;
  Options m_options;
  Image* m_artwork = nullptr;
  Image* m_backgroundArtwork = nullptr;
  AudioVisualizer* m_audioVisualizer = nullptr;
  Label* m_title = nullptr;
  Label* m_artist = nullptr;
  Flex* m_controls = nullptr;
  Button* m_prev = nullptr;
  Button* m_playPause = nullptr;
  Button* m_next = nullptr;
  float m_visualizerX = 0.0F;
  float m_visualizerY = 0.0F;
  float m_visualizerWidth = 0.0F;
  float m_visualizerHeight = 0.0F;
};
