#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <string>

class AccountsService;
class ConfigService;
class Image;
class Label;

class DesktopWelcomeWidget : public DesktopWidget {
public:
  enum class AvatarShape : std::uint8_t {
    Circular,
    Square,
  };

  struct Options {
    bool showAvatar = true;
    bool showName = true;
    std::string greetingText;
    AvatarShape avatarShape = AvatarShape::Circular;
    bool showAvatarBorder = false;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
  };

  DesktopWelcomeWidget(AccountsService* accounts, ConfigService* config, Options options);

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;

  void refreshAvatar(Renderer& renderer);
  void syncName();
  void applyAvatarChrome();
  void applyTextColors();
  void applyShadow();

  AccountsService* m_accounts;
  ConfigService* m_config;
  bool m_showAvatar;
  bool m_showName;
  std::string m_greetingText;
  AvatarShape m_avatarShape;
  bool m_showAvatarBorder;
  ColorSpec m_color;
  bool m_shadow;
  bool m_avatarVisible = false;
  std::string m_lastName;
  std::string m_lastAvatarPath;
  Image* m_avatar = nullptr;
  Label* m_greetingLabel = nullptr;
  Label* m_nameLabel = nullptr;
};
