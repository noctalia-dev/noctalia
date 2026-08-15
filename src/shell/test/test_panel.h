#pragma once

#include "shell/panel/panel.h"

#include <memory>
#include <string>
#include <vector>

class Flex;
class Button;
class Box;
class Checkbox;
class Glyph;
class Input;
class RadioButton;
class Segmented;
class Select;
class Label;
class Slider;
class Spinner;
class Stepper;
class Toggle;
class ScrollView;

class TestPanel : public Panel {
public:
  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(1100.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(780.0F); }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  std::unique_ptr<Flex> buildTextLabSection(float scale);
  void applyTestFontFamily(const std::string& family);
  void selectTab(std::size_t index);
  Flex* m_container = nullptr;
  Label* m_headerLabel = nullptr;
  Label* m_sliderValueLabel = nullptr;
  Label* m_toggleValueLabel = nullptr;
  Label* m_checkboxValueLabel = nullptr;
  Select* m_select = nullptr;
  Button* m_glyphTextButton = nullptr;
  Button* m_glyphButton = nullptr;
  Box* m_glyphBox = nullptr;
  Glyph* m_glyph = nullptr;
  Box* m_transformStage = nullptr;
  Box* m_transformDemoBox = nullptr;
  Glyph* m_transformDemoGlyph = nullptr;
  Button* m_transformDemoButton = nullptr;
  Box* m_transformBadgeBox = nullptr;
  Label* m_transformBadgeLabel = nullptr;
  Slider* m_slider = nullptr;
  Toggle* m_toggle = nullptr;
  Checkbox* m_checkbox = nullptr;
  RadioButton* m_radioA = nullptr;
  RadioButton* m_radioB = nullptr;
  Spinner* m_spinner = nullptr;
  Stepper* m_stepper = nullptr;
  Label* m_stepperValueLabel = nullptr;
  Input* m_input = nullptr;
  Label* m_inputValueLabel = nullptr;
  Button* m_openFileDialogButton = nullptr;
  Label* m_fileDialogResultLabel = nullptr;
  Label* m_transformHelp = nullptr;
  Box* m_colorPickerResultSwatch = nullptr;
  Button* m_openColorPickerButton = nullptr;
  Button* m_openGlyphPickerButton = nullptr;
  Label* m_glyphPickerResultLabel = nullptr;
  Label* m_gridSelectionLabel = nullptr;
  Segmented* m_segmented = nullptr;
  Label* m_segmentedValueLabel = nullptr;
  Button* m_closeButton = nullptr;
  ScrollView* m_scrollView = nullptr;
  Flex* m_controlsTab = nullptr;
  Flex* m_textTab = nullptr;
  Segmented* m_tabSwitch = nullptr;
  Input* m_fontFamilyInput = nullptr;
  Label* m_fontStatusLabel = nullptr;
  Label* m_baselineModeLabel = nullptr;
  Toggle* m_baselineModeToggle = nullptr;
  std::vector<Button*> m_gridTileButtons;
};
