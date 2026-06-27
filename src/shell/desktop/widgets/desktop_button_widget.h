#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/controls/button.h"
#include "ui/palette.h"

#include <optional>
#include <string>

class DesktopButtonWidget : public DesktopWidget {
public:
  struct Options {
    std::string glyph;
    std::string label;
    std::string command;
    ButtonVariant variant = ButtonVariant::Default;
    bool showBackground = true;
    std::optional<ColorSpec> labelColor;
    ColorSpec hoverBackground = colorSpecFromRole(ColorRole::Hover);
  };

  explicit DesktopButtonWidget(Options options);

  void create() override;
  void setEditorPreview(bool enabled) noexcept override;
  void layout(Renderer& renderer) override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void syncButtonContent();
  void syncButtonStyle();
  void executeCommand() const;

  std::string m_glyph;
  std::string m_label;
  std::string m_command;
  ButtonVariant m_variant;
  std::optional<ColorSpec> m_labelColor;
  ColorSpec m_hoverBackground;

  Button* m_button = nullptr;
  bool m_showBackground = true;
  bool m_editorPreview = false;
  bool m_fillToBox = false;
};
