#include "capture/color_picker_overlay.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/wayland/layer_surface.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <utility>

namespace capture {
  namespace {

    constexpr Logger kLog("color-picker");
    // Magnified pixels across the loupe, wheel-selectable. Odd, so one cell sits at the center.
    constexpr std::array<int, 6> kZoomLevels{9, 11, 15, 21, 27, 35};
    constexpr std::size_t kDefaultZoomIndex = 2;
    // Target logical width of the magnified grid; each cell rounds to whole device pixels, so the grid is
    // a few pixels off this at most.
    constexpr float kLoupeGridSize = 150.0F;
    // Band of picked color between the magnified pixels and the loupe's outer edge.
    constexpr float kLoupeRim = 5.0F;
    constexpr float kCenterOutlineWidth = 2.0F;
    // Grid lines between magnified pixels appear from this logical cell size up; smaller cells would be
    // mostly lines.
    constexpr float kGridMinCellSize = 9.5F;
    // Cells at the largest gridded zoom level, which bounds the grid-line pool.
    constexpr int kMaxGriddedCells = 15;
    constexpr const char* kHexFontFamily = "monospace";
    constexpr int kNudgeStep = 1;
    constexpr int kNudgeStepShift = 10;

    PickedColor pixelAt(const ScreencopyImage& image, int x, int y) {
      const std::size_t offset =
          ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) + static_cast<std::size_t>(x)) * 4U;
      return PickedColor{.r = image.rgba[offset], .g = image.rgba[offset + 1], .b = image.rgba[offset + 2]};
    }

    float snapToDevice(float logical, float scale) { return std::round(logical * scale) / scale; }

  } // namespace

  struct ColorPickerOverlay::Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputArea* input = nullptr;
    Image* backdrop = nullptr;
    // The loupe stands in for the hidden pointer: a round magnifier centered on the sampled pixel.
    Box* loupe = nullptr;
    // The magnified pixels: a cells² nearest-filtered texture drawn at whole device pixels per texel.
    Image* zoom = nullptr;
    TextureHandle zoomTexture;
    int zoomTextureCells = 0;
    std::vector<std::uint8_t> zoomPixels;
    bool zoomDirty = false;
    // Neutral line between the colored rim and the magnified pixels.
    Box* innerRing = nullptr;
    std::vector<Box*> gridLines;
    Box* centerOutline = nullptr;
    // Current loupe geometry, from layoutLoupe().
    int cells = 0;
    float ringWidth = kCenterOutlineWidth;
    // Readout under the loupe, filled with the picked color.
    Box* infoPill = nullptr;
    Label* hexLabel = nullptr;
    Label* rgbLabel = nullptr;
    // Usage hint, at the bottom edge unless the loupe or its readout would overlap it there.
    Flex* hintBar = nullptr;
    bool hintAtTop = false;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
  };

  ColorPickerOverlay::ColorPickerOverlay() = default;

  ColorPickerOverlay::~ColorPickerOverlay() = default;

  void ColorPickerOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void ColorPickerOverlay::setPickCallback(PickCallback callback) { m_onPick = std::move(callback); }

  void ColorPickerOverlay::setFailureCallback(FailureCallback callback) { m_onFailure = std::move(callback); }

  void ColorPickerOverlay::setKeybindLabels(KeybindLabels labels) { m_keybindLabels = std::move(labels); }

  void ColorPickerOverlay::begin(std::vector<FrozenScreenshot> screenshots, wl_output* initialOutput) {
    if (m_wayland == nullptr || m_renderContext == nullptr || screenshots.empty()) {
      return;
    }
    destroySurfaces();
    m_frozenScreenshots = std::move(screenshots);
    m_active = true;
    m_pointerOutput = nullptr;
    m_nudgeX = 0;
    m_nudgeY = 0;
    m_zoomIndex = kDefaultZoomIndex;
    m_loupeShown = false;

    m_sampleOutput = frozenImage(initialOutput) != nullptr ? initialOutput : m_frozenScreenshots.front().output;
    if (const auto* image = frozenImage(m_sampleOutput); image != nullptr) {
      m_sampleX = image->width / 2;
      m_sampleY = image->height / 2;
    }

    ensureSurfaces();
    for (auto& inst : m_instances) {
      inst->surface->requestLayout();
      inst->surface->requestRedraw();
    }
  }

  void ColorPickerOverlay::cancel() {
    m_active = false;
    destroySurfaces();
    m_frozenScreenshots.clear();
    m_sampleOutput = nullptr;
    m_pointerOutput = nullptr;
  }

  void ColorPickerOverlay::onOutputChange() {
    if (m_active && !surfacesMatchOutputs()) {
      // The frozen frames no longer describe the desktop.
      finishLater(std::nullopt);
    }
  }

  const ScreencopyImage* ColorPickerOverlay::frozenImage(wl_output* output) const {
    if (output == nullptr) {
      return nullptr;
    }
    for (const auto& entry : m_frozenScreenshots) {
      if (entry.output == output && entry.image.image.width > 0 && entry.image.image.height > 0) {
        return &entry.image.image;
      }
    }
    return nullptr;
  }

  std::optional<PickedColor> ColorPickerOverlay::currentColor() const {
    const auto* image = frozenImage(m_sampleOutput);
    if (image == nullptr) {
      return std::nullopt;
    }
    return pixelAt(*image, m_sampleX, m_sampleY);
  }

  bool ColorPickerOverlay::surfacesMatchOutputs() const {
    if (m_wayland == nullptr) {
      return m_instances.empty();
    }
    std::size_t index = 0;
    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr
          || output.logicalWidth <= 0
          || output.logicalHeight <= 0
          || frozenImage(output.output) == nullptr) {
        continue;
      }
      if (index >= m_instances.size() || m_instances[index]->output != output.output) {
        return false;
      }
      ++index;
    }
    return index == m_instances.size();
  }

  void ColorPickerOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr || !m_active) {
      return;
    }

    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr
          || output.logicalWidth <= 0
          || output.logicalHeight <= 0
          || frozenImage(output.output) == nullptr) {
        continue;
      }

      auto inst = std::make_unique<Instance>();
      inst->output = output.output;

      auto config = LayerSurfaceConfig{
          .nameSpace = "noctalia-color-picker",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = 0,
          .exclusiveZone = -1,
          .keyboard = LayerShellKeyboard::Exclusive,
          .defaultWidth = static_cast<std::uint32_t>(output.logicalWidth),
          .defaultHeight = static_cast<std::uint32_t>(output.logicalHeight),
      };

      inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
      auto* instPtr = inst.get();
      inst->surface->setRenderContext(m_renderContext);
      inst->surface->setAnimationManager(&inst->animations);
      inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
        instPtr->surface->requestLayout();
      });
      inst->surface->setPrepareFrameCallback([this, instPtr](bool /*needsUpdate*/, bool /*needsLayout*/) {
        prepareFrame(*instPtr);
      });

      if (!inst->surface->initialize(output.output)) {
        kLog.warn("failed to initialize color picker overlay on {}", output.connectorName);
        continue;
      }

      m_instances.push_back(std::move(inst));
    }
  }

  void ColorPickerOverlay::destroySurfaces() {
    for (auto& inst : m_instances) {
      if (inst->surface != nullptr && m_renderContext != nullptr) {
        Renderer& renderer = inst->surface->renderTarget().renderer();
        if (inst->backdrop != nullptr) {
          inst->backdrop->clear(renderer);
        }
        if (inst->zoomTexture.valid()) {
          renderer.textureManager().unload(inst->zoomTexture);
        }
      }
      inst->inputDispatcher.setSceneRoot(nullptr);
      inst->animations.cancelAll();
    }
    m_instances.clear();
  }

  void ColorPickerOverlay::prepareFrame(Instance& inst) {
    if (!m_active || m_renderContext == nullptr || inst.surface == nullptr) {
      return;
    }

    const auto width = inst.surface->width();
    const auto height = inst.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    // An overlay whose EGL surface cannot be made current would be an invisible fullscreen
    // surface eating input, so tear down and report instead.
    if (!m_renderContext->makeCurrent(inst.surface->renderTarget())) {
      abortWithError(i18n::tr("bar.screenshot.overlay-alloc-failed"));
      return;
    }
    Renderer& renderer = inst.surface->renderTarget().renderer();

    const bool needsSceneBuild = inst.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
    if (!needsSceneBuild) {
      uploadZoom(inst, renderer);
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);

    if (inst.backdrop != nullptr) {
      inst.backdrop->clear(renderer);
    }
    if (inst.zoomTexture.valid()) {
      renderer.textureManager().unload(inst.zoomTexture);
    }
    inst.backdrop = nullptr;
    inst.loupe = nullptr;
    inst.zoom = nullptr;
    inst.zoomTextureCells = 0;
    inst.innerRing = nullptr;
    inst.gridLines.clear();
    inst.centerOutline = nullptr;
    inst.infoPill = nullptr;
    inst.hexLabel = nullptr;
    inst.rgbLabel = nullptr;
    inst.hintBar = nullptr;

    inst.sceneRoot = ui::node({.width = w, .height = h});

    auto input = ui::inputArea({
        .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
        .focusable = true,
    });
    input->setOnEnter([this, output = inst.output](const InputArea::PointerData& data) {
      setSampleFromPointer(output, data.localX, data.localY);
    });
    input->setOnMotion([this, output = inst.output](const InputArea::PointerData& data) {
      setSampleFromPointer(output, data.localX, data.localY);
    });
    input->setOnPress([this](const InputArea::PointerData& data) {
      if (data.pressed && data.button == BTN_LEFT) {
        finishLater(currentColor());
      }
    });
    // The wheel changes how many pixels the loupe magnifies; scrolling up zooms in.
    input->setOnAxisHandler([this](const InputArea::PointerData& data) {
      if (const float steps = data.scrollSteps(); steps != 0.0F) {
        changeZoom(steps < 0.0F ? -1 : 1);
      }
      return true;
    });

    if (const auto* frozen = frozenImage(inst.output); frozen != nullptr) {
      auto backdrop = ui::image({
          .fit = ImageFit::Stretch,
          .width = w,
          .height = h,
          .configure = [](Image& image) { image.setPosition(0.0F, 0.0F); },
      });
      if (!backdrop->setSourceRaw(
              renderer, frozen->rgba.data(), frozen->rgba.size(), frozen->width, frozen->height, frozen->width * 4,
              PixmapFormat::RGBA, false
          )) {
        kLog.warn("failed to upload frozen color picker backdrop");
      }
      inst.backdrop = static_cast<Image*>(input->addChild(std::move(backdrop)));
    }

    // Usage hint. A child of the input area, under the loupe, so pointer tracking continues across it; it
    // moves between the bottom and the top edge to stay clear of the loupe (see placeHint()).
    auto hintBar = ui::row(
        {
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .paddingV = Style::spaceSm,
            .paddingH = Style::spaceMd,
            .configure = [](Flex& control) { control.setCardStyle(1.0F, 0.94F, true); },
        },
        ui::label({
            .text = i18n::tr(
                "bar.color-picker.hint", "pick", m_keybindLabels.pick, "move", m_keybindLabels.move, "cancel",
                m_keybindLabels.cancel
            ),
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    inst.hintBar = static_cast<Flex*>(input->addChild(std::move(hintBar)));
    inst.hintBar->layout(renderer);
    inst.hintAtTop = false;
    placeHint(inst);

    // Geometry comes from layoutLoupe(); the nodes here start at zero size.
    const auto outline = colorSpecFromRole(ColorRole::OutlineVariant);
    auto loupe = ui::box({
        .border = outline,
        .borderWidth = 1.0F,
        .visible = false,
    });

    auto zoom = ui::image({.fit = ImageFit::Stretch});
    inst.zoom = static_cast<Image*>(loupe->addChild(std::move(zoom)));

    inst.gridLines.reserve(static_cast<std::size_t>(kMaxGriddedCells - 1) * 2U);
    for (int i = 0; i < (kMaxGriddedCells - 1) * 2; ++i) {
      auto line = ui::box({
          // Mid gray at partial alpha reads on both dark and light pixels.
          .fill = fixedColorSpec(rgba(0.5F, 0.5F, 0.5F, 0.35F)),
          .visible = false,
          .configure = [](Box& box) { box.setNoAa(true); },
      });
      inst.gridLines.push_back(static_cast<Box*>(loupe->addChild(std::move(line))));
    }

    auto innerRing = ui::box({
        .border = outline,
        .borderWidth = 1.0F,
    });
    inst.innerRing = static_cast<Box*>(loupe->addChild(std::move(innerRing)));

    // The ring sits just outside the center cell so the sampled pixel itself stays fully visible.
    inst.centerOutline = static_cast<Box*>(loupe->addChild(ui::box({})));
    inst.loupe = static_cast<Box*>(input->addChild(std::move(loupe)));

    auto infoPill = ui::box({
        .border = outline,
        .borderWidth = 1.0F,
        .visible = false,
    });
    inst.hexLabel = static_cast<Label*>(infoPill->addChild(
        ui::label({
            .text = "#000000",
            .fontSize = Style::fontSizeBody,
            .fontWeight = FontWeight::Bold,
            .fontFamily = std::string(kHexFontFamily),
        })
    ));
    inst.rgbLabel = static_cast<Label*>(infoPill->addChild(
        ui::label({
            .text = "0 0 0",
            .fontSize = Style::fontSizeCaption,
            .fontFamily = std::string(kHexFontFamily),
        })
    ));
    inst.infoPill = static_cast<Box*>(input->addChild(std::move(infoPill)));

    layoutLoupe(inst);
    inst.zoomTexture = renderer.textureManager().loadFromPixels(
        inst.zoomPixels.data(), inst.cells, inst.cells, TextureDataFormat::Rgba, TextureFilter::Nearest
    );
    if (inst.zoomTexture.valid()) {
      inst.zoomTextureCells = inst.cells;
      inst.zoom->setExternalTexture(renderer, inst.zoomTexture);
    } else {
      kLog.warn("failed to create color picker loupe texture");
    }

    inst.input = input.get();
    inst.sceneRoot->addChild(std::move(input));

    inst.surface->setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
    // The loupe is the cursor: keep the system pointer hidden over the picker.
    inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t /*shape*/) {
      if (m_wayland != nullptr) {
        m_wayland->hideCursor(serial);
      }
    });
    inst.inputDispatcher.setFocus(inst.input);

    refreshLoupe(inst);
    uploadZoom(inst, renderer);
  }

  void ColorPickerOverlay::setSampleFromPointer(wl_output* output, float localX, float localY) {
    const auto* image = frozenImage(output);
    const auto* out = m_wayland != nullptr ? m_wayland->findOutputByWl(output) : nullptr;
    if (image == nullptr || out == nullptr || out->logicalWidth <= 0 || out->logicalHeight <= 0) {
      return;
    }
    const double scaleX = static_cast<double>(image->width) / static_cast<double>(out->logicalWidth);
    const double scaleY = static_cast<double>(image->height) / static_cast<double>(out->logicalHeight);
    const int pointerX =
        std::clamp(static_cast<int>(std::floor(static_cast<double>(localX) * scaleX)), 0, image->width - 1);
    const int pointerY =
        std::clamp(static_cast<int>(std::floor(static_cast<double>(localY) * scaleY)), 0, image->height - 1);
    if (output != m_pointerOutput) {
      // Arrow-key offsets are relative to the pointer on one output; a new output starts from the pointer.
      m_pointerOutput = output;
      m_nudgeX = 0;
      m_nudgeY = 0;
    }
    m_pointerX = pointerX;
    m_pointerY = pointerY;
    moveSample(output, pointerX + m_nudgeX, pointerY + m_nudgeY);
  }

  void ColorPickerOverlay::nudgeSample(int dx, int dy) { moveSample(m_sampleOutput, m_sampleX + dx, m_sampleY + dy); }

  void ColorPickerOverlay::moveSample(wl_output* output, int x, int y) {
    const auto* image = frozenImage(output);
    if (image == nullptr) {
      return;
    }
    x = std::clamp(x, 0, image->width - 1);
    y = std::clamp(y, 0, image->height - 1);
    if (output == m_pointerOutput) {
      // Pointer motion continues from the nudged pixel. Clamping at the image edge folds into the offset,
      // so pressing past an edge never leaves a hidden offset behind.
      m_nudgeX = x - m_pointerX;
      m_nudgeY = y - m_pointerY;
    }
    if (m_loupeShown && output == m_sampleOutput && x == m_sampleX && y == m_sampleY) {
      return;
    }
    // The loupe appears on the first pointer event or arrow key, so it never flashes at the fallback center
    // before the pointer's enter event arrives.
    m_loupeShown = true;
    m_sampleOutput = output;
    m_sampleX = x;
    m_sampleY = y;
    refreshLoupes();
  }

  void ColorPickerOverlay::refreshLoupes() {
    for (auto& inst : m_instances) {
      refreshLoupe(*inst);
    }
  }

  void ColorPickerOverlay::refreshLoupe(Instance& inst) {
    if (inst.loupe == nullptr || inst.infoPill == nullptr || inst.surface == nullptr || m_wayland == nullptr) {
      return;
    }
    const auto* image = frozenImage(inst.output);
    const auto* out = m_wayland->findOutputByWl(inst.output);
    if (!m_loupeShown || inst.output != m_sampleOutput || image == nullptr || out == nullptr) {
      if (inst.loupe->visible()) {
        inst.loupe->setVisible(false);
        inst.infoPill->setVisible(false);
        inst.surface->requestRedraw();
      }
      return;
    }

    const int cells = inst.cells;
    const int half = cells / 2;
    // Off-image texels take the panel surface color rather than the picked color filling the loupe.
    const Color offImage = colorForRole(ColorRole::Surface);
    const auto offImageChannel = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0F)); };
    for (int row = 0; row < cells; ++row) {
      for (int col = 0; col < cells; ++col) {
        const int x = m_sampleX + col - half;
        const int y = m_sampleY + row - half;
        auto* texel = inst.zoomPixels.data() + (static_cast<std::size_t>((row * cells) + col) * 4U);
        if (x < 0 || y < 0 || x >= image->width || y >= image->height) {
          texel[0] = offImageChannel(offImage.r);
          texel[1] = offImageChannel(offImage.g);
          texel[2] = offImageChannel(offImage.b);
          texel[3] = 255;
          continue;
        }
        const PickedColor color = pixelAt(*image, x, y);
        texel[0] = color.r;
        texel[1] = color.g;
        texel[2] = color.b;
        texel[3] = 255;
      }
    }
    inst.zoomDirty = true;

    // The rim and the readout pill both carry the picked color, so it reads at a glance, not just as numbers.
    const PickedColor center = pixelAt(*image, m_sampleX, m_sampleY);
    const Color centerColor = center.color();
    const Color contrast = readableTextColorForBackground(centerColor);
    Color contrastSoft = contrast;
    contrastSoft.a = 0.7F;
    inst.loupe->setFill(centerColor);
    inst.centerOutline->setBorder(contrast, inst.ringWidth);
    inst.infoPill->setFill(centerColor);
    inst.hexLabel->setColor(contrast);
    inst.rgbLabel->setColor(contrastSoft);
    inst.hexLabel->setText(formatRgbHex(centerColor));
    inst.rgbLabel->setText(std::format("{} {} {}", center.r, center.g, center.b));
    Renderer& renderer = inst.surface->renderTarget().renderer();
    inst.hexLabel->measure(renderer);
    inst.rgbLabel->measure(renderer);

    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    const float scale = std::max(1.0F, inst.surface->effectiveBufferScale());

    // Center the loupe on the sampled pixel; near an edge it runs off the surface rather than shifting.
    const auto cx = static_cast<float>(
        (static_cast<double>(m_sampleX) + 0.5)
        * static_cast<double>(out->logicalWidth)
        / static_cast<double>(image->width)
    );
    const auto cy = static_cast<float>(
        (static_cast<double>(m_sampleY) + 0.5)
        * static_cast<double>(out->logicalHeight)
        / static_cast<double>(image->height)
    );
    const float diameter = inst.loupe->width();
    const float loupeX = snapToDevice(cx - (diameter * 0.5F), scale);
    const float loupeY = snapToDevice(cy - (diameter * 0.5F), scale);
    inst.loupe->setPosition(loupeX, loupeY);
    inst.loupe->setVisible(true);

    // Readout pill under the loupe, flipped above it near the bottom edge.
    const float padH = Style::spaceMd;
    const float padV = Style::spaceXs;
    const float gap = Style::spaceSm;
    const float hexW = inst.hexLabel->width();
    const float hexH = inst.hexLabel->height();
    const float rgbW = inst.rgbLabel->width();
    const float rgbH = inst.rgbLabel->height();
    const float pillH = snapToDevice(std::max(hexH, rgbH) + (padV * 2.0F), scale);
    const float pillW = snapToDevice(padH + hexW + gap + rgbW + padH, scale);
    inst.infoPill->setSize(pillW, pillH);
    inst.infoPill->setRadius(pillH * 0.5F);
    inst.hexLabel->setPosition(padH, snapToDevice((pillH - hexH) * 0.5F, scale));
    inst.rgbLabel->setPosition(snapToDevice(padH + hexW + gap, scale), snapToDevice((pillH - rgbH) * 0.5F, scale));
    float pillY = loupeY + diameter + Style::spaceXs;
    if (pillY + pillH > surfaceH) {
      pillY = loupeY - Style::spaceXs - pillH;
    }
    const float pillX = std::clamp(cx - (pillW * 0.5F), 0.0F, std::max(0.0F, surfaceW - pillW));
    inst.infoPill->setPosition(snapToDevice(pillX, scale), snapToDevice(pillY, scale));
    inst.infoPill->setVisible(true);
    placeHint(inst);

    // A plain redraw skips prepareFrame, where the loupe texture is uploaded with this surface current.
    inst.surface->requestUpdateOnly();
    inst.surface->requestRedraw();
  }

  void ColorPickerOverlay::placeHint(Instance& inst) {
    if (inst.hintBar == nullptr || inst.surface == nullptr) {
      return;
    }
    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    const float scale = std::max(1.0F, inst.surface->effectiveBufferScale());
    const float margin = Style::spaceMd;
    const float barW = inst.hintBar->width();
    const float barH = inst.hintBar->height();
    const float barX = (surfaceW - barW) * 0.5F;
    const float bottomY = std::max(margin, surfaceH - barH - margin);
    const float topY = margin;

    if (inst.loupe != nullptr && inst.infoPill != nullptr && inst.loupe->visible()) {
      // Loupe plus readout pill, grown by the margin so the hint moves away before they touch.
      const float x0 = std::min(inst.loupe->x(), inst.infoPill->x()) - margin;
      const float y0 = std::min(inst.loupe->y(), inst.infoPill->y()) - margin;
      const float x1 =
          std::max(inst.loupe->x() + inst.loupe->width(), inst.infoPill->x() + inst.infoPill->width()) + margin;
      const float y1 =
          std::max(inst.loupe->y() + inst.loupe->height(), inst.infoPill->y() + inst.infoPill->height()) + margin;
      const auto overlaps = [&](float barY) { return x1 > barX && x0 < barX + barW && y1 > barY && y0 < barY + barH; };
      // Switch only when the current edge is hit, so the hint does not flip back and forth while the loupe moves.
      if (inst.hintAtTop ? overlaps(topY) : overlaps(bottomY)) {
        inst.hintAtTop = !inst.hintAtTop;
      }
    }

    inst.hintBar->setPosition(snapToDevice(barX, scale), snapToDevice(inst.hintAtTop ? topY : bottomY, scale));
  }

  void ColorPickerOverlay::uploadZoom(Instance& inst, Renderer& renderer) {
    if (!inst.zoomDirty || !inst.zoomTexture.valid() || inst.zoom == nullptr) {
      return;
    }
    inst.zoomDirty = false;
    if (inst.zoomTextureCells != inst.cells) {
      // A zoom change resizes the texture to the new cell count.
      if (!renderer.textureManager().replace(
              inst.zoomTexture, inst.zoomPixels.data(), inst.cells, inst.cells, TextureDataFormat::Rgba,
              TextureFilter::Nearest
          )) {
        kLog.warn("failed to resize color picker loupe texture");
        return;
      }
      inst.zoomTextureCells = inst.cells;
      inst.zoom->setExternalTexture(renderer, inst.zoomTexture);
    } else if (!renderer.textureManager().updateSubImage(
                   inst.zoomTexture, inst.zoomPixels.data(), 0, 0, inst.cells, inst.cells, TextureDataFormat::Rgba
               )) {
      kLog.warn("failed to update color picker loupe texture");
    }
    inst.zoom->markPaintDirty();
  }

  void ColorPickerOverlay::layoutLoupe(Instance& inst) {
    if (inst.loupe == nullptr || inst.surface == nullptr) {
      return;
    }
    // Every length lands on whole device pixels: at fractional scales a logical cell is not an integer number
    // of device pixels, and uneven cells read as a ragged grid.
    const float scale = std::max(1.0F, inst.surface->effectiveBufferScale());
    const int cells = kZoomLevels[m_zoomIndex];
    const float cellSize = std::max(1.0F, std::round(kLoupeGridSize * scale / static_cast<float>(cells))) / scale;
    const float gridSize = static_cast<float>(cells) * cellSize;
    const float rim = snapToDevice(kLoupeRim, scale);
    const float line = 1.0F / scale;
    const float diameter = gridSize + (rim * 2.0F);
    inst.cells = cells;
    inst.ringWidth = snapToDevice(kCenterOutlineWidth, scale);
    inst.zoomPixels.assign(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells) * 4U, 0);

    inst.loupe->setSize(diameter, diameter);
    inst.loupe->setRadius(diameter * 0.5F);

    inst.zoom->setSize(gridSize, gridSize);
    inst.zoom->setRadius(gridSize * 0.5F);
    inst.zoom->setPosition(rim, rim);

    inst.innerRing->setSize(gridSize + (line * 2.0F), gridSize + (line * 2.0F));
    inst.innerRing->setRadius((gridSize * 0.5F) + line);
    inst.innerRing->setPosition(rim - line, rim - line);

    const float centerOrigin = rim + (static_cast<float>(cells / 2) * cellSize);
    inst.centerOutline->setSize(cellSize + (inst.ringWidth * 2.0F), cellSize + (inst.ringWidth * 2.0F));
    inst.centerOutline->setPosition(centerOrigin - inst.ringWidth, centerOrigin - inst.ringWidth);

    // Grid lines between cells, cut to the circle so none pokes into the rim.
    const bool showGrid = cellSize >= kGridMinCellSize && cells <= kMaxGriddedCells;
    const float radius = gridSize * 0.5F;
    std::size_t used = 0;
    if (showGrid) {
      for (int i = 1; i < cells; ++i) {
        const float offset = static_cast<float>(i) * cellSize;
        const float fromCenter = offset - radius;
        const float halfChord = std::sqrt(std::max(0.0F, (radius * radius) - (fromCenter * fromCenter)));
        const float start = snapToDevice(rim + radius - halfChord, scale);
        const float length = snapToDevice(halfChord * 2.0F, scale);
        Box* vertical = inst.gridLines[used++];
        vertical->setPosition(rim + offset, start);
        vertical->setSize(line, length);
        vertical->setVisible(true);
        Box* horizontal = inst.gridLines[used++];
        horizontal->setPosition(start, rim + offset);
        horizontal->setSize(length, line);
        horizontal->setVisible(true);
      }
    }
    for (; used < inst.gridLines.size(); ++used) {
      inst.gridLines[used]->setVisible(false);
    }
  }

  void ColorPickerOverlay::changeZoom(int direction) {
    const auto next = static_cast<std::size_t>(
        std::clamp(static_cast<int>(m_zoomIndex) + direction, 0, static_cast<int>(kZoomLevels.size()) - 1)
    );
    if (next == m_zoomIndex) {
      return;
    }
    m_zoomIndex = next;
    for (auto& inst : m_instances) {
      layoutLoupe(*inst);
      refreshLoupe(*inst);
    }
  }

  bool ColorPickerOverlay::onPointerEvent(const PointerEvent& event) {
    if (!m_active) {
      return false;
    }

    Instance* target = nullptr;
    if (event.surface != nullptr) {
      for (auto& inst : m_instances) {
        if (inst->surface != nullptr && inst->surface->wlSurface() == event.surface) {
          target = inst.get();
          break;
        }
      }
    }
    if (target == nullptr) {
      for (auto& inst : m_instances) {
        if (inst->pointerInside) {
          target = inst.get();
          break;
        }
      }
    }
    if (target == nullptr) {
      return false;
    }

    const bool onTarget =
        event.surface != nullptr && target->surface != nullptr && event.surface == target->surface->wlSurface();

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onTarget) {
        target->pointerInside = true;
        target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      return onTarget;
    case PointerEvent::Type::Leave:
      if (onTarget || target->pointerInside) {
        target->pointerInside = false;
        target->inputDispatcher.pointerLeave();
      }
      return onTarget || target->pointerInside;
    case PointerEvent::Type::Motion:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (onTarget || target->pointerInside) {
        target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      return false;
    case PointerEvent::Type::Button:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (!onTarget && !target->pointerInside) {
        return false;
      }
      return target->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
          event.time, event.touch
      );
    case PointerEvent::Type::Axis:
      if (onTarget || target->pointerInside) {
        return target->inputDispatcher.pointerAxis(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
            event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      return false;
    }

    return false;
  }

  void ColorPickerOverlay::onKeyboardEvent(const KeyboardEvent& event) {
    if (!m_active) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      finishLater(std::nullopt);
      return;
    }
    // Space stays a pick key alongside [keybinds].validate, as in the region overlay.
    if (KeybindMatcher::matches(KeybindAction::Validate, event.sym, event.modifiers) || KeySymbol::isSpace(event.sym)) {
      finishLater(currentColor());
      return;
    }
    // Shift multiplies the step; the direction is matched against the chord without it.
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    const std::uint32_t modifiers = event.modifiers & ~KeyMod::Shift;
    const int step = shift ? kNudgeStepShift : kNudgeStep;
    if (KeybindMatcher::matches(KeybindAction::Left, event.sym, modifiers)) {
      nudgeSample(-step, 0);
    } else if (KeybindMatcher::matches(KeybindAction::Right, event.sym, modifiers)) {
      nudgeSample(step, 0);
    } else if (KeybindMatcher::matches(KeybindAction::Up, event.sym, modifiers)) {
      nudgeSample(0, -step);
    } else if (KeybindMatcher::matches(KeybindAction::Down, event.sym, modifiers)) {
      nudgeSample(0, step);
    }
  }

  void ColorPickerOverlay::finishLater(std::optional<PickedColor> result) {
    // Defer past InputDispatcher::pointerButton before destroying the surfaces it dispatches on.
    DeferredCall::callLater([this, result] {
      if (!m_active) {
        return;
      }
      cancel();
      if (m_onPick) {
        m_onPick(result);
      }
    });
  }

  void ColorPickerOverlay::abortWithError(const std::string& message) {
    if (!m_active) {
      return;
    }
    // Stop further frames from re-triggering the abort while teardown is pending.
    m_active = false;
    kLog.warn("aborting color picker overlay: {}", message);
    FailureCallback onFailure = m_onFailure;
    // Defer past the surface's prepareFrame callback before destroying its surfaces.
    DeferredCall::callLater([this, onFailure, message] {
      cancel();
      if (onFailure) {
        onFailure(message);
      }
    });
  }

} // namespace capture
