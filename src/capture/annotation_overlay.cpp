#include "capture/annotation_overlay.h"

#include "capture/annotation_raster.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <utility>

namespace capture {
  namespace {

    constexpr Logger kLog("annotate");

    constexpr float kCropDimOpacity = 0.65F;
    constexpr float kCropFrameWidth = 2.0F;
    constexpr float kSwatchSize = 20.0F;
    constexpr float kSwatchGlyphSize = 12.0F;
    constexpr float kStrokeButtonSize = 24.0F;
    constexpr float kToolbarFillAlpha = 0.9F;
    constexpr float kImageSurroundAlpha = 0.85F;
    constexpr float kSemiFillAlpha = 0.3F;
    constexpr double kMoveHitTolerance = 8.0;
    constexpr double kFreehandMinDistance = 2.5;
    constexpr double kFreehandSimplifyTolerance = 0.75;
    constexpr double kDirtyPadding = 2.0;
    constexpr double kMinToolWidth = 1.0;
    constexpr double kMaxToolWidth = 100.0;
    constexpr double kMinTextWidth = 8.0;
    constexpr double kMaxTextWidth = 200.0;
    constexpr float kCaretWidth = 2.0F;
    constexpr float kSizeLabelWidth = 26.0F;
    constexpr float kToolbarEdgeMargin = 4.0F;

    // S / M / L multiply the tool's own default width, so a preset means the same relative
    // weight for a 6 px brush and a 32 px blur.
    constexpr std::array<double, 3> kSizePresets = {0.5, 1.0, 2.0};
    constexpr std::size_t kDefaultSizePreset = 1;
    // The stroke button's dot grows with the S / M / L preset nearest the current width.
    constexpr std::array<float, kSizePresets.size()> kStrokeDotSizes = {8.0F, 12.0F, 16.0F};

    struct Swatch {
      AnnotationColor color;
      const char* tooltipKey;
    };

    constexpr std::array<Swatch, 8> kSwatches = {{
        {{.r = 0.96F, .g = 0.20F, .b = 0.28F, .a = 1.0F}, "bar.annotate.color-red"},
        {{.r = 1.00F, .g = 0.91F, .b = 0.20F, .a = 1.0F}, "bar.annotate.color-yellow"},
        {{.r = 0.20F, .g = 0.80F, .b = 0.40F, .a = 1.0F}, "bar.annotate.color-green"},
        {{.r = 0.20F, .g = 0.55F, .b = 1.00F, .a = 1.0F}, "bar.annotate.color-blue"},
        {{.r = 1.00F, .g = 0.60F, .b = 0.10F, .a = 1.0F}, "bar.annotate.color-orange"},
        {{.r = 0.70F, .g = 0.35F, .b = 0.95F, .a = 1.0F}, "bar.annotate.color-purple"},
        {{.r = 1.00F, .g = 1.00F, .b = 1.00F, .a = 1.0F}, "bar.annotate.color-white"},
        {{.r = 0.00F, .g = 0.00F, .b = 0.00F, .a = 1.0F}, "bar.annotate.color-black"},
    }};

    struct ToolButtonSpec {
      AnnotationTool tool;
      const char* glyph;
      const char* tooltipKey;
    };

    constexpr std::array<ToolButtonSpec, 12> kToolButtons = {{
        {AnnotationTool::Move, "arrows-move", "bar.annotate.tool-move"},
        {AnnotationTool::Brush, "brush", "bar.annotate.tool-brush"},
        {AnnotationTool::Highlighter, "highlight", "bar.annotate.tool-highlighter"},
        {AnnotationTool::Line, "line", "bar.annotate.tool-line"},
        {AnnotationTool::Arrow, "arrow-up-right", "bar.annotate.tool-arrow"},
        {AnnotationTool::Rectangle, "square", "bar.annotate.tool-rectangle"},
        {AnnotationTool::Circle, "circle", "bar.annotate.tool-circle"},
        {AnnotationTool::Text, "typography", "bar.annotate.tool-text"},
        {AnnotationTool::Numbering, "number", "bar.annotate.tool-numbering"},
        {AnnotationTool::Blur, "blur", "bar.annotate.tool-blur"},
        {AnnotationTool::Eraser, "eraser", "bar.annotate.tool-eraser"},
        {AnnotationTool::Crop, "crop", "bar.annotate.tool-crop"},
    }};

    [[nodiscard]] std::size_t toolIndex(AnnotationTool tool) { return static_cast<std::size_t>(tool); }

    // Move and Crop carry no stroke; Eraser and Blur take a size but no color.
    [[nodiscard]] bool toolUsesSize(AnnotationTool tool) {
      return tool != AnnotationTool::Move && tool != AnnotationTool::Crop;
    }

    [[nodiscard]] bool toolUsesColor(AnnotationTool tool) {
      return toolUsesSize(tool) && tool != AnnotationTool::Eraser && tool != AnnotationTool::Blur;
    }

    using BackdropConfig = ScreenshotBackdropSettings;

    constexpr float kGradientChipSize = 22.0F;
    // Rows stretch to this width, so toggles, chip strip and value boxes share one right edge.
    constexpr float kBackdropPanelContentWidth = 300.0F;
    constexpr float kBackdropLabelWidth = 72.0F;
    // Value box width matches the settings slider rows.
    constexpr float kBackdropValueWidth = 50.0F;
    // The preview frame is smooth (gradient, blurred shadow), so half device resolution is plenty.
    constexpr double kBackdropPreviewDownscale = 2.0;

    struct GradientPreset {
      ColorSpec start;
      ColorSpec end;
      const char* tooltipKey;
    };

    constexpr std::size_t kGradientPresetCount = 9;

