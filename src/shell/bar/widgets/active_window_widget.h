#pragma once

#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class Image;
class Label;
class Renderer;
class InputArea;
class CompositorPlatform;

enum class ActiveWindowTitleScrollMode : std::uint8_t {
  None,
  Always,
  OnHover,
};

enum class ActiveWindowDisplayMode : std::uint8_t {
  IconAndText,
  IconOnly,
  TextOnly,
};

class ConfigService;

class ActiveWindowWidget : public Widget {
public:
  struct Options {
    int minWidth = 80;
    int maxWidth = 260;
    int iconSize = static_cast<int>(Style::fontSizeBody);
    ActiveWindowTitleScrollMode titleScrollMode = ActiveWindowTitleScrollMode::None;
    ActiveWindowDisplayMode displayMode = ActiveWindowDisplayMode::IconAndText;
    bool showEmptyLabel = false;
  };

  ActiveWindowWidget(ConfigService& config, CompositorPlatform& platform, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void applyTitleScrollMode(bool titleVisible);
  void syncWidgetVisibility(bool showWidget);
  void syncState(Renderer& renderer);
  [[nodiscard]] std::string resolveIconPath(const std::string& appId);
  void buildDesktopIconIndex();

  ConfigService& m_config;
  CompositorPlatform& m_platform;
  float m_maxWidth = 260.0f;
  float m_minWidth = 80.0f;
  float m_iconSize = 16.0f;
  ActiveWindowTitleScrollMode m_titleScrollMode = ActiveWindowTitleScrollMode::None;
  ActiveWindowDisplayMode m_displayMode = ActiveWindowDisplayMode::IconAndText;
  bool m_showEmptyLabel = false;
  InputArea* m_area = nullptr;
  Image* m_icon = nullptr;
  Label* m_title = nullptr;

  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_appIcons;
  std::uint64_t m_desktopEntriesVersion = 0;

  std::string m_lastIdentifier;
  std::string m_lastTitle;
  std::string m_lastAppId;
  std::string m_lastIconPath;
  std::string m_lastTooltipTitle;
  bool m_lastEmptyState = false;
  bool m_iconColorizeRefreshPending = false;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
