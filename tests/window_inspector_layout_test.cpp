#include "compositors/compositor_platform.h"
#include "i18n/i18n.h"
#include "render/scene/image_node.h"
#include "shell/panel/panel_manager.h"
#include "shell/window_inspector/window_inspector_panel.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <cmath>
#include <print>
namespace {
  class StubRenderer final : public Renderer {
  public:
    // Fixed-advance shaping, so a wrap budget maps to an exact character count and the
    // resulting line count is predictable.
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int maxLines, TextAlign, TextEllipsize,
        bool
    ) override {
      constexpr float kAdvance = 10.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      float width = natural;
      int lineCount = text.empty() ? 0 : 1;
      if (maxWidth > 0.0F && natural > maxWidth) {
        const float perLine = std::floor(maxWidth / kAdvance) * kAdvance;
        lineCount = perLine > 0.0F ? static_cast<int>(std::ceil(natural / perLine)) : 1;
        if (maxLines > 0) {
          lineCount = std::min(lineCount, maxLines);
        }
        width = maxWidth;
      }
      return TextMetrics{
          .width = width,
          .right = width,
          .bottom = fontSize * static_cast<float>(lineCount),
          .lineCount = lineCount,
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override {
      return TextMetrics{
          .width = 18.0F,
          .left = 1.5F,
          .right = 19.5F,
          .top = -15.0F,
          .bottom = 3.0F,
          .inkTop = -15.0F,
          .inkBottom = 3.0F,
          .inkLeft = 1.5F,
          .inkRight = 19.5F,
      };
    }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  Button* findButton(Node& node, std::string_view text) {
    if (auto* button = dynamic_cast<Button*>(&node); button && button->label() && button->label()->text() == text)
      return button;
    for (const auto& child : node.children())
      if (auto* found = findButton(*child, text))
        return found;
    return nullptr;
  }

  template <typename T> T* findControl(Node& node) {
    if (auto* control = dynamic_cast<T*>(&node))
      return control;
    for (const auto& child : node.children())
      if (auto* found = findControl<T>(*child))
        return found;
    return nullptr;
  }

  Label* findLabelContaining(Node& node, std::string_view text) {
    if (auto* label = dynamic_cast<Label*>(&node); label && label->text().contains(text))
      return label;
    for (const auto& child : node.children())
      if (auto* found = findLabelContaining(*child, text))
        return found;
    return nullptr;
  }

  float buttonBottom(Node& node, std::string_view text, float top = 0.0F) {
    top += node.y();
    if (auto* button = dynamic_cast<Button*>(&node); button && button->label() && button->label()->text() == text)
      return top + node.height();
    for (const auto& child : node.children()) {
      if (const float bottom = buttonBottom(*child, text, top); bottom >= 0.0F)
        return bottom;
    }
    return -1.0F;
  }

  float buttonRight(Node& node, std::string_view text, float left = 0.0F) {
    left += node.x();
    if (auto* button = dynamic_cast<Button*>(&node); button && button->label() && button->label()->text() == text)
      return left + node.width();
    for (const auto& child : node.children()) {
      if (const float right = buttonRight(*child, text, left); right >= 0.0F)
        return right;
    }
    return -1.0F;
  }

  float checkContentFit(WindowInspectorPanel& panel, Renderer& renderer, ScrollView& viewport, float width) {
    // Match the host: lay out a bounded slot, measure intrinsic content, then
    // arrange at the measured target. Insets and wrapped rows must be included.
    panel.layout(renderer, width, 652.0F);
    const float natural = panel.measureContentHeight(renderer, width);
    panel.layout(renderer, width, natural);
    TEST_CHECK(viewport.maxScrollOffset() <= 0.5F);
    for (const auto key :
         {"window-inspector.focus", "common.minimize", "window-inspector.close", "window-inspector.floating",
          "window-inspector.pinned", "window-inspector.fullscreen", "window-inspector.maximized"}) {
      const float bottom = buttonBottom(*panel.root(), i18n::tr(key));
      TEST_CHECK(bottom >= 0.0F && bottom <= natural - viewport.viewportPaddingV() + 0.5F);
      const float right = buttonRight(*panel.root(), i18n::tr(key));
      TEST_CHECK(right <= width - viewport.viewportPaddingH() - viewport.scrollbarGutter() + 0.5F);
    }
    TEST_CHECK(std::abs(panel.measureContentHeight(renderer, width) - natural) <= 0.5F);
    return natural;
  }
} // namespace

int main() {
  i18n::Service::instance().init("en");
  StubRenderer renderer;
  PanelManager host;
  WaylandConnection wayland{WaylandConnection::ShellProtocols{}};
  CompositorPlatform platform(wayland);
  WindowInspectorPanel panel(platform);
  panel.create();
  panel.onOpen("{}");
  panel.update(renderer);
  auto* viewport = dynamic_cast<ScrollView*>(panel.root()->children()[1].get());
  TEST_CHECK(viewport);
  const float normal = checkContentFit(panel, renderer, *viewport, 852.0F);
  // The fix must measure actual viewport chrome, not merely hide the original
  // regression by changing padding to zero.
  viewport->setViewportPaddingH(7.0F);
  viewport->setViewportPaddingV(11.0F);
  const float inset = checkContentFit(panel, renderer, *viewport, 852.0F);
  TEST_CHECK(inset >= normal + 22.0F);
  viewport->setViewportPaddingH(0.0F);
  viewport->setViewportPaddingV(0.0F);
  const float narrow = checkContentFit(panel, renderer, *viewport, 360.0F);
  TEST_CHECK(narrow > normal);
  panel.layout(renderer, 360.0F, narrow / 2.0F);
  TEST_CHECK(viewport->maxScrollOffset() > 0.0F);
  checkContentFit(panel, renderer, *viewport, 852.0F);

  TEST_CHECK(!findButton(*panel.root(), i18n::tr("window-inspector.details")));
  auto* technical = findLabelContaining(*panel.root(), i18n::tr("window-inspector.identity"));
  auto* application = findLabelContaining(*panel.root(), i18n::tr("window-inspector.window-gone"));
  TEST_CHECK(technical && technical->visible() && application && technical->fontSize() < application->fontSize());
  auto* preview = findControl<Image>(*panel.root());
  TEST_CHECK(preview && preview->parent()->children().size() == 1);
  auto* image = findControl<ImageNode>(*preview);
  TEST_CHECK(image && image->radius() > 0 && image->borderWidth() == 0 && image->fitMode() == ImageFitMode::Contain);
  auto* main = dynamic_cast<Flex*>(preview->parent()->parent()->parent());
  TEST_CHECK(main && main->align() == FlexAlign::Start);

  auto* forceQuit = findButton(*panel.root(), i18n::tr("window-inspector.force-quit"));
  TEST_CHECK(forceQuit);
  // Exercise confirmation layout without a compositor target or any command.
  // The subsequent update restores capability gating; no force-quit is invoked.
  forceQuit->setEnabled(true);
  forceQuit->inputArea()->dispatchPress(1, 1, BTN_LEFT, true);
  forceQuit->inputArea()->dispatchPress(1, 1, BTN_LEFT, false);
  panel.update(renderer);
  const float confirmation = checkContentFit(panel, renderer, *viewport, 852.0F);
  TEST_CHECK(confirmation > normal);
  TEST_CHECK(panel.dismissTransientUi());
  panel.update(renderer);
  TEST_CHECK(std::abs(checkContentFit(panel, renderer, *viewport, 852.0F) - normal) <= 0.5F);
  panel.onClose();
}
