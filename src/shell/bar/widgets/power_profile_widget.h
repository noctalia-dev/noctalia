#pragma once

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class InputArea;
class PowerProfilesService;

class PowerProfileWidget : public Widget {
public:
  struct Options {
    bool enableScroll = true;

    bool operator==(const Options&) const = default;
  };

  explicit PowerProfileWidget(PowerProfilesService* powerProfiles, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  void cycleProfile(int direction);

  PowerProfilesService* m_powerProfiles = nullptr;
  bool m_enableScroll = true;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  std::string m_lastProfile;
  bool m_available = false;
};
