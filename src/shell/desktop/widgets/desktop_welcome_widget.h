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
  struct Options {
    bool showAvater = true;
    bool showName = true;
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
  void applyTextColors();
  void applyShadow();

  AccountsService* m_accounts;
  ConfigService* m_config;
  bool m_showAvater;
  bool m_showName;
  ColorSpec m_color;
  bool m_shadow;
  bool m_avatarVisible = false;
  std::string m_lastName;
  std::string m_lastAvatarPath;
  Image* m_avatar = nullptr;
  Label* m_greetingLabel = nullptr;
  Label* m_nameLabel = nullptr;
};
