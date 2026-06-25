#pragma once

#include "shell/panel/panel.h"

#include <cstdint>
#include <string>

class ConfigService;
class Flex;
class Image;
class Label;
class Select;
class Toggle;
class WaylandConnection;
enum class PaletteSource : std::uint8_t;

class SetupWizardPanel : public Panel {
public:
  SetupWizardPanel(ConfigService* config, WaylandConnection* /*wayland*/) : m_config(config) {}

  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(620.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(580.0f); }
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }

  static bool isFirstRun(const ConfigService& config);

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void configureThemeOptionSelect();
  void commit();

  ConfigService* m_config = nullptr;
  Flex* m_root = nullptr;
  Image* m_logo = nullptr;
  Toggle* m_telemetryToggle = nullptr;
  Select* m_modeSelect = nullptr;
  Select* m_themeSourceSelect = nullptr;
  Label* m_themeOptionLabel = nullptr;
  Select* m_themeOptionSelect = nullptr;
  Label* m_wallpaperLabel = nullptr;
  PaletteSource m_paletteSource{};
  bool m_configuringThemeOptionSelect = false;
  std::string m_builtinPalette;
  std::string m_wallpaperDir;
};