    [[nodiscard]] const std::array<GradientPreset, kGradientPresetCount>& gradientPresets() {
      static const std::array<GradientPreset, kGradientPresetCount> presets = {{
          {colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Tertiary),
           "bar.annotate.gradient-theme"},
          {colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Secondary),
           "bar.annotate.gradient-theme-accent"},
          {colorSpecFromRole(ColorRole::PrimaryContainer), colorSpecFromRole(ColorRole::TertiaryContainer),
           "bar.annotate.gradient-theme-soft"},
          {colorSpecFromRole(ColorRole::SurfaceContainer), colorSpecFromRole(ColorRole::SurfaceContainerHigh),
           "bar.annotate.gradient-theme-subtle"},
          {fixedColorSpec(rgbHex(0x232526)), fixedColorSpec(rgbHex(0x414345)), "bar.annotate.gradient-graphite"},
          {fixedColorSpec(rgbHex(0xE0E0E0)), fixedColorSpec(rgbHex(0xFAFAFA)), "bar.annotate.gradient-paper"},
          {fixedColorSpec(rgbHex(0x2F80ED)), fixedColorSpec(rgbHex(0x56CCF2)), "bar.annotate.gradient-sky"},
          {fixedColorSpec(rgbHex(0x00B09B)), fixedColorSpec(rgbHex(0x96C93D)), "bar.annotate.gradient-mint"},
          {fixedColorSpec(rgbHex(0xFF7E5F)), fixedColorSpec(rgbHex(0xFEB47B)), "bar.annotate.gradient-sunset"},
      }};
      return presets;
    }

    enum class BackdropSlider : std::uint8_t {
      Angle,
      Padding,
      Radius,
      ShadowBlur,
      ShadowOpacity,
      ShadowOffsetX,
      ShadowOffsetY
    };
    constexpr std::size_t kBackdropSliderCount = 7;

    // Values are in the units the panel shows (whole degrees, pixels, 0..1 opacity).
    struct BackdropSliderSpec {
      const char* labelKey;
      double minValue;
      double maxValue;
      double step;
      double (*get)(const BackdropConfig&);
      void (*set)(BackdropConfig&, double);
    };

    [[nodiscard]] std::int32_t toPixels(double v) { return static_cast<std::int32_t>(std::lround(v)); }
    // Whole-step sliders show integers; fractional ones (opacity) two decimals, like settings sliders.
    [[nodiscard]] std::string formatBackdropValue(const BackdropSliderSpec& spec, double v) {
      return spec.step >= 1.0 ? std::format("{}", std::lround(v)) : StringUtils::formatFixedDotDecimal(v, 2);
    }

    [[nodiscard]] const std::array<BackdropSliderSpec, kBackdropSliderCount>& backdropSliders() {
      static const std::array<BackdropSliderSpec, kBackdropSliderCount> specs = {{
          {"bar.annotate.backdrop-angle", 0.0, 360.0, 5.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.gradientAngle); },
           [](BackdropConfig& s, double v) { s.gradientAngle = static_cast<float>(v); }},
          {"bar.annotate.backdrop-padding", 0.0, 256.0, 4.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.padding); },
           [](BackdropConfig& s, double v) { s.padding = toPixels(v); }},
          {"bar.annotate.backdrop-radius", 0.0, 64.0, 1.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.cornerRadius); },
           [](BackdropConfig& s, double v) { s.cornerRadius = toPixels(v); }},
          {"bar.annotate.backdrop-shadow-blur", 0.0, 100.0, 1.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.shadowBlur); },
           [](BackdropConfig& s, double v) { s.shadowBlur = toPixels(v); }},
          {"bar.annotate.backdrop-shadow-opacity", 0.0, 1.0, 0.01,
           [](const BackdropConfig& s) { return static_cast<double>(s.shadowOpacity); },
           [](BackdropConfig& s, double v) { s.shadowOpacity = static_cast<float>(v); }},
          {"bar.annotate.backdrop-shadow-offset-x", -64.0, 64.0, 1.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.shadowOffsetX); },
           [](BackdropConfig& s, double v) { s.shadowOffsetX = toPixels(v); }},
          {"bar.annotate.backdrop-shadow-offset-y", -64.0, 64.0, 1.0,
           [](const BackdropConfig& s) { return static_cast<double>(s.shadowOffsetY); },
           [](BackdropConfig& s, double v) { s.shadowOffsetY = toPixels(v); }},
      }};
      return specs;
    }

    void paintGradientChip(Box& box, const GradientPreset& preset, bool selected) {
      const Color start = resolveColorSpec(preset.start);
      const Color end = resolveColorSpec(preset.end);
      box.setStyle(
          RoundedRectStyle{
              .fill = start,
              .border = colorForRole(selected ? ColorRole::Primary : ColorRole::OutlineVariant),
              .fillMode = FillMode::LinearGradient,
              .gradientDirection = GradientDirection::Horizontal,
              .gradientStops =
                  {GradientStop{0.0F, start}, GradientStop{0.0F, start}, GradientStop{1.0F, end},
                   GradientStop{1.0F, end}},
              .radius = Style::radiusSm,
              .borderWidth = selected ? 2.5F : Style::borderWidth,
          }
      );
    }

    [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
      for (const auto& entry : wayland.outputs()) {
        if (entry.output == output) {
          return &entry;
        }
      }
      return nullptr;
    }

    // The check mark drawn on the selected swatch needs to read against the swatch itself,
    // not against the surface, so it flips with the swatch's own luminance.
    [[nodiscard]] Color swatchContrast(const AnnotationColor& color) {
      const float luminance = (0.299F * color.r) + (0.587F * color.g) + (0.114F * color.b);
      return luminance > 0.5F ? rgba(0.0F, 0.0F, 0.0F, 1.0F) : rgba(1.0F, 1.0F, 1.0F, 1.0F);
    }

    [[nodiscard]] Button::ButtonPalette swatchPalette(const AnnotationColor& color) {
      const Color fill = rgba(color.r, color.g, color.b, 1.0F);
      Button::ButtonStateColors state{
          .bg = fixedColorSpec(fill),
          .border = colorSpecFromRole(ColorRole::OutlineVariant),
          .label = fixedColorSpec(swatchContrast(color)),
      };
      Button::ButtonPalette palette;
      palette.borderWidth = 2.0F;
      palette.normal = state;
      palette.hover = state;
      palette.hover.border = colorSpecFromRole(ColorRole::OnSurface);
      palette.pressed = state;
      palette.disabled = state;
      Button::ButtonStateColors selected = state;
      selected.border = colorSpecFromRole(ColorRole::OnSurface);
      palette.selected = selected;
      return palette;
    }

    void clearCairoRect(cairo_t* cr, double x, double y, double width, double height) {
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      cairo_rectangle(cr, x, y, width, height);
      cairo_fill(cr);
      cairo_restore(cr);
    }

    [[nodiscard]] bool isFreehandTool(AnnotationTool tool) {
      return tool == AnnotationTool::Brush || tool == AnnotationTool::Highlighter;
    }

    [[nodiscard]] AnnotationRect freehandTailBounds(const std::vector<AnnotationPoint>& points, double width) {
      if (points.empty()) {
        return AnnotationRect{};
      }

      const std::size_t first = points.size() > 3 ? points.size() - 3 : 0;
      double left = points[first].x;
      double top = points[first].y;
      double right = left;
      double bottom = top;
      for (std::size_t i = first + 1; i < points.size(); ++i) {
        left = std::min(left, points[i].x);
        top = std::min(top, points[i].y);
        right = std::max(right, points[i].x);
        bottom = std::max(bottom, points[i].y);
      }

      const double halfWidth = width / 2.0;
      return AnnotationRect{
          .x = left - halfWidth,
          .y = top - halfWidth,
          .width = (right - left) + width,
          .height = (bottom - top) + width,
      };
    }

    bool simplifyFreehandPoints(std::vector<AnnotationPoint>& points) {
      if (points.size() < 3) {
        return false;
      }

      const std::size_t originalSize = points.size();
      std::size_t candidate = 1;
      for (std::size_t next = 2; next < points.size(); ++next) {
        if (canSimplifyPoint(points[candidate - 1], points[candidate], points[next], kFreehandSimplifyTolerance)) {
          points[candidate] = points[next];
        } else {
          ++candidate;
          points[candidate] = points[next];
        }
      }
      points.resize(candidate + 1);
      return points.size() != originalSize;
    }

    // Grows a logical rect by the dirty padding and converts it to whole device pixels
    // clamped to the surface.
    [[nodiscard]] AnnotationRect
    toDeviceRect(const AnnotationRect& logical, double scale, int deviceWidth, int deviceHeight) {
      if (logical.width <= 0.0 || logical.height <= 0.0) {
        return AnnotationRect{};
      }
      const double left = std::floor((logical.x - kDirtyPadding) * scale);
      const double top = std::floor((logical.y - kDirtyPadding) * scale);
      const double right = std::ceil((logical.x + logical.width + kDirtyPadding) * scale);
      const double bottom = std::ceil((logical.y + logical.height + kDirtyPadding) * scale);
      const double clampedLeft = std::clamp(left, 0.0, static_cast<double>(deviceWidth));
      const double clampedTop = std::clamp(top, 0.0, static_cast<double>(deviceHeight));
      const double clampedRight = std::clamp(right, 0.0, static_cast<double>(deviceWidth));
      const double clampedBottom = std::clamp(bottom, 0.0, static_cast<double>(deviceHeight));
      if (clampedRight <= clampedLeft || clampedBottom <= clampedTop) {
        return AnnotationRect{};
      }
      return AnnotationRect{
          .x = clampedLeft,
          .y = clampedTop,
          .width = clampedRight - clampedLeft,
          .height = clampedBottom - clampedTop,
      };
    }

    void appendUtf8(std::string& out, std::uint32_t codepoint) {
      if (codepoint < 0x80U) {
        out.push_back(static_cast<char>(codepoint));
      } else if (codepoint < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
      } else if (codepoint < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
      } else {
        out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
      }
    }

    void popUtf8(std::string& text) {
      while (!text.empty()) {
        const auto byte = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((byte & 0xC0U) != 0x80U) {
          return;
        }
      }
    }

    const char* fillGlyph(AnnotationFillMode mode) {
      switch (mode) {
      case AnnotationFillMode::Off:
        return "bucket-off";
      case AnnotationFillMode::Semi:
        return "bucket-droplet";
      case AnnotationFillMode::Opaque:
        return "bucket";
      }
      return "bucket-off";
    }

    const char* fillTooltipKey(AnnotationFillMode mode) {
      switch (mode) {
      case AnnotationFillMode::Off:
        return "bar.annotate.fill-off";
      case AnnotationFillMode::Semi:
        return "bar.annotate.fill-semi";
      case AnnotationFillMode::Opaque:
        return "bar.annotate.fill-opaque";
      }
      return "bar.annotate.fill-off";
    }

    [[nodiscard]] AnnotationFillMode nextFillMode(AnnotationFillMode mode, bool forward) {
      if (forward) {
        switch (mode) {
        case AnnotationFillMode::Off:
          return AnnotationFillMode::Semi;
        case AnnotationFillMode::Semi:
          return AnnotationFillMode::Opaque;
        case AnnotationFillMode::Opaque:
          return AnnotationFillMode::Off;
        }
      } else {
        switch (mode) {
        case AnnotationFillMode::Off:
          return AnnotationFillMode::Opaque;
        case AnnotationFillMode::Semi:
          return AnnotationFillMode::Off;
        case AnnotationFillMode::Opaque:
          return AnnotationFillMode::Semi;
        }
      }
      return AnnotationFillMode::Off;
    }

  } // namespace

  AnnotationToolState defaultAnnotationToolState() {
    AnnotationToolState state;
    for (std::size_t i = 0; i < kAnnotationToolCount; ++i) {
      state.color[i] = kSwatches[0].color;
      state.width[i] = annotationToolDefaultWidth(static_cast<AnnotationTool>(i));
    }
    state.color[toolIndex(AnnotationTool::Highlighter)] = kSwatches[1].color;
    return state;
  }

  std::string_view annotationFillModeName(AnnotationFillMode mode) noexcept {
    switch (mode) {
    case AnnotationFillMode::Off:
      return "off";
    case AnnotationFillMode::Semi:
      return "semi";
    case AnnotationFillMode::Opaque:
      return "opaque";
    }
    return "off";
  }

  std::optional<AnnotationFillMode> annotationFillModeFromName(std::string_view name) noexcept {
    if (name == "off") {
      return AnnotationFillMode::Off;
    }
    if (name == "semi") {
      return AnnotationFillMode::Semi;
    }
    if (name == "opaque") {
      return AnnotationFillMode::Opaque;
    }
    return std::nullopt;
  }

  struct AnnotationOverlay::Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    // sceneRoot must be destroyed before `animations`: ~Node() calls cancelForOwner().
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;

    InputArea* canvas = nullptr;
    Box* surround = nullptr;
    Image* backdrop = nullptr;
    Image* committedLayer = nullptr;
    Image* activeLayer = nullptr;
    Box* dimTop = nullptr;
    Box* dimBottom = nullptr;
    Box* dimLeft = nullptr;
    Box* dimRight = nullptr;
    Box* cropFrame = nullptr;
    Flex* toolbar = nullptr;
    std::array<Button*, kAnnotationToolCount> toolButtons{};
    Button* strokeButton = nullptr;
    Button* fillButton = nullptr;
    Button* freezeButton = nullptr;
    Button* cursorButton = nullptr;
    Button* undoButton = nullptr;
    Button* redoButton = nullptr;
    Button* copyButton = nullptr;
    Button* saveButton = nullptr;
    Button* doneButton = nullptr;
    Button* dragHandle = nullptr;
    Separator* strokeSeparator = nullptr;
    Separator* exportSeparator = nullptr;
    Button* backdropButton = nullptr;

    // Stroke popover: the color swatches and the size controls of the current tool.
    Flex* strokePanel = nullptr;
    Flex* strokeSwatchRow = nullptr;
    std::array<Button*, kSwatches.size()> swatchButtons{};
    Segmented* sizePresets = nullptr;
    Button* sizeDownButton = nullptr;
    Button* sizeUpButton = nullptr;
    Label* sizeLabel = nullptr;

    // Backdrop panel and the Image-mode preview frame drawn behind the canvas.
    Flex* backdropPanel = nullptr;
    Flex* backdropBody = nullptr;
    Flex* backdropGradientRows = nullptr;
    Flex* backdropShadowRows = nullptr;
    std::array<Box*, kGradientPresetCount> gradientChips{};
    std::array<Input*, kBackdropSliderCount> backdropValueInputs{};
    Image* backdropPreview = nullptr;
    Box* imageOutline = nullptr;
    std::optional<ScreenshotBackdrop> previewBackdrop;
    AnnotationRect previewCanvas{};
    // -2 = chips not yet painted, -1 = no preset matches the current colors.
    int markedGradient = -2;

    TextureHandle committedTexture{};
    TextureHandle activeTexture{};
    cairo_surface_t* committedSurface = nullptr;
    cairo_surface_t* activeSurface = nullptr;
    cairo_surface_t* backgroundSurface = nullptr;

    std::optional<AnnotationRect> committedDirty;
    std::optional<AnnotationRect> activeDirty;
    AnnotationRect activePainted{};
    std::optional<AnnotationRect> pendingActiveBounds;
    bool pendingActiveFullRedraw = false;
    bool committedNeedsFullRedraw = true;
    std::uint64_t renderedGeneration = 0;

    AnnotationRect canvasRect{};
    double canvasScale = 1.0;
    int deviceWidth = 0;
    int deviceHeight = 0;
    float scale = 1.0F;
    const ScreencopyImage* backdropImage = nullptr;
    bool pointerInside = false;
    // Toolbar glyph state, so a mark only moves (and forces a relayout) when it really changed.
    // -2 = swatches not yet marked, -1 = no swatch matches the tool color.
    int markedSwatch = -2;
    // Stroke button face; an empty preset means it is not painted yet.
    std::optional<AnnotationColor> strokeFaceColor;
    std::optional<std::size_t> strokeFacePreset;
    AnnotationFillMode fillGlyphMode = AnnotationFillMode::Off;
    const char* cursorTooltip = nullptr;
  };

  AnnotationOverlay::AnnotationOverlay() = default;

  AnnotationOverlay::~AnnotationOverlay() { destroySurfaces(); }

  void AnnotationOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void AnnotationOverlay::setExportCallback(ExportCallback callback) { m_onExport = std::move(callback); }

  void AnnotationOverlay::setFreezeCallback(FreezeCallback callback) { m_onFreeze = std::move(callback); }

  void AnnotationOverlay::setCaptureRegionCallback(CaptureRegionCallback callback) {
    m_onCaptureRegion = std::move(callback);
  }

  void AnnotationOverlay::setClosedCallback(ClosedCallback callback) { m_onClosed = std::move(callback); }

  void AnnotationOverlay::setFailureCallback(FailureCallback callback) { m_onFailure = std::move(callback); }

  void AnnotationOverlay::setFeedbackCallback(FeedbackCallback callback) { m_onFeedback = std::move(callback); }

  void AnnotationOverlay::setStateSetter(StateSetter setter) { m_stateSetter = std::move(setter); }

  void AnnotationOverlay::setBackdropChangedCallback(BackdropChangedCallback callback) {
    m_onBackdropChanged = std::move(callback);
  }

  void AnnotationOverlay::setScreenshotBackdrop(const BackdropConfig& backdrop) {
    m_backdrop = backdrop;
    m_backdropEdited = false;
    m_backdropLayoutPadding = m_backdrop.enabled ? std::max(0, m_backdrop.padding) : 0;
  }

  void AnnotationOverlay::setToolState(const AnnotationToolState& state) {
    m_tools = state;
    for (std::size_t i = 0; i < kAnnotationToolCount; ++i) {
      const auto tool = static_cast<AnnotationTool>(i);
      const double limit = tool == AnnotationTool::Text ? kMaxTextWidth : kMaxToolWidth;
      const double floorWidth = tool == AnnotationTool::Text ? kMinTextWidth : kMinToolWidth;
      m_tools.width[i] = std::clamp(m_tools.width[i], floorWidth, limit);
    }
    m_toolbarPositions.clear();
    for (auto& inst : m_instances) {
      if (inst != nullptr) {
        positionToolbar(*inst);
      }
    }
  }

  void AnnotationOverlay::setFrozenScreenshots(std::vector<FrozenScreenshot> frozen) {
    m_frozen = std::move(frozen);
    m_mode = m_frozen.empty() ? AnnotationMode::Live : AnnotationMode::Frozen;
    if (m_mode == AnnotationMode::Live
        && (m_tools.tool == AnnotationTool::Blur || m_tools.tool == AnnotationTool::Crop)) {
      m_tools.tool = AnnotationTool::Brush;
    }
  }

  void AnnotationOverlay::setCursorVisible(bool visible) { m_cursorVisible = visible; }

  bool AnnotationOverlay::cursorVisible() const noexcept {
    if (m_mode == AnnotationMode::Image) {
      return m_image.canToggleCursor() ? m_cursorVisible : m_image.cursorVisible;
    }
    if (m_mode == AnnotationMode::Frozen && !m_instances.empty()) {
      const Instance* target = m_instances.front().get();
      for (const auto& inst : m_instances) {
        if (inst != nullptr && inst->pointerInside) {
          target = inst.get();
          break;
        }
      }
      if (target != nullptr) {
        if (const ScreenshotImage* image = screenshot(*target); image != nullptr) {
          return image->canToggleCursor() ? m_cursorVisible : image->cursorVisible;
        }
      }
    }
    return m_cursorVisible;
  }

  void AnnotationOverlay::begin() {
    if (m_wayland == nullptr || m_renderContext == nullptr) {
      return;
    }
    destroySurfaces();
    m_image = ScreenshotImage{};
    m_imageOutput = nullptr;
    m_active = true;
    ensureSurfaces();
    requestFullRefresh();
  }

  void AnnotationOverlay::beginImage(ScreenshotImage image, wl_output* target) {
    if (m_wayland == nullptr || m_renderContext == nullptr || image.image.width <= 0 || image.image.height <= 0) {
      return;
    }
    destroySurfaces();
    m_documents.clear();
    m_frozen.clear();
    m_mode = AnnotationMode::Image;
    m_image = std::move(image);
    if (!m_image.canToggleCursor()) {
      m_cursorVisible = m_image.cursorVisible;
    }
    m_imageOutput =
        target != nullptr ? target : (m_wayland->outputs().empty() ? nullptr : m_wayland->outputs().front().output);
    if (m_imageOutput == nullptr) {
      return;
    }
    m_active = true;
    ensureSurfaces();
    requestFullRefresh();
  }

  void AnnotationOverlay::hideForCapture() {
    commitPendingText();
    destroySurfaces();
  }

  void AnnotationOverlay::resumeAfterCapture() {
    if (!m_active) {
      return;
    }
    ensureSurfaces();
    requestFullRefresh();
  }

  void AnnotationOverlay::cancel() {
    const bool wasActive = m_active;
    m_active = false;
    m_gestureActive = false;
    m_gestureInstance = nullptr;
    m_moveIndex.reset();
    m_cropDragging = false;
    m_textInstance = nullptr;
    m_textIndex.reset();
    m_strokePanelOpen = false;
    destroySurfaces();
    m_documents.clear();
    m_frozen.clear();
    m_image = ScreenshotImage{};
    m_imageOutput = nullptr;
    m_mode = AnnotationMode::Live;
    if (wasActive) {
      persistToolState();
      if (m_backdropEdited && m_onBackdropChanged) {
        m_onBackdropChanged(m_backdrop);
      }
      m_backdropEdited = false;
      if (m_onClosed) {
        m_onClosed();
      }
    }
  }

  void AnnotationOverlay::onOutputChange() {
    if (!m_active) {
      return;
    }
    syncToolbarPositionsForConnectedOutputs();
    std::erase_if(m_toolbarPositions, [this](const auto& entry) {
      return findOutput(*m_wayland, entry.first) == nullptr;
    });
    if (m_mode == AnnotationMode::Image) {
      if (findOutput(*m_wayland, m_imageOutput) == nullptr) {
        DeferredCall::callLater([this] { cancel(); });
      }
      return;
    }
    if (!m_instances.empty() && !surfacesMatchOutputs()) {
      destroySurfaces();
      ensureSurfaces();
      requestRedrawAll();
    }
  }

  bool AnnotationOverlay::surfacesMatchOutputs() const {
    if (m_wayland == nullptr) {
      return m_instances.empty();
    }
    if (m_mode == AnnotationMode::Image) {
      return m_instances.size() == 1 && m_instances.front()->output == m_imageOutput;
    }
    const auto& outputs = m_wayland->outputs();
    if (m_instances.size() != outputs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (m_instances[i] == nullptr || m_instances[i]->output != outputs[i].output) {
        return false;
      }
    }
    return true;
  }

  void AnnotationOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr || !m_active) {
      return;
    }
    if (!m_instances.empty() && surfacesMatchOutputs()) {
      return;
    }
    destroySurfaces();

    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }
      if (m_mode == AnnotationMode::Image && output.output != m_imageOutput) {
        continue;
      }

      auto inst = std::make_unique<Instance>();
      inst->output = output.output;

      auto config = LayerSurfaceConfig{
          .nameSpace = "noctalia-annotate",
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
      inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
        prepareFrame(*instPtr, needsUpdate, needsLayout);
      });

      if (!inst->surface->initialize(output.output)) {
        kLog.warn("failed to initialize annotation overlay on {}", output.connectorName);
        continue;
      }

      m_documents.try_emplace(output.output);
      m_instances.push_back(std::move(inst));
    }
  }

  void AnnotationOverlay::destroySurfaces() {
    // A gesture cut short by the teardown (Freeze mid-drag, an output going away) has a
    // snapshot pushed and a half-drawn annotation appended; roll it back rather than leave the
    // document with an interaction nobody can close.
    if (m_gestureActive && m_gestureInstance != nullptr) {
      documentFor(*m_gestureInstance).discardInteraction();
    }
    if (!m_instances.empty()) {
      // A tooltip is an xdg_popup on one of these layer surfaces and must not outlive it.
      TooltipManager::instance().forceDestroy();
    }
    for (auto& inst : m_instances) {
      if (inst == nullptr) {
        continue;
      }
      if (inst->surface != nullptr && m_renderContext != nullptr) {
        Renderer& renderer = inst->surface->renderTarget().renderer();
        if (inst->backdrop != nullptr) {
          inst->backdrop->clear(renderer);
        }
        renderer.textureManager().unload(inst->committedTexture);
        renderer.textureManager().unload(inst->activeTexture);
      }
      if (inst->committedSurface != nullptr) {
        cairo_surface_destroy(inst->committedSurface);
        inst->committedSurface = nullptr;
      }
      if (inst->activeSurface != nullptr) {
        cairo_surface_destroy(inst->activeSurface);
        inst->activeSurface = nullptr;
      }
      if (inst->backgroundSurface != nullptr) {
        cairo_surface_destroy(inst->backgroundSurface);
        inst->backgroundSurface = nullptr;
      }
      inst->inputDispatcher.setSceneRoot(nullptr);
      inst->animations.cancelAll();
    }
    m_instances.clear();
    m_gestureInstance = nullptr;
    m_gestureActive = false;
    m_textInstance = nullptr;
    m_textIndex.reset();
    m_moveIndex.reset();
    m_toolbarDragInstance = nullptr;
    m_cropDragging = false;
    clearBlurCache();
  }

  const ScreenshotImage* AnnotationOverlay::screenshot(const Instance& instance) const {
    switch (m_mode) {
    case AnnotationMode::Live:
      return nullptr;
    case AnnotationMode::Image:
      return m_image.image.width > 0 ? &m_image : nullptr;
    case AnnotationMode::Frozen:
      break;
    }
    for (const auto& frozen : m_frozen) {
      if (frozen.output == instance.output) {
        return &frozen.image;
      }
    }
    return nullptr;
  }

  const ScreencopyImage* AnnotationOverlay::background(const Instance& instance) const {
    const ScreenshotImage* image = screenshot(instance);
    if (image == nullptr) {
      return nullptr;
    }
    const ScreencopyImage& chosen = image->imageForCursor(m_cursorVisible);
    return chosen.rgba.empty() ? nullptr : &chosen;
  }

  const char* AnnotationOverlay::cursorTooltipKey(const Instance& instance) const {
    if (m_mode == AnnotationMode::Live) {
      return "bar.annotate.cursor-live";
    }
    const ScreenshotImage* image = screenshot(instance);
    if (image == nullptr) {
      return "bar.annotate.cursor-not-captured";
    }
    if (image->canToggleCursor()) {
      return "bar.annotate.cursor";
    }
    switch (image->cursorStatus) {
    case CursorToggleStatus::CaptureFailed:
      return "bar.annotate.cursor-capture-failed";
    case CursorToggleStatus::Available:
    case CursorToggleStatus::NotCaptured:
      return "bar.annotate.cursor-not-captured";
    }
    return "bar.annotate.cursor-not-captured";
  }

  AnnotationDocument& AnnotationOverlay::documentFor(const Instance& instance) {
    return m_documents.try_emplace(instance.output).first->second;
  }

  AnnotationOverlay::Instance* AnnotationOverlay::instanceForSurface(const void* surface) {
    for (auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == surface) {
        return inst.get();
      }
    }
    return nullptr;
  }

  AnnotationOverlay::Instance* AnnotationOverlay::exportTarget() {
    for (auto& inst : m_instances) {
      if (inst != nullptr && inst->pointerInside) {
        return inst.get();
      }
    }
    return m_instances.empty() ? nullptr : m_instances.front().get();
  }

  // Ink lives in GL textures the prepare-frame callback fills, and that callback only runs for
  // an update request; a plain redraw would present the previous frame's textures unchanged.
  void AnnotationOverlay::requestRedrawAll() {
    for (auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr) {
        inst->surface->requestUpdateOnly();
      }
    }
  }

  void AnnotationOverlay::requestFullRefresh() {
    for (auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr) {
        inst->surface->requestLayout();
        inst->surface->requestUpdateOnly();
      }
    }
  }

  void AnnotationOverlay::markCommittedFullRedraw() {
    for (auto& inst : m_instances) {
      if (inst != nullptr) {
        inst->committedNeedsFullRedraw = true;
      }
    }
  }

  void AnnotationOverlay::abortWithError(const std::string& message) {
    if (!m_active) {
      return;
    }
    m_active = false;
    kLog.warn("aborting annotation overlay: {}", message);
    FailureCallback onFailure = m_onFailure;
    DeferredCall::callLater([this, onFailure, message] {
      m_active = true; // cancel() only fires the closed callback for an active overlay
      cancel();
      if (onFailure) {
        onFailure(message);
      }
    });
  }

  void AnnotationOverlay::layoutCanvas(Instance& inst) {
    const auto surfaceW = static_cast<double>(inst.surface->width());
    const auto surfaceH = static_cast<double>(inst.surface->height());

    if (m_mode != AnnotationMode::Image) {
      inst.canvasRect = AnnotationRect{.x = 0.0, .y = 0.0, .width = surfaceW, .height = surfaceH};
      inst.canvasScale = std::max(0.1, static_cast<double>(inst.scale));
      inst.deviceWidth = static_cast<int>(inst.surface->bufferWidthFor(inst.surface->width()));
      inst.deviceHeight = static_cast<int>(inst.surface->bufferHeightFor(inst.surface->height()));
      return;
    }

    // The capture shows at 1:1 device pixels when it fits below the toolbar, scaled down otherwise.
    // Anchored to the toolbar's default band, not its dragged position, so moving the toolbar
    // never reflows the canvas underneath it.
    const double toolbarHeight = inst.toolbar != nullptr ? static_cast<double>(inst.toolbar->height()) : 0.0;
    const double areaTop = Style::spaceMd + toolbarHeight + Style::spaceLg;
    const double availableWidth = std::max(1.0, surfaceW - (2.0 * Style::spaceLg));
    const double availableHeight = std::max(1.0, surfaceH - areaTop - Style::spaceLg);
    const auto imageWidth = static_cast<double>(m_image.image.width);
    const auto imageHeight = static_cast<double>(m_image.image.height);
    // Room for the backdrop frame, so its preview stays on screen around a large capture.
    const double frame = 2.0 * static_cast<double>(m_backdropLayoutPadding);
    const double fitScale = std::max(
        {0.1, static_cast<double>(inst.scale), (imageWidth + frame) / availableWidth,
         (imageHeight + frame) / availableHeight}
    );
    const double displayWidth = imageWidth / fitScale;
    const double displayHeight = imageHeight / fitScale;

    inst.canvasRect = AnnotationRect{
        .x = (surfaceW - displayWidth) * 0.5,
        .y = areaTop + ((availableHeight - displayHeight) * 0.5),
        .width = displayWidth,
        .height = displayHeight,
    };
    inst.canvasScale = fitScale;
    inst.deviceWidth = m_image.image.width;
    inst.deviceHeight = m_image.image.height;
  }

  void AnnotationOverlay::buildScene(Instance& inst) {
    Renderer& renderer = inst.surface->renderTarget().renderer();
    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (inst.backdrop != nullptr) {
      inst.backdrop->clear(renderer);
    }
    renderer.textureManager().unload(inst.committedTexture);
    renderer.textureManager().unload(inst.activeTexture);
    for (cairo_surface_t** surface : {&inst.committedSurface, &inst.activeSurface, &inst.backgroundSurface}) {
      if (*surface != nullptr) {
        cairo_surface_destroy(*surface);
        *surface = nullptr;
      }
    }
    clearBlurCache();

    inst.canvas = nullptr;
    inst.surround = nullptr;
    inst.backdrop = nullptr;
    inst.committedLayer = nullptr;
    inst.activeLayer = nullptr;
    inst.dimTop = inst.dimBottom = inst.dimLeft = inst.dimRight = inst.cropFrame = nullptr;
    inst.toolbar = nullptr;
    inst.toolButtons.fill(nullptr);
    inst.strokeButton = inst.fillButton = inst.freezeButton = inst.cursorButton = nullptr;
    inst.cursorTooltip = nullptr;
    inst.undoButton = inst.redoButton = inst.copyButton = inst.saveButton = inst.doneButton = inst.dragHandle = nullptr;
    inst.strokeSeparator = inst.exportSeparator = nullptr;
    inst.backdropButton = nullptr;
    inst.strokePanel = inst.strokeSwatchRow = nullptr;
    inst.swatchButtons.fill(nullptr);
    inst.sizePresets = nullptr;
    inst.sizeDownButton = inst.sizeUpButton = nullptr;
    inst.sizeLabel = nullptr;
    inst.markedSwatch = -2;
    inst.strokeFaceColor.reset();
    inst.strokeFacePreset.reset();
    if (inst.backdropPreview != nullptr) {
      inst.backdropPreview->clear(renderer);
    }
    inst.backdropPanel = inst.backdropBody = inst.backdropGradientRows = inst.backdropShadowRows = nullptr;
    inst.gradientChips.fill(nullptr);
    inst.backdropValueInputs.fill(nullptr);
    inst.backdropPreview = nullptr;
    inst.imageOutline = nullptr;
    inst.previewBackdrop.reset();
    inst.markedGradient = -2;
    inst.activePainted = AnnotationRect{};
    inst.committedDirty.reset();
    inst.activeDirty.reset();

    inst.scale = inst.surface->effectiveBufferScale();
    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    inst.sceneRoot = ui::node({.width = surfaceW, .height = surfaceH});

    // The toolbar is measured first because the Image-mode canvas centers below it.
    auto toolbar = buildToolbar(inst);
    toolbar->layout(renderer);
    auto backdropPanel = buildBackdropPanel(inst);
    backdropPanel->layout(renderer);
    auto strokePanel = buildStrokePanel(inst);
    strokePanel->layout(renderer);
    positionToolbar(inst);
    layoutCanvas(inst);

    const int deviceWidth = std::max(1, inst.deviceWidth);
    const int deviceHeight = std::max(1, inst.deviceHeight);
    inst.committedSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, deviceWidth, deviceHeight);
    inst.activeSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, deviceWidth, deviceHeight);
    inst.committedTexture = renderer.textureManager().createEmpty(
        deviceWidth, deviceHeight, TextureDataFormat::Rgba, TextureFilter::Linear
    );
    inst.activeTexture = renderer.textureManager().createEmpty(
        deviceWidth, deviceHeight, TextureDataFormat::Rgba, TextureFilter::Linear
    );
    if (cairo_surface_status(inst.committedSurface) != CAIRO_STATUS_SUCCESS
        || cairo_surface_status(inst.activeSurface) != CAIRO_STATUS_SUCCESS
        || !inst.committedTexture.valid()
        || !inst.activeTexture.valid()) {
      inst.sceneRoot.reset();
      abortWithError(i18n::tr("bar.screenshot.overlay-alloc-failed"));
      return;
    }

    const ScreencopyImage* bg = background(inst);
    if (bg != nullptr) {
      inst.backgroundSurface = frozenToCairo(*bg);
    }
    inst.backdropImage = bg;

    const auto canvasX = static_cast<float>(inst.canvasRect.x);
    const auto canvasY = static_cast<float>(inst.canvasRect.y);
    const auto canvasW = static_cast<float>(inst.canvasRect.width);
    const auto canvasH = static_cast<float>(inst.canvasRect.height);

    auto canvas = ui::inputArea({
        .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
        .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
        .focusable = true,
        .width = surfaceW,
        .height = surfaceH,
    });

    if (m_mode == AnnotationMode::Image) {
      auto surround = ui::box({
          .fill = colorSpecFromRole(ColorRole::Surface),
          .width = surfaceW,
          .height = surfaceH,
          .opacity = kImageSurroundAlpha,
          .configure = [](Box& box) { box.setPosition(0.0F, 0.0F); },
      });
      inst.surround = static_cast<Box*>(canvas->addChild(std::move(surround)));
      inst.backdropPreview = static_cast<Image*>(
          canvas->addChild(ui::image({.fit = ImageFit::Stretch, .width = 0.0F, .height = 0.0F, .visible = false}))
      );
    }

    if (bg != nullptr) {
      auto backdrop = ui::image({.fit = ImageFit::Stretch, .width = canvasW, .height = canvasH});
      if (!backdrop->setSourceRaw(
              renderer, bg->rgba.data(), bg->rgba.size(), bg->width, bg->height, bg->width * 4, PixmapFormat::RGBA,
              false
          )) {
        kLog.warn("failed to upload annotation backdrop");
      }
      backdrop->setFit(ImageFit::Stretch);
      backdrop->setSize(canvasW, canvasH);
      backdrop->setPosition(canvasX, canvasY);
      inst.backdrop = static_cast<Image*>(canvas->addChild(std::move(backdrop)));
    }

    const auto makeLayer = [&](const TextureHandle& handle) {
      auto layer = ui::image({.fit = ImageFit::Stretch, .width = canvasW, .height = canvasH});
      layer->setExternalTexture(renderer, handle);
      layer->setFit(ImageFit::Stretch);
      layer->setSize(canvasW, canvasH);
      layer->setPosition(canvasX, canvasY);
      return static_cast<Image*>(canvas->addChild(std::move(layer)));
    };
    inst.committedLayer = makeLayer(inst.committedTexture);
    inst.activeLayer = makeLayer(inst.activeTexture);

    if (m_mode == AnnotationMode::Image) {
      // Box borders paint inward without changing the canvas or exported pixels.
      inst.imageOutline = static_cast<Box*>(canvas->addChild(
          ui::box({
              .fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 0.0F)),
              .border = colorSpecFromRole(ColorRole::OutlineVariant),
              .borderWidth = Style::borderWidth,
              .radius = 0.0F,
              .width = canvasW,
              .height = canvasH,
              .configure = [canvasX, canvasY](Box& box) { box.setPosition(canvasX, canvasY); },
          })
      ));
    }

    const auto makeDimStrip = [&] {
      auto strip = ui::box({
          // Fixed black scrim so the cropped-away area darkens under every theme.
          .fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 1.0F)),
          .width = 0.0F,
          .height = 0.0F,
          .opacity = kCropDimOpacity,
          .visible = false,
          .configure = [](Box& box) { box.setPosition(0.0F, 0.0F); },
      });
      return static_cast<Box*>(canvas->addChild(std::move(strip)));
    };
    inst.dimTop = makeDimStrip();
    inst.dimBottom = makeDimStrip();
    inst.dimLeft = makeDimStrip();
    inst.dimRight = makeDimStrip();

    Color frameColor = colorForRole(ColorRole::Primary);
    frameColor.a = 1.0F;
    auto cropFrame = ui::box({
        .visible = false,
        .configure = [frameColor](Box& box) { box.setBorder(fixedColorSpec(frameColor), kCropFrameWidth); },
    });
    inst.cropFrame = static_cast<Box*>(canvas->addChild(std::move(cropFrame)));

    auto* instPtr = &inst;
    canvas->setOnPress([this, instPtr](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      onCanvasPress(*instPtr, static_cast<double>(data.localX), static_cast<double>(data.localY), data.pressed);
    });
    canvas->setOnMotion([this, instPtr](const InputArea::PointerData& data) {
      onCanvasMotion(*instPtr, static_cast<double>(data.localX), static_cast<double>(data.localY));
    });

    inst.canvas = canvas.get();
    inst.sceneRoot->addChild(std::move(canvas));
    inst.sceneRoot->addChild(std::move(toolbar));
    inst.sceneRoot->addChild(std::move(backdropPanel));
    inst.sceneRoot->addChild(std::move(strokePanel));

    inst.surface->setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      if (m_wayland != nullptr) {
        m_wayland->setCursorShape(serial, shape);
      }
    });
    // Toolbar tooltips are xdg_popups parented to this layer surface, and they must die
    // before it does; destroySurfaces() force-destroys them.
    inst.inputDispatcher.setHoverChangeCallback([this, instPtr](InputArea* /*old*/, InputArea* next) {
      if (instPtr->surface != nullptr) {
        if (next != nullptr && instPtr->toolbar != nullptr) {
          // Open toward the roomier half of the output so the popup stays clear of the movable toolbar.
          const float toolbarCenterY = instPtr->toolbar->y() + (instPtr->toolbar->height() * 0.5F);
          const float surfaceCenterY = static_cast<float>(instPtr->surface->height()) * 0.5F;
          next->setTooltipPlacement(
              toolbarCenterY <= surfaceCenterY ? TooltipPlacement::Below : TooltipPlacement::Above
          );
        }
        TooltipManager::instance().onHoverChange(
            m_toolbarDragInstance != nullptr ? nullptr : next, instPtr->surface->layerSurface(), instPtr->output
        );
      }
    });
    inst.inputDispatcher.setFocus(inst.canvas);

    inst.committedNeedsFullRedraw = true;
    // createEmpty leaves texture contents undefined, and drivers hand back recycled memory, so
    // an unwritten layer shows whatever the last session drew. The committed layer is covered
    // by its full redraw below; the active layer needs its cleared surface pushed once.
    inst.activeDirty = AnnotationRect{
        .x = 0.0,
        .y = 0.0,
        .width = static_cast<double>(inst.deviceWidth),
        .height = static_cast<double>(inst.deviceHeight),
    };
    updateCropVisuals(inst);
    if (m_gestureActive && m_gestureInstance == &inst) {
      redrawActive(inst);
    }
  }

  void AnnotationOverlay::prepareFrame(Instance& inst, bool /*needsUpdate*/, bool /*needsLayout*/) {
    if (!m_active || m_renderContext == nullptr || inst.surface == nullptr) {
      return;
    }
    const auto width = inst.surface->width();
    const auto height = inst.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    // A surface whose EGL context cannot be made current would paint nothing while still
    // eating input, so tear down and report instead of spinning.
    if (!m_renderContext->makeCurrent(inst.surface->renderTarget())) {
      abortWithError(i18n::tr("bar.screenshot.overlay-alloc-failed"));
      return;
    }
    Renderer& renderer = inst.surface->renderTarget().renderer();

    const float scale = inst.surface->effectiveBufferScale();
    const bool needsBuild = inst.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height
        || std::abs(inst.scale - scale) > 0.001F;
    if (needsBuild) {
      buildScene(inst);
      if (inst.sceneRoot == nullptr) {
        return;
      }
    }

    AnnotationDocument& doc = documentFor(inst);

    // Toggling the cursor swaps the whole backdrop, and Blur samples it, so both re-upload.
    const ScreencopyImage* bg = background(inst);
    if (bg != inst.backdropImage) {
      if (bg != nullptr && inst.backdrop != nullptr) {
        inst.backdrop->clear(renderer);
        if (!inst.backdrop->setSourceRaw(
                renderer, bg->rgba.data(), bg->rgba.size(), bg->width, bg->height, bg->width * 4, PixmapFormat::RGBA,
                false
            )) {
          kLog.warn("failed to re-upload annotation backdrop");
        }
        inst.backdrop->setFit(ImageFit::Stretch);
        inst.backdrop->setSize(static_cast<float>(inst.canvasRect.width), static_cast<float>(inst.canvasRect.height));
        inst.backdrop->setPosition(static_cast<float>(inst.canvasRect.x), static_cast<float>(inst.canvasRect.y));
        if (inst.backgroundSurface != nullptr) {
          cairo_surface_destroy(inst.backgroundSurface);
        }
        inst.backgroundSurface = frozenToCairo(*bg);
        clearBlurCache();
        inst.committedNeedsFullRedraw = true;
        inst.pendingActiveFullRedraw = true;
      }
      inst.backdropImage = bg;
    }

    const bool gestureHere = m_gestureActive && m_gestureInstance == &inst;
    if (inst.committedNeedsFullRedraw || (!gestureHere && inst.renderedGeneration != doc.generation())) {
      redrawCommitted(inst);
    }
    if (inst.pendingActiveFullRedraw) {
      redrawActive(inst);
    } else if (inst.pendingActiveBounds.has_value()) {
      redrawActive(inst, inst.pendingActiveBounds);
    }
    uploadDirty(inst, renderer);
    updateBackdropPreview(inst, renderer);
    refreshToolbar(inst, &renderer);
  }

  std::unique_ptr<Flex> AnnotationOverlay::buildToolbar(Instance& inst) {
    Color toolbarFill = colorForRole(ColorRole::Surface);
    toolbarFill.a = kToolbarFillAlpha;

    auto toolbar = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm,
        .paddingV = Style::spaceSm,
        .paddingH = Style::spaceMd,
        .fill = fixedColorSpec(toolbarFill),
        .radius = Style::radiusXl,
        .border = colorSpecFromRole(ColorRole::OutlineVariant),
        .borderWidth = Style::borderWidth,
    });

    const auto addSeparator = [&] {
      return static_cast<Separator*>(toolbar->addChild(
          ui::separator({
              .orientation = SeparatorOrientation::VerticalRule,
              .height = Style::controlHeightSm * 0.6F,
          })
      ));
    };

    const auto addGhostButton = [&](const char* glyph, const char* tooltipKey, std::function<void()> onClick) {
      return static_cast<Button*>(toolbar->addChild(
          ui::button({
              .glyph = std::string(glyph),
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr(tooltipKey),
              .onClick = std::move(onClick),
          })
      ));
    };

    // Grab handle. The press starts the drag; onPointerEvent steers it from the surface-local
    // pointer stream, which the pointer's implicit grab keeps pointed at this overlay.
    auto* instPtr = &inst;
    inst.dragHandle = addGhostButton("grip-vertical", "bar.annotate.move-toolbar", nullptr);
    inst.dragHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL);
    inst.dragHandle->setOnPress([this, instPtr](float /*localX*/, float /*localY*/, bool pressed) {
      if (pressed) {
        beginToolbarDrag(*instPtr);
      } else if (m_toolbarDragInstance == instPtr) {
        m_toolbarDragInstance = nullptr;
      }
    });
    if (InputArea* dragArea = inst.dragHandle->inputArea(); dragArea != nullptr) {
      dragArea->setOnCancel([this, instPtr] {
        if (m_toolbarDragInstance == instPtr) {
          m_toolbarDragInstance = nullptr;
        }
      });
    }

    // Editor controls lead the bar, keeping Close well away from Done at the trailing end.
    if (Style::closeButtonsEnabled()) {
      addGhostButton("x", "bar.annotate.close", [this] { requestClose(); });
    }
    addSeparator();

    inst.undoButton = addGhostButton("arrow-back-up", "bar.annotate.undo", [this] { undo(); });
    inst.redoButton = addGhostButton("arrow-forward-up", "bar.annotate.redo", [this] { redo(); });
    addSeparator();

    for (const auto& spec : kToolButtons) {
      inst.toolButtons[toolIndex(spec.tool)] = static_cast<Button*>(toolbar->addChild(
          ui::button({
              .glyph = std::string(spec.glyph),
              .selected = m_tools.tool == spec.tool,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr(spec.tooltipKey),
              .onClick = [this, tool = spec.tool] { selectTool(tool); },
          })
      ));
    }

    // Separators sit before each group; refreshToolbar hides the ones whose group is empty.
    inst.strokeSeparator = addSeparator();
    inst.strokeButton = static_cast<Button*>(toolbar->addChild(
        ui::button({
            .glyph = std::string("circle-filled"),
            .glyphSize = kStrokeDotSizes[kDefaultSizePreset],
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("bar.annotate.stroke"),
            .minWidth = kStrokeButtonSize,
            .minHeight = kStrokeButtonSize,
            .maxWidth = kStrokeButtonSize,
            .maxHeight = kStrokeButtonSize,
            .padding = 0.0F,
            .radius = Style::radiusSm,
            .width = kStrokeButtonSize,
            .height = kStrokeButtonSize,
            .onClick = [this] { toggleStrokePanel(); },
        })
    ));
    inst.fillButton = addGhostButton("bucket-off", "bar.annotate.fill-off", [this] { cycleFill(); });
    inst.fillButton->setOnRightClick([this] { cycleFill(false); });

    addSeparator();
    inst.cursorButton = addGhostButton("pointer", "bar.annotate.cursor", [this, instPtr] { toggleCursor(instPtr); });
    if (const ScreenshotImage* image = screenshot(inst);
        image != nullptr && !image->canToggleCursor() && image->cursorVisible) {
      // Keep the captured-on state visible even when this capture cannot be toggled.
      auto palette = Button::defaultPalette(ButtonVariant::Ghost);
      if (palette.selected.has_value()) {
        palette.disabled.bg = palette.selected->bg;
        palette.disabled.border = palette.selected->border;
      }
      inst.cursorButton->setCustomPalette(palette);
    }
    inst.freezeButton = addGhostButton("snowflake", "bar.annotate.freeze", [this] { requestFreeze(); });
    addGhostButton("screenshot", "bar.annotate.capture-new-region", [this] { requestCaptureRegion(); });

    inst.exportSeparator = addSeparator();
    inst.backdropButton = addGhostButton("background", "bar.annotate.backdrop", [this] { toggleBackdropPanel(); });
    inst.copyButton = addGhostButton("copy", "bar.annotate.copy", [this] { requestExport(AnnotationExport::Copy); });
    inst.saveButton =
        addGhostButton("device-floppy", "bar.annotate.save", [this] { requestExport(AnnotationExport::Save); });
    inst.doneButton = static_cast<Button*>(toolbar->addChild(
        ui::button({
            .glyph = std::string("check"),
            .variant = ButtonVariant::Primary,
            .tooltip = i18n::tr("bar.annotate.done"),
            .onClick = [this] { requestExport(AnnotationExport::Done); },
        })
    ));

    inst.toolbar = toolbar.get();
    refreshToolbar(inst, nullptr);
    return toolbar;
  }

  void AnnotationOverlay::refreshToolbar(Instance& inst, Renderer* renderer) {
    if (inst.toolbar == nullptr) {
      return;
    }
    const bool frozenOrImage = m_mode != AnnotationMode::Live;
    const bool imageMode = m_mode == AnnotationMode::Image;
    const bool shapeTool = m_tools.tool == AnnotationTool::Rectangle || m_tools.tool == AnnotationTool::Circle;

    // A toolbar item that appears or disappears changes the row's extent, and an update-only
    // frame runs no layout pass of its own, so track the flip and re-lay the row below.
    bool needsLayout = false;
    const auto show = [&needsLayout](Node* node, bool visible) {
      if (node != nullptr && node->visible() != visible) {
        node->setVisible(visible);
        node->setParticipatesInLayout(visible);
        needsLayout = true;
      }
    };

    for (const auto& spec : kToolButtons) {
      Button* button = inst.toolButtons[toolIndex(spec.tool)];
      if (button == nullptr) {
        continue;
      }
      const bool available = frozenOrImage || (spec.tool != AnnotationTool::Blur && spec.tool != AnnotationTool::Crop);
      show(button, available);
      button->setSelected(m_tools.tool == spec.tool);
    }

    show(inst.freezeButton, m_mode == AnnotationMode::Live);
    if (inst.cursorButton != nullptr) {
      const ScreenshotImage* image = screenshot(inst);
      const bool available = image != nullptr && image->canToggleCursor();
      inst.cursorButton->setSelected(image != nullptr && (available ? m_cursorVisible : image->cursorVisible));
      inst.cursorButton->setEnabled(available);
      const char* tooltip = cursorTooltipKey(inst);
      if (inst.cursorTooltip != tooltip) {
        inst.cursorButton->setTooltip(i18n::tr(tooltip));
        inst.cursorTooltip = tooltip;
      }
      // Keep disabled controls hoverable. Button guards activation; consume the press
      // rather than letting it fall through to the annotation canvas underneath.
      if (InputArea* area = inst.cursorButton->inputArea(); area != nullptr) {
        area->setEnabled(true);
        area->setFocusable(available);
        area->setCursorShape(
            available ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
        );
      }
    }

    // Move and Crop carry no stroke, so the stroke group (and its separator) has nothing to act on.
    const bool sizedTool = toolUsesSize(m_tools.tool);
    show(inst.strokeSeparator, sizedTool);
    show(inst.strokeButton, sizedTool);
    if (inst.strokeButton != nullptr && sizedTool) {
      inst.strokeButton->setSelected(m_strokePanelOpen);
      // The face is the tool color, neutral for tools that ignore color, with a dot sized by
      // the nearest S / M / L preset.
      std::optional<AnnotationColor> faceColor;
      if (toolUsesColor(m_tools.tool)) {
        faceColor = m_tools.color[toolIndex(m_tools.tool)];
      }
      const std::size_t facePreset = nearestSizePreset(m_tools.tool);
      if (faceColor != inst.strokeFaceColor || inst.strokeFacePreset != facePreset) {
        inst.strokeButton->setCustomPalette(
            faceColor.has_value() ? swatchPalette(*faceColor) : Button::defaultPalette(ButtonVariant::Ghost)
        );
        inst.strokeButton->setGlyphSize(kStrokeDotSizes[facePreset]);
        inst.strokeFaceColor = faceColor;
        inst.strokeFacePreset = facePreset;
        needsLayout = true;
      }
    }
    show(inst.fillButton, shapeTool);
    if (inst.fillButton != nullptr) {
      inst.fillButton->setSelected(m_tools.fill != AnnotationFillMode::Off);
      // The glyph carries the state as well as the highlight: a struck-through bucket reads
      // as off, a droplet bucket as semi, and a full bucket as opaque.
      if (inst.fillGlyphMode != m_tools.fill) {
        inst.fillButton->setGlyph(fillGlyph(m_tools.fill));
        inst.fillButton->setTooltip(i18n::tr(fillTooltipKey(m_tools.fill)));
        inst.fillGlyphMode = m_tools.fill;
        needsLayout = true;
      }
    }

    show(inst.backdropButton, imageMode);
    if (inst.backdropButton != nullptr) {
      inst.backdropButton->setSelected(m_backdropPanelOpen);
    }

    show(inst.exportSeparator, frozenOrImage);
    show(inst.copyButton, frozenOrImage);
    show(inst.saveButton, frozenOrImage);
    show(inst.doneButton, imageMode);

    const AnnotationDocument& doc = documentFor(inst);
    if (inst.undoButton != nullptr) {
      inst.undoButton->setEnabled(doc.canUndo());
    }
    if (inst.redoButton != nullptr) {
      inst.redoButton->setEnabled(doc.canRedo());
    }

    if (needsLayout && renderer != nullptr) {
      inst.toolbar->layout(*renderer);
    }
    refreshBackdropPanel(inst, renderer);
    refreshStrokePanel(inst, renderer);
    positionToolbar(inst);
  }

  std::size_t AnnotationOverlay::nearestSizePreset(AnnotationTool tool) const {
    const double base = annotationToolDefaultWidth(tool);
    const double width = m_tools.width[toolIndex(tool)];
    std::size_t best = kDefaultSizePreset;
    double bestDistance = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < kSizePresets.size(); ++i) {
      const double distance = std::abs(width - (base * kSizePresets[i]));
      if (distance < bestDistance) {
        bestDistance = distance;
        best = i;
      }
    }
    return best;
  }

  void AnnotationOverlay::applySizePreset(std::size_t index) {
    if (m_syncingSizePreset || index >= kSizePresets.size()) {
      return;
    }
    const AnnotationTool tool = m_tools.tool;
    const bool text = tool == AnnotationTool::Text;
    m_tools.width[toolIndex(tool)] = std::clamp(
        annotationToolDefaultWidth(tool) * kSizePresets[index], text ? kMinTextWidth : kMinToolWidth,
        text ? kMaxTextWidth : kMaxToolWidth
    );
    restyleActiveText();
    requestRedrawAll();
  }

  // Default placement is centered under the top edge. Each output keeps its own dragged spot,
  // clamped so it stays reachable when that output resizes.
  void AnnotationOverlay::positionToolbar(Instance& inst) {
    if (inst.toolbar == nullptr || inst.surface == nullptr) {
      return;
    }
    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    const float width = inst.toolbar->width();
    const float height = inst.toolbar->height();
    const float maxX = std::max(kToolbarEdgeMargin, surfaceW - width - kToolbarEdgeMargin);
    const float maxY = std::max(kToolbarEdgeMargin, surfaceH - height - kToolbarEdgeMargin);

    const WaylandOutput* output = m_wayland != nullptr ? findOutput(*m_wayland, inst.output) : nullptr;
    auto positionIt = m_toolbarPositions.find(inst.output);
    if (positionIt == m_toolbarPositions.end() && output != nullptr && !output->connectorName.empty()) {
      const auto savedIt = m_tools.toolbarPositions.find(output->connectorName);
      if (savedIt != m_tools.toolbarPositions.end()) {
        positionIt = m_toolbarPositions.emplace(inst.output, savedIt->second).first;
      }
    }

    float x = std::max(Style::spaceMd, (surfaceW - width) * 0.5F);
    float y = Style::spaceMd;
    if (positionIt != m_toolbarPositions.end()) {
      x = static_cast<float>(positionIt->second.x);
      y = static_cast<float>(positionIt->second.y);
    }
    x = std::clamp(x, kToolbarEdgeMargin, maxX);
    y = std::clamp(y, kToolbarEdgeMargin, maxY);
    inst.toolbar->setPosition(x, y);

    if (positionIt != m_toolbarPositions.end()) {
      positionIt->second = AnnotationPoint{.x = static_cast<double>(x), .y = static_cast<double>(y)};
      if (output != nullptr && !output->connectorName.empty()) {
        m_tools.toolbarPositions.insert_or_assign(output->connectorName, positionIt->second);
      }
    }
    positionBackdropPanel(inst);
    positionStrokePanel(inst);
  }

  void AnnotationOverlay::syncToolbarPositionsForConnectedOutputs() {
    if (m_wayland == nullptr) {
      return;
    }
    for (const auto& [outputHandle, position] : m_toolbarPositions) {
      const WaylandOutput* output = findOutput(*m_wayland, outputHandle);
      if (output != nullptr && !output->connectorName.empty()) {
        m_tools.toolbarPositions.insert_or_assign(output->connectorName, position);
      }
    }
  }

  // The grab is anchored in surface coordinates rather than as a delta inside the handle,
  // because a toolbar relayout moves the handle out from under the pointer mid-drag.
  void AnnotationOverlay::beginToolbarDrag(Instance& inst) {
    if (inst.toolbar == nullptr) {
      return;
    }
    m_toolbarDragInstance = &inst;
    TooltipManager::instance().forceDestroy();
    m_toolbarGrabX = static_cast<float>(m_pointerX) - inst.toolbar->x();
    m_toolbarGrabY = static_cast<float>(m_pointerY) - inst.toolbar->y();
  }

  void AnnotationOverlay::dragToolbarTo(double surfaceX, double surfaceY) {
    if (m_toolbarDragInstance == nullptr) {
      return;
    }
    Instance& inst = *m_toolbarDragInstance;
    m_toolbarPositions.insert_or_assign(
        inst.output,
        AnnotationPoint{
            .x = surfaceX - static_cast<double>(m_toolbarGrabX),
            .y = surfaceY - static_cast<double>(m_toolbarGrabY),
        }
    );
    positionToolbar(inst);
    if (inst.surface != nullptr) {
      inst.surface->requestRedraw();
    }
  }

  void AnnotationOverlay::markCommittedDirty(Instance& inst, const AnnotationRect& logicalBounds) {
    const AnnotationRect device = toDeviceRect(logicalBounds, inst.canvasScale, inst.deviceWidth, inst.deviceHeight);
    if (device.width <= 0.0) {
      return;
    }
    inst.committedDirty = inst.committedDirty.has_value() ? unionRect(*inst.committedDirty, device) : device;
  }

  void AnnotationOverlay::markActiveDirty(Instance& inst, const AnnotationRect& logicalBounds) {
    const AnnotationRect device = toDeviceRect(logicalBounds, inst.canvasScale, inst.deviceWidth, inst.deviceHeight);
    if (device.width <= 0.0) {
      return;
    }
    inst.activeDirty = inst.activeDirty.has_value() ? unionRect(*inst.activeDirty, device) : device;
  }

  void AnnotationOverlay::redrawCommitted(Instance& inst) {
    if (inst.committedSurface == nullptr) {
      return;
    }
    AnnotationDocument& doc = documentFor(inst);
    const std::optional<std::size_t> skipIndex =
        (m_gestureInstance == &inst) ? m_moveIndex : std::optional<std::size_t>{};

    cairo_t* cr = cairo_create(inst.committedSurface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, inst.canvasScale, inst.canvasScale);
    renderAnnotations(cr, doc.annotations(), inst.backgroundSurface, inst.canvasScale, skipIndex);
    cairo_destroy(cr);

    inst.committedDirty = AnnotationRect{
        .x = 0.0,
        .y = 0.0,
        .width = static_cast<double>(inst.deviceWidth),
        .height = static_cast<double>(inst.deviceHeight),
    };
    inst.committedNeedsFullRedraw = false;
    inst.renderedGeneration = doc.generation();
  }

  void AnnotationOverlay::foldIntoCommitted(Instance& inst, const Annotation& annotation) {
    if (inst.committedSurface == nullptr) {
      return;
    }
    const AnnotationRect bounds = annotationBounds(annotation);
    cairo_t* cr = cairo_create(inst.committedSurface);
    cairo_scale(cr, inst.canvasScale, inst.canvasScale);
    cairo_rectangle(
        cr, bounds.x - kDirtyPadding, bounds.y - kDirtyPadding, bounds.width + (2.0 * kDirtyPadding),
        bounds.height + (2.0 * kDirtyPadding)
    );
    cairo_clip(cr);
    renderAnnotation(cr, annotation, inst.backgroundSurface, inst.canvasScale);
    cairo_destroy(cr);
    markCommittedDirty(inst, bounds);
  }

  void AnnotationOverlay::queueActiveRedraw(Instance& inst, std::optional<AnnotationRect> changedBounds) {
    if (!changedBounds.has_value()) {
      inst.pendingActiveFullRedraw = true;
      inst.pendingActiveBounds.reset();
      return;
    }
    if (!inst.pendingActiveFullRedraw) {
      inst.pendingActiveBounds =
          inst.pendingActiveBounds.has_value() ? unionRect(*inst.pendingActiveBounds, *changedBounds) : changedBounds;
    }
  }

  void AnnotationOverlay::redrawActive(Instance& inst, std::optional<AnnotationRect> changedBounds) {
    if (inst.activeSurface == nullptr) {
      return;
    }
    inst.pendingActiveFullRedraw = false;
    inst.pendingActiveBounds.reset();

    cairo_t* cr = cairo_create(inst.activeSurface);
    std::optional<AnnotationRect> changedDevice;
    if (changedBounds.has_value()) {
      changedDevice = toDeviceRect(*changedBounds, inst.canvasScale, inst.deviceWidth, inst.deviceHeight);
      if (changedDevice->width <= 0.0 || changedDevice->height <= 0.0) {
        cairo_destroy(cr);
        return;
      }
      clearCairoRect(cr, changedDevice->x, changedDevice->y, changedDevice->width, changedDevice->height);
      cairo_rectangle(cr, changedDevice->x, changedDevice->y, changedDevice->width, changedDevice->height);
      cairo_clip(cr);
    } else if (inst.activePainted.width > 0.0 && inst.activePainted.height > 0.0) {
      clearCairoRect(
          cr, inst.activePainted.x, inst.activePainted.y, inst.activePainted.width, inst.activePainted.height
      );
      markActiveDirty(
          inst,
          AnnotationRect{
              .x = inst.activePainted.x / inst.canvasScale,
              .y = inst.activePainted.y / inst.canvasScale,
              .width = inst.activePainted.width / inst.canvasScale,
              .height = inst.activePainted.height / inst.canvasScale,
          }
      );
    }

    AnnotationRect bounds{};
    if (m_gestureActive && m_gestureInstance == &inst) {
      AnnotationDocument& doc = documentFor(inst);
      const Annotation* target = nullptr;
      if (m_moveIndex.has_value() && *m_moveIndex < doc.annotations().size()) {
        target = &doc.annotations()[*m_moveIndex];
      } else {
        target = doc.active();
      }
      if (target != nullptr) {
        const bool editingText = m_textIndex.has_value() && m_textInstance == &inst;
        bounds = annotationBounds(*target);
        cairo_save(cr);
        cairo_scale(cr, inst.canvasScale, inst.canvasScale);
        renderAnnotation(cr, *target, inst.backgroundSurface, inst.canvasScale);
        if (editingText) {
          const AnnotationRect text = measureText(*target);
          cairo_set_source_rgba(cr, target->stroke.r, target->stroke.g, target->stroke.b, target->stroke.a);
          cairo_rectangle(cr, text.x + text.width, text.y, kCaretWidth, text.height);
          cairo_fill(cr);
          bounds = unionRect(
              bounds, AnnotationRect{.x = text.x + text.width, .y = text.y, .width = kCaretWidth, .height = text.height}
          );
        }
        cairo_restore(cr);
      }
    }
    cairo_destroy(cr);

    const AnnotationRect device = toDeviceRect(bounds, inst.canvasScale, inst.deviceWidth, inst.deviceHeight);
    inst.activePainted = device;
    if (changedDevice.has_value()) {
      if (changedDevice->width > 0.0 && changedDevice->height > 0.0) {
        inst.activeDirty = inst.activeDirty.has_value() ? unionRect(*inst.activeDirty, *changedDevice) : *changedDevice;
      }
    } else if (device.width > 0.0) {
      inst.activeDirty = inst.activeDirty.has_value() ? unionRect(*inst.activeDirty, device) : device;
    }
  }

  void AnnotationOverlay::uploadDirty(Instance& inst, Renderer& renderer) {
    const auto upload = [&](std::optional<AnnotationRect>& dirty, cairo_surface_t* surface, TextureHandle& texture,
                            Image* layer) {
      if (!dirty.has_value()) {
        return;
      }
      const AnnotationRect rect = *dirty;
      dirty.reset();
      if (surface == nullptr || !texture.valid()) {
        return;
      }
      const int x = static_cast<int>(rect.x);
      const int y = static_cast<int>(rect.y);
      const int width = static_cast<int>(rect.width);
      const int height = static_cast<int>(rect.height);
      if (width <= 0 || height <= 0) {
        return;
      }
      copyArgb32RectToRgba(surface, x, y, width, height, m_uploadScratch);
      if (m_uploadScratch.empty()) {
        return;
      }
      if (!renderer.textureManager().updateSubImage(
              texture, m_uploadScratch.data(), x, y, width, height, TextureDataFormat::Rgba
          )) {
        kLog.warn("annotation layer upload failed");
        return;
      }
      if (layer != nullptr) {
        layer->markPaintDirty();
      }
    };

    upload(inst.committedDirty, inst.committedSurface, inst.committedTexture, inst.committedLayer);
    upload(inst.activeDirty, inst.activeSurface, inst.activeTexture, inst.activeLayer);
  }

  void AnnotationOverlay::updateCropVisuals(Instance& inst) {
    if (inst.dimTop == nullptr) {
      return;
    }
    const auto crop = documentFor(inst).crop();
    const auto hide = [&] {
      for (Box* box : {inst.dimTop, inst.dimBottom, inst.dimLeft, inst.dimRight, inst.cropFrame}) {
        if (box != nullptr) {
          box->setVisible(false);
        }
      }
    };
    if (!crop.has_value() || crop->width <= 0.0 || crop->height <= 0.0) {
      hide();
      return;
    }

    const auto left = static_cast<float>(inst.canvasRect.x);
    const auto top = static_cast<float>(inst.canvasRect.y);
    const auto right = static_cast<float>(inst.canvasRect.x + inst.canvasRect.width);
    const auto bottom = static_cast<float>(inst.canvasRect.y + inst.canvasRect.height);
    const float holeX0 = std::clamp(static_cast<float>(inst.canvasRect.x + crop->x), left, right);
    const float holeY0 = std::clamp(static_cast<float>(inst.canvasRect.y + crop->y), top, bottom);
    const float holeX1 = std::clamp(static_cast<float>(inst.canvasRect.x + crop->x + crop->width), left, right);
    const float holeY1 = std::clamp(static_cast<float>(inst.canvasRect.y + crop->y + crop->height), top, bottom);
    if (holeX1 <= holeX0 || holeY1 <= holeY0) {
      hide();
      return;
    }

    inst.dimTop->setVisible(true);
    inst.dimTop->setPosition(left, top);
    inst.dimTop->setSize(right - left, holeY0 - top);
    inst.dimBottom->setVisible(true);
    inst.dimBottom->setPosition(left, holeY1);
    inst.dimBottom->setSize(right - left, bottom - holeY1);
    inst.dimLeft->setVisible(true);
    inst.dimLeft->setPosition(left, holeY0);
    inst.dimLeft->setSize(holeX0 - left, holeY1 - holeY0);
    inst.dimRight->setVisible(true);
    inst.dimRight->setPosition(holeX1, holeY0);
    inst.dimRight->setSize(right - holeX1, holeY1 - holeY0);

    if (inst.cropFrame != nullptr) {
      inst.cropFrame->setVisible(true);
      inst.cropFrame->setPosition(holeX0 - kCropFrameWidth, holeY0 - kCropFrameWidth);
      inst.cropFrame->setSize(
          (holeX1 - holeX0) + (kCropFrameWidth * 2.0F), (holeY1 - holeY0) + (kCropFrameWidth * 2.0F)
      );
    }
  }

  bool AnnotationOverlay::onPointerEvent(const PointerEvent& event) {
    if (!m_active) {
      return false;
    }

    Instance* target = instanceForSurface(event.surface);
    if (target == nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->pointerInside) {
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

    // Surface-local pointer position, kept for the toolbar drag which needs an absolute
    // anchor rather than the handle-relative deltas a relayout invalidates.
    if (event.type != PointerEvent::Type::Leave) {
      m_pointerX = event.sx;
      m_pointerY = event.sy;
    }

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
        return true;
      }
      return false;
    case PointerEvent::Type::Motion:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (onTarget || target->pointerInside) {
        if (m_toolbarDragInstance != nullptr) {
          dragToolbarTo(event.sx, event.sy);
          return true;
        }
        target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      return false;
    case PointerEvent::Type::Button: {
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
    }
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

  void AnnotationOverlay::onCanvasPress(Instance& inst, double localX, double localY, bool pressed) {
    if (!pressed) {
      finishGesture();
      return;
    }

    // Drawing dismisses the stroke popover; the press still starts the mark.
    if (m_strokePanelOpen) {
      m_strokePanelOpen = false;
      requestRedrawAll();
    }

    // A fresh press elsewhere ends an open text edit before starting anything new.
    if (m_textIndex.has_value()) {
      commitTextEdit();
    }

    const AnnotationPoint point{.x = localX - inst.canvasRect.x, .y = localY - inst.canvasRect.y};
    if (point.x < 0.0 || point.y < 0.0 || point.x > inst.canvasRect.width || point.y > inst.canvasRect.height) {
      return;
    }

    AnnotationDocument& doc = documentFor(inst);
    const AnnotationTool tool = m_tools.tool;
    const std::size_t index = toolIndex(tool);
    const double width = m_tools.width[index];
    const AnnotationColor color = m_tools.color[index];

    m_gestureInstance = &inst;
    m_gestureStart = point;
    m_gestureLast = point;
    m_moved = false;
    m_moveIndex.reset();
    m_cropDragging = false;

    switch (tool) {
    case AnnotationTool::Move: {
      const auto hit = doc.hitTest(point, kMoveHitTolerance);
      if (!hit.has_value()) {
        m_gestureInstance = nullptr;
        return;
      }
      doc.beginInteraction();
      m_moveIndex = hit;
      m_gestureActive = true;
      inst.committedNeedsFullRedraw = true;
      redrawActive(inst);
      break;
    }
    case AnnotationTool::Eraser:
      doc.beginInteraction();
      m_gestureActive = true;
      if (doc.eraseAlong(point, point, width / 2.0) > 0) {
        inst.committedNeedsFullRedraw = true;
      }
      break;
    case AnnotationTool::Crop:
      m_gestureActive = true;
      m_cropDragging = true;
      m_cropStart = point;
      doc.setCrop(std::nullopt);
      inst.renderedGeneration = doc.generation();
      updateCropVisuals(inst);
      break;
    case AnnotationTool::Text: {
      doc.beginInteraction();
      m_gestureActive = true;
      doc.push(
          Annotation{
              .tool = AnnotationTool::Text,
              .width = width,
              .stroke = color,
              .fill = AnnotationColor{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
              .points = {point},
              .text = {},
          }
      );
      m_textInstance = &inst;
      m_textIndex = doc.annotations().size() - 1;
      redrawActive(inst);
      break;
    }
    case AnnotationTool::Numbering: {
      doc.beginInteraction();
      int count = 0;
      for (const auto& annotation : doc.annotations()) {
        if (annotation.tool == AnnotationTool::Numbering) {
          ++count;
        }
      }
      const Annotation& added = doc.push(
          Annotation{
              .tool = AnnotationTool::Numbering,
              .width = width,
              .stroke = color,
              .fill = AnnotationColor{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
              .points = {point},
              .text = std::to_string(count + 1),
          }
      );
      foldIntoCommitted(inst, added);
      doc.endInteraction();
      inst.renderedGeneration = doc.generation();
      m_gestureActive = false;
      m_gestureInstance = nullptr;
      break;
    }
    default: {
      doc.beginInteraction();
      m_gestureActive = true;
      AnnotationColor fill{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F};
      const bool shape = tool == AnnotationTool::Rectangle || tool == AnnotationTool::Circle;
      if (shape && m_tools.fill != AnnotationFillMode::Off) {
        fill = color;
        if (m_tools.fill == AnnotationFillMode::Semi) {
          fill.a = kSemiFillAlpha;
        }
      }
      std::vector<AnnotationPoint> points{point};
      if (annotationToolIsShape(tool)) {
        points.push_back(point);
      }
      doc.push(
          Annotation{
              .tool = tool,
              .width = width,
              .stroke = color,
              .fill = fill,
              .points = std::move(points),
              .text = {},
          }
      );
      redrawActive(inst);
      break;
    }
    }

    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::onCanvasMotion(Instance& inst, double localX, double localY) {
    if (!m_gestureActive || m_gestureInstance != &inst) {
      return;
    }
    AnnotationDocument& doc = documentFor(inst);
    const AnnotationPoint point{.x = localX - inst.canvasRect.x, .y = localY - inst.canvasRect.y};

    if (m_moveIndex.has_value()) {
      doc.offset(*m_moveIndex, point.x - m_gestureLast.x, point.y - m_gestureLast.y);
      m_gestureLast = point;
      m_moved = true;
      inst.committedNeedsFullRedraw = true;
      queueActiveRedraw(inst, std::nullopt);
    } else if (m_tools.tool == AnnotationTool::Eraser) {
      if (doc.eraseAlong(m_gestureLast, point, m_tools.width[toolIndex(AnnotationTool::Eraser)] / 2.0) > 0) {
        inst.committedNeedsFullRedraw = true;
      }
      m_gestureLast = point;
    } else if (m_cropDragging) {
      doc.setCrop(
          AnnotationRect{
              .x = std::min(m_cropStart.x, point.x),
              .y = std::min(m_cropStart.y, point.y),
              .width = std::abs(point.x - m_cropStart.x),
              .height = std::abs(point.y - m_cropStart.y),
          }
      );
      inst.renderedGeneration = doc.generation();
      m_gestureLast = point;
      updateCropVisuals(inst);
    } else {
      Annotation* active = doc.active();
      if (active == nullptr) {
        return;
      }
      std::optional<AnnotationRect> changedBounds;
      if (annotationToolIsShape(active->tool)) {
        AnnotationPoint end = point;
        // A held Shift never produces a key event, so the constraint reads the seat's live state.
        const std::uint32_t modifiers = m_wayland != nullptr ? m_wayland->keyboardModifiers() : 0;
        if ((modifiers & KeyMod::Shift) != 0 && !active->points.empty()) {
          const AnnotationPoint start = active->points.front();
          end = (active->tool == AnnotationTool::Line || active->tool == AnnotationTool::Arrow)
              ? snapAngle45(start, point)
              : snapSquare(start, point);
        }
        if (active->points.size() < 2) {
          active->points.push_back(end);
        } else {
          active->points.back() = end;
        }
      } else if (active->points.empty()) {
        active->points.push_back(point);
      } else {
        const AnnotationPoint last = active->points.back();
        if (std::hypot(point.x - last.x, point.y - last.y) < kFreehandMinDistance) {
          return;
        }
        active->points.push_back(point);
        changedBounds = freehandTailBounds(active->points, active->width);
      }
      doc.touch();
      m_gestureLast = point;
      queueActiveRedraw(inst, changedBounds);
    }

    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::finishGesture() {
    if (!m_gestureActive || m_gestureInstance == nullptr) {
      return;
    }
    Instance& inst = *m_gestureInstance;
    AnnotationDocument& doc = documentFor(inst);

    if (m_textIndex.has_value()) {
      // Releasing the button keeps the caret; Return or Escape ends the edit.
      return;
    }

    if (m_moveIndex.has_value()) {
      if (m_moved) {
        doc.endInteraction();
      } else {
        doc.discardInteraction();
      }
      m_moveIndex.reset();
      m_gestureActive = false;
      m_gestureInstance = nullptr;
      inst.committedNeedsFullRedraw = true;
      redrawActive(inst);
    } else if (m_cropDragging) {
      m_cropDragging = false;
      m_gestureActive = false;
      m_gestureInstance = nullptr;
      if (const auto crop = doc.crop(); crop.has_value() && (crop->width < 2.0 || crop->height < 2.0)) {
        doc.setCrop(std::nullopt);
      }
      inst.renderedGeneration = doc.generation();
      updateCropVisuals(inst);
    } else if (m_tools.tool == AnnotationTool::Eraser) {
      doc.endInteraction();
      m_gestureActive = false;
      m_gestureInstance = nullptr;
    } else {
      if (Annotation* active = doc.active(); active != nullptr) {
        if (isFreehandTool(active->tool) && simplifyFreehandPoints(active->points)) {
          doc.touch();
        }
        foldIntoCommitted(inst, *active);
      }
      doc.endInteraction();
      m_gestureActive = false;
      m_gestureInstance = nullptr;
      inst.renderedGeneration = doc.generation();
      redrawActive(inst);
    }

    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::commitTextEdit() {
    if (!m_textIndex.has_value() || m_textInstance == nullptr) {
      return;
    }
    Instance& inst = *m_textInstance;
    AnnotationDocument& doc = documentFor(inst);
    Annotation* target = doc.active();
    const bool discard = target == nullptr || target->text.empty();

    m_textIndex.reset();
    m_textInstance = nullptr;
    if (discard) {
      doc.discardInteraction();
    } else {
      foldIntoCommitted(inst, *target);
      doc.endInteraction();
    }
    m_gestureActive = false;
    m_gestureInstance = nullptr;
    inst.renderedGeneration = doc.generation();
    redrawActive(inst);
    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::cancelTextEdit() {
    if (!m_textIndex.has_value() || m_textInstance == nullptr) {
      return;
    }
    Instance& inst = *m_textInstance;
    AnnotationDocument& doc = documentFor(inst);
    m_textIndex.reset();
    m_textInstance = nullptr;
    doc.discardInteraction();
    m_gestureActive = false;
    m_gestureInstance = nullptr;
    inst.renderedGeneration = doc.generation();
    redrawActive(inst);
    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  bool AnnotationOverlay::handleTextKey(const KeyboardEvent& event) {
    if (!m_textIndex.has_value() || m_textInstance == nullptr) {
      return false;
    }
    if (KeySymbol::isEnter(event.sym)) {
      commitTextEdit();
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      cancelTextEdit();
      return true;
    }

    Instance& inst = *m_textInstance;
    AnnotationDocument& doc = documentFor(inst);
    Annotation* target = doc.active();
    if (target == nullptr) {
      cancelTextEdit();
      return true;
    }

    if (KeySymbol::isBackspace(event.sym)) {
      popUtf8(target->text);
    } else if (event.utf32 != 0 && !event.preedit) {
      appendUtf8(target->text, event.utf32);
    } else {
      // Every other key is swallowed so shortcuts cannot fire mid-word.
      return true;
    }
    doc.touch();
    inst.renderedGeneration = doc.generation();
    redrawActive(inst);
    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
    return true;
  }

  bool AnnotationOverlay::handleToolKey(std::uint32_t sym) {
    const auto pick = [&](AnnotationTool tool) {
      selectTool(tool);
      return true;
    };
    switch (sym) {
    case XKB_KEY_m:
    case XKB_KEY_M:
      return pick(AnnotationTool::Move);
    case XKB_KEY_b:
    case XKB_KEY_B:
      return pick(AnnotationTool::Brush);
    case XKB_KEY_h:
    case XKB_KEY_H:
      return pick(AnnotationTool::Highlighter);
    case XKB_KEY_l:
    case XKB_KEY_L:
      return pick(AnnotationTool::Line);
    case XKB_KEY_a:
    case XKB_KEY_A:
      return pick(AnnotationTool::Arrow);
    case XKB_KEY_s:
    case XKB_KEY_S:
      return pick(AnnotationTool::Rectangle);
    case XKB_KEY_o:
    case XKB_KEY_O:
      return pick(AnnotationTool::Circle);
    case XKB_KEY_t:
    case XKB_KEY_T:
      return pick(AnnotationTool::Text);
    case XKB_KEY_n:
    case XKB_KEY_N:
      return pick(AnnotationTool::Numbering);
    case XKB_KEY_u:
    case XKB_KEY_U:
      return pick(AnnotationTool::Blur);
    case XKB_KEY_e:
    case XKB_KEY_E:
      return pick(AnnotationTool::Eraser);
    case XKB_KEY_c:
    case XKB_KEY_C:
      return pick(AnnotationTool::Crop);
    case XKB_KEY_f:
    case XKB_KEY_F:
      cycleFill();
      return true;
    case XKB_KEY_z:
    case XKB_KEY_Z:
      requestFreeze();
      return true;
    case XKB_KEY_p:
    case XKB_KEY_P:
      toggleCursor();
      return true;
    case XKB_KEY_1:
    case XKB_KEY_2:
    case XKB_KEY_3:
    case XKB_KEY_4:
    case XKB_KEY_5:
    case XKB_KEY_6:
    case XKB_KEY_7:
    case XKB_KEY_8:
      // Same as clicking swatch 1 to 8; tools that ignore color leave the key unhandled.
      if (!toolUsesColor(m_tools.tool)) {
        return false;
      }
      setToolColor(kSwatches[sym - XKB_KEY_1].color);
      return true;
    case XKB_KEY_bracketleft:
      adjustToolWidth(-1.0);
      return true;
    case XKB_KEY_bracketright:
      adjustToolWidth(1.0);
      return true;
    default:
      return false;
    }
  }

  bool AnnotationOverlay::onKeyboardEvent(const KeyboardEvent& event) {
    if (!m_active || m_wayland == nullptr) {
      return false;
    }
    Instance* keyboardInst = instanceForSurface(m_wayland->lastKeyboardSurface());
    if (keyboardInst == nullptr) {
      return false;
    }

    // A focused backdrop value box takes the keyboard; Escape hands focus back to the canvas, which
    // commits the box like clicking elsewhere.
    if (backdropPanelInputFocused(*keyboardInst)) {
      Instance& inst = *keyboardInst;
      if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
        inst.inputDispatcher.setFocus(inst.canvas);
      } else {
        inst.inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
      }
      if (inst.surface != nullptr
          && inst.sceneRoot != nullptr
          && (inst.sceneRoot->paintDirty() || inst.sceneRoot->layoutDirty())) {
        if (inst.sceneRoot->layoutDirty()) {
          inst.surface->requestLayout();
        } else {
          inst.surface->requestRedraw();
        }
      }
      return true;
    }

    if (!event.pressed) {
      return false;
    }

    if (m_textIndex.has_value()) {
      return handleTextKey(event);
    }

    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;

    if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      if (m_strokePanelOpen) {
        m_strokePanelOpen = false;
        requestRedrawAll();
        return true;
      }
      if (m_gestureActive && m_gestureInstance != nullptr) {
        Instance* inst = m_gestureInstance;
        AnnotationDocument& doc = documentFor(*inst);
        m_gestureActive = false;
        m_gestureInstance = nullptr;
        m_moveIndex.reset();
        m_cropDragging = false;
        doc.discardInteraction();
        inst->committedNeedsFullRedraw = true;
        redrawActive(*inst);
        updateCropVisuals(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestUpdateOnly();
        }
        return true;
      }
      if (Instance* inst = exportTarget(); inst != nullptr && documentFor(*inst).crop().has_value()) {
        AnnotationDocument& doc = documentFor(*inst);
        doc.setCrop(std::nullopt);
        inst->renderedGeneration = doc.generation();
        updateCropVisuals(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestUpdateOnly();
        }
        return true;
      }
      requestClose();
      return true;
    }

    if (ctrl && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z)) {
      if (shift) {
        redo();
      } else {
        undo();
      }
      return true;
    }
    if (ctrl && (event.sym == XKB_KEY_y || event.sym == XKB_KEY_Y)) {
      redo();
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Copy, event.sym, event.modifiers)) {
      requestExport(AnnotationExport::Copy);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Save, event.sym, event.modifiers)) {
      requestExport(AnnotationExport::Save);
      return true;
    }
    if (KeySymbol::isEnter(event.sym)) {
      if (m_mode == AnnotationMode::Image) {
        requestExport(AnnotationExport::Done);
        return true;
      }
      return false;
    }
    if (ctrl || (event.modifiers & (KeyMod::Alt | KeyMod::Super)) != 0) {
      return false;
    }
    return handleToolKey(event.sym);
  }

  // A toolbar click while the caret is live would otherwise be swallowed: the open text
  // interaction counts as a gesture, and gestures block tool switching.
  void AnnotationOverlay::commitPendingText() {
    if (m_textIndex.has_value()) {
      commitTextEdit();
    }
  }

  // Restyling mid-edit retargets the text under the caret, so S/M/L and the swatches read as
  // acting on what is on screen instead of only on the next annotation.
  void AnnotationOverlay::restyleActiveText() {
    if (!m_textIndex.has_value() || m_textInstance == nullptr) {
      return;
    }
    Instance& inst = *m_textInstance;
    AnnotationDocument& doc = documentFor(inst);
    Annotation* target = doc.active();
    if (target == nullptr || target->tool != AnnotationTool::Text) {
      return;
    }
    const std::size_t index = toolIndex(AnnotationTool::Text);
    target->width = m_tools.width[index];
    target->stroke = m_tools.color[index];
    doc.touch();
    inst.renderedGeneration = doc.generation();
    redrawActive(inst);
    if (inst.surface != nullptr) {
      inst.surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::selectTool(AnnotationTool tool) {
    commitPendingText();
    if (m_gestureActive) {
      return;
    }
    if (m_mode == AnnotationMode::Live && (tool == AnnotationTool::Blur || tool == AnnotationTool::Crop)) {
      return;
    }
    m_tools.tool = tool;
    // The stroke button hides for tools without a stroke; its popover goes with it.
    if (!toolUsesSize(tool)) {
      m_strokePanelOpen = false;
    }
    requestRedrawAll();
  }

  void AnnotationOverlay::setToolColor(AnnotationColor color) {
    m_tools.color[toolIndex(m_tools.tool)] = color;
    restyleActiveText();
    requestRedrawAll();
  }

  void AnnotationOverlay::adjustToolWidth(double delta) {
    const std::size_t index = toolIndex(m_tools.tool);
    const bool text = m_tools.tool == AnnotationTool::Text;
    m_tools.width[index] = std::clamp(
        m_tools.width[index] + delta, text ? kMinTextWidth : kMinToolWidth, text ? kMaxTextWidth : kMaxToolWidth
    );
    restyleActiveText();
    requestRedrawAll();
  }

  void AnnotationOverlay::cycleFill(bool forward) {
    commitPendingText();
    m_tools.fill = nextFillMode(m_tools.fill, forward);
    requestRedrawAll();
  }

  void AnnotationOverlay::undo() {
    commitPendingText();
    if (m_gestureActive) {
      return;
    }
    Instance* inst = exportTarget();
    if (inst == nullptr || !documentFor(*inst).undo()) {
      return;
    }
    inst->committedNeedsFullRedraw = true;
    updateCropVisuals(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::redo() {
    commitPendingText();
    if (m_gestureActive) {
      return;
    }
    Instance* inst = exportTarget();
    if (inst == nullptr || !documentFor(*inst).redo()) {
      return;
    }
    inst->committedNeedsFullRedraw = true;
    updateCropVisuals(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestUpdateOnly();
    }
  }

  void AnnotationOverlay::requestFreeze() {
    commitPendingText();
    if (!m_active || m_mode != AnnotationMode::Live) {
      return;
    }
    if (!m_onFreeze) {
      return;
    }
    FreezeCallback onFreeze = m_onFreeze;
    DeferredCall::callLater([onFreeze] { onFreeze(); });
  }

  void AnnotationOverlay::requestCaptureRegion() {
    if (!m_active || !m_onCaptureRegion) {
      return;
    }
    CaptureRegionCallback onCaptureRegion = m_onCaptureRegion;
    DeferredCall::callLater([onCaptureRegion] { onCaptureRegion(); });
  }

  void AnnotationOverlay::toggleCursor(Instance* instance) {
    if (!m_active) {
      return;
    }
    if (instance == nullptr) {
      instance = exportTarget();
    }
    if (instance == nullptr) {
      return;
    }
    const ScreenshotImage* image = screenshot(*instance);
    if (image == nullptr || !image->canToggleCursor()) {
      if (m_onFeedback) {
        const std::string message = i18n::tr(cursorTooltipKey(*instance));
        FeedbackCallback onFeedback = m_onFeedback;
        DeferredCall::callLater([onFeedback, message] { onFeedback(message); });
      }
      return;
    }
    commitPendingText();
    m_cursorVisible = !m_cursorVisible;
    requestRedrawAll();
  }

  void AnnotationOverlay::requestExport(AnnotationExport action) {
    if (!m_active || m_mode == AnnotationMode::Live) {
      return;
    }
    if (m_textIndex.has_value()) {
      commitTextEdit();
    }
    Instance* inst = exportTarget();
    if (inst == nullptr) {
      return;
    }
    const ScreencopyImage* bg = background(*inst);
    if (bg == nullptr) {
      if (m_onFailure) {
        m_onFailure(i18n::tr("bar.screenshot.annotate-export-failed"));
      }
      return;
    }

    ScreencopyImage image = composeExport(*bg, inst->canvasRect.width, inst->canvasRect.height, documentFor(*inst));
    if (image.rgba.empty()) {
      if (m_onFailure) {
        m_onFailure(i18n::tr("bar.screenshot.annotate-export-failed"));
      }
      return;
    }
    if (m_onExport) {
      m_onExport(std::move(image), action);
    }
    if (action == AnnotationExport::Done) {
      closeOverlay();
    }
  }

  void AnnotationOverlay::requestClose() {
    if (m_copyOnClose && m_mode != AnnotationMode::Live) {
      // Closes even when the copy fails; the failure is already notified.
      requestExport(AnnotationExport::Copy);
    }
    closeOverlay();
  }

  void AnnotationOverlay::closeOverlay() {
    if (!m_active) {
      return;
    }
    DeferredCall::callLater([this] { cancel(); });
  }

  std::unique_ptr<Flex> AnnotationOverlay::buildBackdropPanel(Instance& inst) {
    Color panelFill = colorForRole(ColorRole::Surface);
    panelFill.a = kToolbarFillAlpha;

    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm,
        .paddingV = Style::spaceMd,
        .paddingH = Style::spaceMd,
        .fill = fixedColorSpec(panelFill),
        .radius = Style::radiusXl,
        .border = colorSpecFromRole(ColorRole::OutlineVariant),
        .borderWidth = Style::borderWidth,
        .width = kBackdropPanelContentWidth + (2.0F * Style::spaceMd),
        .visible = false,
        .participatesInLayout = false,
    });

    // Label on the left, switch flush with the panel's right edge.
    const auto toggleRow = [](const char* key, bool checked, bool strong, std::function<void(bool)> onChange) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
      row->addChild(
          ui::label({
              .text = i18n::tr(key),
              .fontSize = strong ? Style::fontSizeBody : Style::fontSizeCaption,
              .fontWeight = strong ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0F,
          })
      );
      row->addChild(ui::toggle({.checked = checked, .toggleSize = ToggleSize::Small, .onChange = std::move(onChange)}));
      return row;
    };

    // Same cluster as a settings slider: track plus editable value box.
    const auto sliderRow = [this, &inst](BackdropSlider which) {
      const auto index = static_cast<std::size_t>(which);
      const BackdropSliderSpec& spec = backdropSliders()[index];
      const double value = spec.get(m_backdrop);
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
      row->addChild(
          ui::label({
              .text = i18n::tr(spec.labelKey),
              .fontSize = Style::fontSizeCaption,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .minWidth = kBackdropLabelWidth,
              .maxWidth = kBackdropLabelWidth,
          })
      );

      Input* valueInput = nullptr;
      auto input = ui::input({
          .out = &valueInput,
          .value = formatBackdropValue(spec, value),
          .fontSize = Style::fontSizeCaption,
          .controlHeight = Style::controlHeightSm,
          .horizontalPadding = Style::spaceXs,
          .width = kBackdropValueWidth,
          .height = Style::controlHeightSm,
      });

      Slider* slider = nullptr;
      row->addChild(
          ui::slider({
              .out = &slider,
              .minValue = spec.minValue,
              .maxValue = spec.maxValue,
              .step = spec.step,
              .value = value,
              .flexGrow = 1.0F,
              .onValueChanged = [this, index, valueInput](double next) {
                valueInput->setInvalid(false);
                valueInput->setValue(formatBackdropValue(backdropSliders()[index], next));
                editBackdrop([index, next](BackdropConfig& backdrop) { backdropSliders()[index].set(backdrop, next); });
              },
          })
      );

      const auto commitText = [this, index, slider, valueInput](const std::string& text) {
        const BackdropSliderSpec& sliderSpec = backdropSliders()[index];
        const auto parsed = StringUtils::parseDotDecimal<double>(text);
        if (!parsed.has_value() || *parsed < sliderSpec.minValue || *parsed > sliderSpec.maxValue) {
          valueInput->setInvalid(true);
          return;
        }
        slider->setValue(*parsed);
        const double snapped = slider->value();
        valueInput->setInvalid(false);
        valueInput->setValue(formatBackdropValue(sliderSpec, snapped));
        editBackdrop([index, snapped](BackdropConfig& backdrop) { backdropSliders()[index].set(backdrop, snapped); });
      };
      valueInput->setOnChange([valueInput](const std::string& /*text*/) { valueInput->setInvalid(false); });
      valueInput->setOnSubmit(commitText);
      valueInput->setOnFocusLoss([commitText, valueInput] { commitText(valueInput->value()); });
      inst.backdropValueInputs[index] = valueInput;
      row->addChild(std::move(input));
      return row;
    };

    panel->addChild(toggleRow("bar.annotate.backdrop-enabled", m_backdrop.enabled, true, [this](bool on) {
      editBackdrop([on](BackdropConfig& backdrop) { backdrop.enabled = on; });
    }));

    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
    body->addChild(ui::separator({}));
    body->addChild(toggleRow("bar.annotate.backdrop-gradient", m_backdrop.gradient, false, [this](bool on) {
      editBackdrop([on](BackdropConfig& backdrop) { backdrop.gradient = on; });
    }));

    auto gradientRows = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
    auto chips = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween});
    const auto& presets = gradientPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
      auto chip = ui::inputArea({
          .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .tooltip = i18n::tr(presets[i].tooltipKey),
          .width = kGradientChipSize,
          .height = kGradientChipSize,
          .onClick = [this, i](const InputArea::PointerData& /*data*/) {
            editBackdrop([i](BackdropConfig& backdrop) {
              backdrop.gradientStart = gradientPresets()[i].start;
              backdrop.gradientEnd = gradientPresets()[i].end;
              backdrop.gradient = true;
            });
          },
      });
      inst.gradientChips[i] =
          static_cast<Box*>(chip->addChild(ui::box({.width = kGradientChipSize, .height = kGradientChipSize})));
      chips->addChild(std::move(chip));
    }
    gradientRows->addChild(std::move(chips));
    gradientRows->addChild(sliderRow(BackdropSlider::Angle));
    inst.backdropGradientRows = static_cast<Flex*>(body->addChild(std::move(gradientRows)));

    body->addChild(sliderRow(BackdropSlider::Padding));
    body->addChild(sliderRow(BackdropSlider::Radius));
    body->addChild(ui::separator({}));
    body->addChild(toggleRow("bar.annotate.backdrop-shadow", m_backdrop.shadow, false, [this](bool on) {
      editBackdrop([on](BackdropConfig& backdrop) { backdrop.shadow = on; });
    }));

    auto shadowRows = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
    shadowRows->addChild(sliderRow(BackdropSlider::ShadowBlur));
    shadowRows->addChild(sliderRow(BackdropSlider::ShadowOpacity));
    shadowRows->addChild(sliderRow(BackdropSlider::ShadowOffsetX));
    shadowRows->addChild(sliderRow(BackdropSlider::ShadowOffsetY));
    inst.backdropShadowRows = static_cast<Flex*>(body->addChild(std::move(shadowRows)));

    inst.backdropBody = static_cast<Flex*>(panel->addChild(std::move(body)));
    inst.backdropPanel = panel.get();
    refreshBackdropPanel(inst, nullptr);
    return panel;
  }

  void AnnotationOverlay::refreshBackdropPanel(Instance& inst, Renderer* renderer) {
    if (inst.backdropPanel == nullptr) {
      return;
    }
    bool needsLayout = false;
    const auto show = [&needsLayout](Node* node, bool visible) {
      if (node != nullptr && node->visible() != visible) {
        node->setVisible(visible);
        node->setParticipatesInLayout(visible);
        needsLayout = true;
      }
    };
    show(inst.backdropPanel, m_backdropPanelOpen && m_mode == AnnotationMode::Image);
    show(inst.backdropBody, m_backdrop.enabled);
    show(inst.backdropGradientRows, m_backdrop.gradient);
    show(inst.backdropShadowRows, m_backdrop.shadow);

    int marked = -1;
    const auto& presets = gradientPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
      if (m_backdrop.gradientStart == presets[i].start && m_backdrop.gradientEnd == presets[i].end) {
        marked = static_cast<int>(i);
        break;
      }
    }
    if (marked != inst.markedGradient) {
      for (std::size_t i = 0; i < presets.size(); ++i) {
        if (inst.gradientChips[i] != nullptr) {
          paintGradientChip(*inst.gradientChips[i], presets[i], static_cast<int>(i) == marked);
        }
      }
      inst.markedGradient = marked;
    }

    // The surface does not lay out the overlay scene itself; edits in a value box mark the panel
    // layout-dirty and only a panel relayout moves the caret to the new text.
    if ((needsLayout || inst.backdropPanel->layoutDirty()) && renderer != nullptr) {
      inst.backdropPanel->layout(*renderer);
    }
  }

  // Hangs off the toolbar's trailing edge, on whichever side of it has more room.
  void AnnotationOverlay::positionBackdropPanel(Instance& inst) {
    if (inst.backdropPanel == nullptr || inst.toolbar == nullptr || inst.surface == nullptr) {
      return;
    }
    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    const float width = inst.backdropPanel->width();
    const float height = inst.backdropPanel->height();
    const float toolbarBottom = inst.toolbar->y() + inst.toolbar->height();
    const bool below = inst.toolbar->y() + (inst.toolbar->height() * 0.5F) <= surfaceH * 0.5F;

    float x = inst.toolbar->x() + inst.toolbar->width() - width;
    float y = below ? toolbarBottom + Style::spaceSm : inst.toolbar->y() - Style::spaceSm - height;
    x = std::clamp(x, kToolbarEdgeMargin, std::max(kToolbarEdgeMargin, surfaceW - width - kToolbarEdgeMargin));
    y = std::clamp(y, kToolbarEdgeMargin, std::max(kToolbarEdgeMargin, surfaceH - height - kToolbarEdgeMargin));
    inst.backdropPanel->setPosition(x, y);
  }

  // The preview reuses the export raster on a transparent stand-in for the capture, so the frame
  // matches the delivered image; the canvas layers cover the middle and only the frame shows.
  void AnnotationOverlay::updateBackdropPreview(Instance& inst, Renderer& renderer) {
    if (m_mode != AnnotationMode::Image
        || inst.backdropPreview == nullptr
        || inst.deviceWidth <= 0
        || inst.deviceHeight <= 0) {
      return;
    }
    const std::optional<ScreenshotBackdrop> backdrop = resolveScreenshotBackdrop(m_backdrop);

    const float maxRadius = static_cast<float>(std::min(inst.deviceWidth, inst.deviceHeight)) * 0.5F;
    const float radius = backdrop.has_value()
        ? std::clamp(static_cast<float>(backdrop->cornerRadius), 0.0F, maxRadius) / static_cast<float>(inst.canvasScale)
        : 0.0F;
    for (Image* layer : {inst.backdrop, inst.committedLayer, inst.activeLayer}) {
      if (layer != nullptr) {
        layer->setRadius(radius);
      }
    }
    if (inst.imageOutline != nullptr) {
      inst.imageOutline->setVisible(!backdrop.has_value());
    }

    if (!backdrop.has_value()) {
      inst.backdropPreview->setVisible(false);
      inst.previewBackdrop.reset();
      return;
    }
    const AnnotationRect& rect = inst.canvasRect;
    if (inst.previewBackdrop == backdrop
        && inst.previewCanvas.x == rect.x
        && inst.previewCanvas.y == rect.y
        && inst.previewCanvas.width == rect.width
        && inst.previewCanvas.height == rect.height) {
      return;
    }

    const double downscale =
        std::max(1.0, (inst.canvasScale / std::max(0.1, static_cast<double>(inst.scale))) * kBackdropPreviewDownscale);
    const auto scaled = [downscale](int pixels) { return static_cast<int>(std::lround(pixels / downscale)); };
    const int standInWidth = std::max(1, scaled(inst.deviceWidth));
    const int standInHeight = std::max(1, scaled(inst.deviceHeight));
    const ScreencopyImage standIn{
        .width = standInWidth,
        .height = standInHeight,
        .yInvert = false,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(standInWidth) * standInHeight * 4U, 0),
    };
    ScreenshotBackdrop previewBackdrop = *backdrop;
    previewBackdrop.padding = scaled(backdrop->padding);
    previewBackdrop.cornerRadius = scaled(backdrop->cornerRadius);
    previewBackdrop.shadowBlur = scaled(backdrop->shadowBlur);
    previewBackdrop.shadowOffsetX = scaled(backdrop->shadowOffsetX);
    previewBackdrop.shadowOffsetY = scaled(backdrop->shadowOffsetY);
    const ScreencopyImage frame = applyScreenshotBackdrop(standIn, previewBackdrop);

    inst.backdropPreview->clear(renderer);
    if (!inst.backdropPreview->setSourceRaw(
            renderer, frame.rgba.data(), frame.rgba.size(), frame.width, frame.height, frame.width * 4,
            PixmapFormat::RGBA, false
        )) {
      kLog.warn("failed to upload screenshot backdrop preview");
      inst.backdropPreview->setVisible(false);
      return;
    }
    // Map preview pixels onto the canvas so the stand-in lands exactly under it.
    const double unitX = rect.width / standInWidth;
    const double unitY = rect.height / standInHeight;
    inst.backdropPreview->setFit(ImageFit::Stretch);
    inst.backdropPreview->setSize(static_cast<float>(frame.width * unitX), static_cast<float>(frame.height * unitY));
    inst.backdropPreview->setPosition(
        static_cast<float>(rect.x - (previewBackdrop.padding * unitX)),
        static_cast<float>(rect.y - (previewBackdrop.padding * unitY))
    );
    inst.backdropPreview->setVisible(true);
    inst.previewBackdrop = backdrop;
    inst.previewCanvas = rect;
  }

  void AnnotationOverlay::toggleBackdropPanel() {
    m_backdropPanelOpen = !m_backdropPanelOpen;
    if (m_backdropPanelOpen) {
      m_strokePanelOpen = false;
    }
    requestRedrawAll();
  }

  void AnnotationOverlay::editBackdrop(const std::function<void(BackdropConfig&)>& edit) {
    edit(m_backdrop);
    m_backdropEdited = true;
    requestRedrawAll();
  }

  bool AnnotationOverlay::backdropPanelInputFocused(const Instance& inst) const {
    const InputArea* focused = inst.inputDispatcher.focusedArea();
    return focused != nullptr && std::ranges::any_of(inst.backdropValueInputs, [focused](const Input* input) {
             return input != nullptr && input->inputArea() == focused;
           });
  }

  std::unique_ptr<Flex> AnnotationOverlay::buildStrokePanel(Instance& inst) {
    Color panelFill = colorForRole(ColorRole::Surface);
    panelFill.a = kToolbarFillAlpha;

    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm,
        .paddingV = Style::spaceMd,
        .paddingH = Style::spaceMd,
        .fill = fixedColorSpec(panelFill),
        .radius = Style::radiusXl,
        .border = colorSpecFromRole(ColorRole::OutlineVariant),
        .borderWidth = Style::borderWidth,
        .visible = false,
        .participatesInLayout = false,
    });

    auto swatches = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceSm});
    for (std::size_t i = 0; i < kSwatches.size(); ++i) {
      const AnnotationColor color = kSwatches[i].color;
      inst.swatchButtons[i] = static_cast<Button*>(swatches->addChild(
          ui::button({
              .glyph = std::string("check"),
              .glyphSize = kSwatchGlyphSize,
              .customPalette = swatchPalette(color),
              .tooltip = i18n::tr(kSwatches[i].tooltipKey),
              .minWidth = kSwatchSize,
              .minHeight = kSwatchSize,
              .maxWidth = kSwatchSize,
              .maxHeight = kSwatchSize,
              .padding = 0.0F,
              .radius = Style::radiusSm,
              .width = kSwatchSize,
              .height = kSwatchSize,
              .onClick = [this, color] { setToolColor(color); },
          })
      ));
    }
    inst.strokeSwatchRow = static_cast<Flex*>(panel->addChild(std::move(swatches)));

    // S / M / L on the left, the pixel stepper for anything in between on the right.
    auto sizes = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceSm});
    inst.sizePresets = static_cast<Segmented*>(sizes->addChild(
        ui::segmented({
            .options =
                std::vector<ui::SegmentedOption>{
                    {.label = "S", .glyph = {}, .tooltip = i18n::tr("bar.annotate.size-small")},
                    {.label = "M", .glyph = {}, .tooltip = i18n::tr("bar.annotate.size-medium")},
                    {.label = "L", .glyph = {}, .tooltip = i18n::tr("bar.annotate.size-large")},
                },
            .selectedIndex = kDefaultSizePreset,
            .fontSize = Style::fontSizeCaption,
            .compact = true,
            .onChange = [this](std::size_t index) { applySizePreset(index); },
        })
    ));

    auto stepper = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs});
    inst.sizeDownButton = static_cast<Button*>(stepper->addChild(
        ui::button({
            .glyph = std::string("minus"),
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("bar.annotate.size-decrease"),
            .onClick = [this] { adjustToolWidth(-1.0); },
        })
    ));
    inst.sizeLabel = static_cast<Label*>(stepper->addChild(
        ui::label({
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .minWidth = kSizeLabelWidth,
            .textAlign = TextAlign::Center,
        })
    ));
    inst.sizeUpButton = static_cast<Button*>(stepper->addChild(
        ui::button({
            .glyph = std::string("plus"),
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("bar.annotate.size-increase"),
            .onClick = [this] { adjustToolWidth(1.0); },
        })
    ));
    sizes->addChild(std::move(stepper));
    panel->addChild(std::move(sizes));

    inst.strokePanel = panel.get();
    refreshStrokePanel(inst, nullptr);
    return panel;
  }

  void AnnotationOverlay::refreshStrokePanel(Instance& inst, Renderer* renderer) {
    if (inst.strokePanel == nullptr) {
      return;
    }
    bool needsLayout = false;
    const auto show = [&needsLayout](Node* node, bool visible) {
      if (node != nullptr && node->visible() != visible) {
        node->setVisible(visible);
        node->setParticipatesInLayout(visible);
        needsLayout = true;
      }
    };
    const std::size_t index = toolIndex(m_tools.tool);
    show(inst.strokePanel, m_strokePanelOpen);
    show(inst.strokeSwatchRow, toolUsesColor(m_tools.tool));

    if (inst.sizeLabel != nullptr) {
      inst.sizeLabel->setText(std::format("{}", static_cast<int>(std::lround(m_tools.width[index]))));
    }
    if (inst.sizePresets != nullptr && toolUsesSize(m_tools.tool)) {
      // setSelectedIndex fires onChange, which would snap a fine-tuned width back to a preset.
      m_syncingSizePreset = true;
      inst.sizePresets->setSelectedIndex(nearestSizePreset(m_tools.tool));
      m_syncingSizePreset = false;
    }

    // The tick moves between swatches, and a button laid out while its glyph was empty keeps a
    // zero-sized glyph box, which parks the mark in a corner. Relayout when the mark moves.
    int marked = -1;
    for (std::size_t i = 0; i < kSwatches.size(); ++i) {
      if (m_tools.color[index] == kSwatches[i].color) {
        marked = static_cast<int>(i);
        break;
      }
    }
    if (marked != inst.markedSwatch) {
      for (std::size_t i = 0; i < kSwatches.size(); ++i) {
        Button* swatch = inst.swatchButtons[i];
        if (swatch == nullptr || swatch->glyph() == nullptr) {
          continue;
        }
        const bool current = static_cast<int>(i) == marked;
        swatch->setSelected(current);
        if (current) {
          swatch->setGlyph("check");
        } else {
          (void)swatch->glyph()->setCodepoint(0);
        }
      }
      inst.markedSwatch = marked;
      needsLayout = true;
    }

    // The surface does not lay out the overlay scene itself; a new size label marks the panel
    // layout-dirty and only a panel relayout re-centers it.
    if ((needsLayout || inst.strokePanel->layoutDirty()) && renderer != nullptr) {
      inst.strokePanel->layout(*renderer);
    }
  }

  // Drops from the stroke button, centered on it, toward whichever side of the toolbar has more room.
  void AnnotationOverlay::positionStrokePanel(Instance& inst) {
    if (inst.strokePanel == nullptr
        || inst.strokeButton == nullptr
        || inst.toolbar == nullptr
        || inst.surface == nullptr) {
      return;
    }
    const auto surfaceW = static_cast<float>(inst.surface->width());
    const auto surfaceH = static_cast<float>(inst.surface->height());
    const float width = inst.strokePanel->width();
    const float height = inst.strokePanel->height();
    const float toolbarBottom = inst.toolbar->y() + inst.toolbar->height();
    const bool below = inst.toolbar->y() + (inst.toolbar->height() * 0.5F) <= surfaceH * 0.5F;

    float x = inst.toolbar->x() + inst.strokeButton->x() + ((inst.strokeButton->width() - width) * 0.5F);
    float y = below ? toolbarBottom + Style::spaceSm : inst.toolbar->y() - Style::spaceSm - height;
    x = std::clamp(x, kToolbarEdgeMargin, std::max(kToolbarEdgeMargin, surfaceW - width - kToolbarEdgeMargin));
    y = std::clamp(y, kToolbarEdgeMargin, std::max(kToolbarEdgeMargin, surfaceH - height - kToolbarEdgeMargin));
    inst.strokePanel->setPosition(x, y);
  }

  // Only one panel is open at a time, so opening this one closes the backdrop panel.
  void AnnotationOverlay::toggleStrokePanel() {
    m_strokePanelOpen = !m_strokePanelOpen;
    if (m_strokePanelOpen) {
      m_backdropPanelOpen = false;
    }
    requestRedrawAll();
  }

  void AnnotationOverlay::persistToolState() {
    if (!m_stateSetter) {
      return;
    }
    m_stateSetter("tool", annotationToolName(m_tools.tool));
    m_stateSetter("fill", annotationFillModeName(m_tools.fill));
    syncToolbarPositionsForConnectedOutputs();
    nlohmann::json toolbarPositions = nlohmann::json::object();
    for (const auto& [outputName, position] : m_tools.toolbarPositions) {
      toolbarPositions[outputName] = {{"x", position.x}, {"y", position.y}};
    }
    m_stateSetter("toolbar_positions", toolbarPositions.dump());
    for (std::size_t i = 0; i < kAnnotationToolCount; ++i) {
      const std::string name(annotationToolName(static_cast<AnnotationTool>(i)));
      m_stateSetter(std::format("width_{}", name), std::format("{}", m_tools.width[i]));
      const AnnotationColor& color = m_tools.color[i];
      m_stateSetter(std::format("color_{}", name), std::format("{},{},{},{}", color.r, color.g, color.b, color.a));
    }
  }

} // namespace capture
