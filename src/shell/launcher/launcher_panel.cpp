#include "shell/launcher/launcher_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "launcher/app_provider.h"
#include "render/animation/animation_manager.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "system/desktop_entry.h"
#include "system/distro_info.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_list_view.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

  constexpr std::size_t kRowOverscan = 3;
  // Minimum trimmed query length before prefixed opt-in providers join the global search.
  constexpr std::size_t kGlobalOptInMinChars = 2;
  constexpr float kIconSizeDefault = 40.0F;
  constexpr float kIconSizeCompact = 28.0F;
  constexpr std::size_t kAppGridColumns = 5;
  constexpr float kLauncherHeight = 620.0F;
  constexpr std::int32_t kCompactNavigationWidth = 680;
  constexpr float kLauncherAvatarSize = 44.0F;
  constexpr std::string_view kApplicationsProviderId = "Applications";
  constexpr std::string_view kSessionProviderId = "Session";
  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr std::string_view kProviderOverviewProviderId = "__launcher_provider_overview__";
  constexpr std::string_view kProviderOverviewResultPrefix = "provider:";

  double usageBoostForScore(double score, int usageCount, bool typedQuery) {
    if (usageCount <= 0) {
      return 0.0;
    }

    const double rawBoost = static_cast<double>(usageCount) * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }

    // For typed searches, usage should nudge close matches without letting a
    // weak fuzzy hit outrank a much stronger lexical match.
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string singleLinePreview(std::string_view text) {
    std::string preview;
    preview.reserve(text.size());
    bool lastWasSpace = false;
    for (const char c : text) {
      const bool whitespace = c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
      if (whitespace) {
        if (!lastWasSpace) {
          preview.push_back(' ');
          lastWasSpace = true;
        }
        continue;
      }
      preview.push_back(c);
      lastWasSpace = c == ' ';
    }
    return preview;
  }

  [[nodiscard]] bool isDetailPresentation(const LauncherResult& result) { return result.presentation == "detail"; }

  [[nodiscard]] std::string providerOverviewId(std::string_view prefix) {
    std::string id(kProviderOverviewResultPrefix);
    id += prefix;
    return id;
  }

  void sortResultsByScore(std::vector<LauncherResult>& results) {
    std::ranges::stable_sort(results, std::ranges::greater{}, &LauncherResult::score);
  }

  struct LauncherListStyle {
    float scale = 1.0F;
    bool showIcons = true;
    bool showAppOriginIndicator = true;
    bool compact = false;
    std::optional<ColorSpec> appIconColorizeTint;
    std::optional<ColorSpec> listItemBackground;
  };

  [[nodiscard]] float launcherIconSize(const LauncherListStyle& style) {
    return (style.compact ? kIconSizeCompact : kIconSizeDefault) * style.scale;
  }

  [[nodiscard]] float stableLabelHeight(const TextMetrics& metrics) { return std::round(metrics.bottom - metrics.top); }

  [[nodiscard]] float launcherTextStackHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float bodySize = Style::fontSizeBody * style.scale;
    float textHeight = stableLabelHeight(renderer.measureFont(bodySize, FontWeight::SemiBold));
    if (!style.compact) {
      const float captionSize = Style::fontSizeCaption * style.scale;
      textHeight += stableLabelHeight(renderer.measureFont(captionSize, FontWeight::Normal));
    }
    return textHeight;
  }

  [[nodiscard]] float launcherRowHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float textHeight = launcherTextStackHeight(renderer, style);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] float launcherRowHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float bodySize = Style::fontSizeBody * style.scale;
    const float captionSize = Style::fontSizeCaption * style.scale;
    const float textHeight = bodySize + (style.compact ? 0.0F : captionSize);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] float launcherAppGridLabelHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float fontSize = Style::fontSizeCaption * style.scale;
    const TextMetrics metrics =
        renderer.measureText("Ag\nyg", fontSize, FontWeight::Normal, wrapWidth, 2, TextAlign::Center);
    const float actualHeight = metrics.bottom - metrics.top;
    const float inkSpan = std::max(0.0F, metrics.inkBottom - metrics.inkTop);
    const float rowExtent = renderer.fontRowExtent(fontSize, FontWeight::Normal);
    return std::ceil(std::max({actualHeight, inkSpan, rowExtent * 2.0F}));
  }

  [[nodiscard]] float launcherAppGridCellHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = launcherAppGridLabelHeight(renderer, style, wrapWidth);
    return std::ceil(paddingY * 2.0F + iconSize + gap + labelHeight);
  }

  [[nodiscard]] float launcherAppGridCellHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = Style::fontSizeCaption * style.scale * 2.4F;
    return std::ceil(paddingY * 2.0F + iconSize + gap + labelHeight);
  }

  [[nodiscard]] LauncherListStyle
  launcherListStyleFrom(const ConfigService* config, float scale, float cardOpacity, bool browser) {
    LauncherListStyle style{.scale = scale, .appIconColorizeTint = std::nullopt, .listItemBackground = std::nullopt};
    if (config != nullptr) {
      const auto& launcher = config->config().shell.launcher;
      style.showIcons = launcher.showIcons;
      style.showAppOriginIndicator = launcher.showAppOriginIndicator;
      style.compact = launcher.compact;
      style.appIconColorizeTint = effectiveShellAppIconColorizationTint(config->config().shell);
      if (config->config().shell.panel.listItemBackground) {
        style.listItemBackground = colorSpecFromRole(ColorRole::SurfaceVariant, cardOpacity);
      } else if (browser) {
        style.listItemBackground = colorSpecFromRole(ColorRole::SurfaceVariant, cardOpacity * 0.45F);
      }
    }
    return style;
  }

  class LauncherResultRow final : public Node {
  public:
    LauncherResultRow(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float iconSize = launcherIconSize(m_style);
      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float paddingV = (m_style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * m_style.scale;
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = gap,
           .paddingV = paddingV,
           .paddingH = Style::spaceSm * m_style.scale,
           .radius = Style::scaledRadiusMd(m_style.scale)}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::label({
              .out = &m_badgeLabel,
              .fontSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::image({
              .out = &m_image,
              .width = iconSize,
              .height = iconSize,
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (!m_style.showIcons
            || m_badgeVisible
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textCol,
                  .align = FlexAlign::Start,
                  .gap = 0.0F,
                  .flexGrow = 1.0F,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * m_style.scale,
                  .fontWeight = FontWeight::SemiBold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              }),
              ui::label({
                  .out = &m_subtitle,
                  .fontSize = Style::fontSizeCaption * m_style.scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              })
          )
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));
      m_badgeVisible = !result.badge.empty();
      m_rowHeight = height;

      setSize(width, height);
      m_row->setFrameSize(width, height);

      m_badgeLabel->setVisible(false);
      m_badgeLabel->setParticipatesInLayout(false);
      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      const bool showAppIcon = m_style.showIcons && !m_badgeVisible;
      const bool showLeadingVisual = m_badgeVisible || showAppIcon;
      if (m_badgeVisible) {
        m_badgeLabel->setText(singleLinePreview(result.badge));
        m_badgeLabel->setSize(iconSize, iconSize);
        m_badgeLabel->setVisible(true);
        m_badgeLabel->setParticipatesInLayout(true);
        m_image->clear(renderer);
      } else if (showAppIcon) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0F;
      const float leadingWidth = showLeadingVisual ? iconSize + gap : 0.0F;
      const float pinnedWidth = result.pinned ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      m_pinnedGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setParticipatesInLayout(result.pinned);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setParticipatesInLayout(hasOrigin);
      const float originWidth = hasOrigin ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      const float textWidth = std::max(0.0F, width - leadingWidth - pinnedWidth - originWidth - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      const bool showSubtitle = !m_style.compact && !result.subtitle.empty();
      if (!showSubtitle) {
        m_subtitle->setVisible(false);
        m_subtitle->setText("");
      } else {
        m_subtitle->setVisible(true);
        m_subtitle->setText(singleLinePreview(result.subtitle));
        m_subtitle->setMaxWidth(textWidth);
      }

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_badgeVisible || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      m_image->setSize(launcherIconSize(m_style), launcherIconSize(m_style));
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_style.showIcons && !m_badgeVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_row->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_row->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_row->clearBorder();
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::OnSurface);
      const ColorSpec mutedForeground =
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant);
      m_badgeLabel->setColor(foreground);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_subtitle->setColor(mutedForeground);
      m_pinnedGlyph->setColor(mutedForeground);
      m_originGlyph->setColor(mutedForeground);
    }

    LauncherListStyle m_style{};
    float m_rowHeight = 0.0F;
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_row = nullptr;
    Label* m_badgeLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textCol = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_badgeVisible = false;
    bool m_reorderTarget = false;
  };

  class LauncherAppGridTile final : public Node {
  public:
    LauncherAppGridTile(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float gap = Style::spaceXs * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      auto col = ui::column({
          .out = &m_col,
          .align = FlexAlign::Center,
          .gap = gap,
          .paddingV = padding,
          .paddingH = padding,
          .radius = Style::scaledRadiusMd(m_style.scale),
          .fillWidth = true,
          .fillHeight = true,
      });
      addChild(std::move(col));

      addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      m_col->addChild(
          ui::image({
              .out = &m_image,
              .visible = false,
          })
      );

      m_col->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = launcherIconSize(m_style),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (!m_style.showIcons
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_col->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .fontWeight = FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));

      setSize(width, height);
      m_col->setSize(width, height);

      const float pinSize = Style::fontSizeBody * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      m_pinnedGlyph->setGlyphSize(pinSize);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, padding);
      m_pinnedGlyph->setFrameSize(pinSize, pinSize);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(pinSize);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, height - padding - pinSize);
      m_originGlyph->setFrameSize(pinSize, pinSize);

      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      if (m_style.showIcons) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        m_image->setSize(iconSize, iconSize);
        m_glyph->setGlyphSize(iconSize);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0F;
      const float textWidth = std::max(0.0F, width - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      const float iconSize = launcherIconSize(m_style);
      m_image->setSize(iconSize, iconSize);
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      m_col->setSize(width(), height());
      if (m_style.showIcons && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_col->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_col->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_col->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_col->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_col->clearBorder();
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::OnSurface);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_pinnedGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      m_originGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
    }

    LauncherListStyle m_style{};
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_col = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    Label* m_title = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_reorderTarget = false;
  };

  struct LauncherSectionRowData {
    enum class Kind : std::uint8_t { Header, Applications };

    Kind kind = Kind::Header;
    std::string label;
    std::string glyphName;
    std::array<std::size_t, kAppGridColumns> resultIndices{};
    std::size_t resultCount = 0;
  };

  class LauncherSectionRow final : public Node {
  public:
    using SelectCallback = std::function<void(std::size_t)>;
    using ActivateCallback = std::function<void(std::size_t)>;
    using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

    LauncherSectionRow(
        LauncherListStyle style, AsyncTextureCache* asyncTextures, SelectCallback onSelect, ActivateCallback onActivate,
        SecondaryActivateCallback onSecondaryActivate
    )
        : m_style(style), m_onSelect(std::move(onSelect)), m_onActivate(std::move(onActivate)),
          m_onSecondaryActivate(std::move(onSecondaryActivate)) {
      auto header = ui::row({
          .out = &m_header,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * m_style.scale,
          .paddingH = Style::spaceXs * m_style.scale,
      });
      header->addChild(
          ui::glyph({
              .out = &m_headerGlyph,
              .glyphSize = Style::fontSizeTitle * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      header->addChild(
          ui::label({
              .out = &m_headerLabel,
              .fontSize = Style::fontSizeBody * m_style.scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      );
      addChild(std::move(header));

      for (std::size_t slot = 0; slot < kAppGridColumns; ++slot) {
        auto area = std::make_unique<InputArea>();
        area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
        area->setOnEnter([this](const InputArea::PointerData&) { markLayoutDirty(); });
        area->setOnLeave([this]() { markLayoutDirty(); });
        area->setOnPress([this, slot](const InputArea::PointerData& data) {
          if (!m_boundResultIndices[slot].has_value()) {
            return;
          }
          const std::size_t resultIndex = *m_boundResultIndices[slot];
          if (data.button == BTN_LEFT) {
            if (data.pressed) {
              if (m_onSelect) {
                m_onSelect(resultIndex);
              }
            } else if (m_onActivate) {
              m_onActivate(resultIndex);
            }
          } else if (data.button == BTN_RIGHT && data.pressed) {
            if (m_onSelect) {
              m_onSelect(resultIndex);
            }
            if (m_onSecondaryActivate) {
              m_onSecondaryActivate(resultIndex, data.sceneX, data.sceneY);
            }
          }
        });
        area->setTooltipProvider([this, slot]() -> TooltipContent {
          if (m_results == nullptr || !m_boundResultIndices[slot].has_value()) {
            return {};
          }
          const std::size_t resultIndex = *m_boundResultIndices[slot];
          if (resultIndex >= m_results->size() || (*m_results)[resultIndex].origin.empty()) {
            return {};
          }
          return (*m_results)[resultIndex].origin;
        });

        auto tile = std::make_unique<LauncherAppGridTile>(m_style, asyncTextures);
        m_tiles[slot] = tile.get();
        area->addChild(std::move(tile));
        auto listRow = std::make_unique<LauncherResultRow>(m_style, asyncTextures);
        listRow->setVisible(false);
        m_listRows[slot] = listRow.get();
        area->addChild(std::move(listRow));
        m_areas[slot] = static_cast<InputArea*>(addChild(std::move(area)));
      }
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setGridView(bool gridView) { m_gridView = gridView; }

    void bind(
        const LauncherSectionRowData& row, const std::vector<LauncherResult>& results,
        std::optional<std::size_t> selectedResultIndex, float width, float height
    ) {
      m_results = &results;
      m_selectedResultIndex = selectedResultIndex;
      setSize(width, height);

      const bool headerVisible = row.kind == LauncherSectionRowData::Kind::Header;
      m_header->setVisible(headerVisible);
      m_header->setPosition(0.0F, 0.0F);
      m_header->setFrameSize(width, height);
      if (headerVisible) {
        m_headerGlyph->setGlyph(row.glyphName);
        m_headerGlyph->setVisible(!row.glyphName.empty());
        m_headerGlyph->setParticipatesInLayout(!row.glyphName.empty());
        m_headerLabel->setText(row.label);
        m_headerLabel->setMaxWidth(std::max(0.0F, width - Style::controlHeight * m_style.scale));
      }

      const std::size_t columns = m_gridView ? kAppGridColumns : 1;
      const float gap = m_gridView ? Style::spaceSm * m_style.scale : 0.0F;
      const auto columnCount = static_cast<float>(columns);
      const float cellWidth = std::max(0.0F, (width - static_cast<float>(columns - 1) * gap) / columnCount);
      for (std::size_t slot = 0; slot < kAppGridColumns; ++slot) {
        const bool active = !headerVisible && slot < row.resultCount && row.resultIndices[slot] < results.size();
        m_boundResultIndices[slot] = active ? std::optional<std::size_t>(row.resultIndices[slot]) : std::nullopt;
        m_areas[slot]->setVisible(active);
        m_tiles[slot]->setVisible(active && m_gridView);
        m_listRows[slot]->setVisible(active && !m_gridView);
        if (!active) {
          continue;
        }
        const std::size_t visualSlot = Style::rtl() ? columns - 1 - slot : slot;
        m_areas[slot]->setPosition(static_cast<float>(visualSlot) * (cellWidth + gap), 0.0F);
        m_areas[slot]->setFrameSize(cellWidth, height);
        m_tiles[slot]->setPosition(0.0F, 0.0F);
        m_tiles[slot]->setSize(cellWidth, height);
        m_listRows[slot]->setPosition(0.0F, 0.0F);
        m_listRows[slot]->setSize(cellWidth, height);
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_header->visible()) {
        m_header->layout(renderer);
        return;
      }
      if (m_results == nullptr) {
        return;
      }
      for (std::size_t slot = 0; slot < kAppGridColumns; ++slot) {
        if (!m_boundResultIndices[slot].has_value()) {
          continue;
        }
        const std::size_t resultIndex = *m_boundResultIndices[slot];
        if (resultIndex >= m_results->size()) {
          continue;
        }
        if (m_gridView) {
          m_tiles[slot]->setListStyle(m_style);
          m_tiles[slot]->bind(
              renderer, (*m_results)[resultIndex], m_areas[slot]->width(), m_areas[slot]->height(),
              m_selectedResultIndex == resultIndex, m_areas[slot]->hovered()
          );
        } else {
          m_listRows[slot]->setListStyle(m_style);
          m_listRows[slot]->bind(
              renderer, (*m_results)[resultIndex], m_areas[slot]->width(), m_areas[slot]->height(),
              m_selectedResultIndex == resultIndex, m_areas[slot]->hovered()
          );
        }
        m_areas[slot]->layout(renderer);
      }
    }

  private:
    LauncherListStyle m_style{};
    Flex* m_header = nullptr;
    Glyph* m_headerGlyph = nullptr;
    Label* m_headerLabel = nullptr;
    std::array<InputArea*, kAppGridColumns> m_areas{};
    std::array<LauncherAppGridTile*, kAppGridColumns> m_tiles{};
    std::array<LauncherResultRow*, kAppGridColumns> m_listRows{};
    std::array<std::optional<std::size_t>, kAppGridColumns> m_boundResultIndices{};
    const std::vector<LauncherResult>* m_results = nullptr;
    std::optional<std::size_t> m_selectedResultIndex;
    SelectCallback m_onSelect;
    ActivateCallback m_onActivate;
    SecondaryActivateCallback m_onSecondaryActivate;
    bool m_gridView = false;
  };

} // namespace

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherResultAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->setListStyle(m_style);
    row->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets>
  itemTooltipAnchorInsets(std::size_t index, float cellWidth, float cellHeight) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float pinnedWidth = (*m_results)[index].pinned ? glyphSize + gap : 0.0F;
    const float left = std::max(padding, cellWidth - padding - pinnedWidth - glyphSize);
    const float top = std::max(0.0F, (cellHeight - glyphSize) * 0.5F);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;

    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherAppGridAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherAppGridAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherAppGridTile>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* gridTile = static_cast<LauncherAppGridTile*>(&tile);
    gridTile->setListStyle(m_style);
    gridTile->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    gridTile->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets>
  itemTooltipAnchorInsets(std::size_t index, float cellWidth, float cellHeight) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float left = std::max(0.0F, cellWidth - padding - glyphSize);
    const float top = std::max(0.0F, cellHeight - padding - glyphSize);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;
    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherSectionsAdapter final : public VirtualListAdapter {
public:
  using SelectCallback = std::function<void(std::size_t)>;
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherSectionsAdapter(
      LauncherListStyle style, AsyncTextureCache* cache, SelectCallback onSelect, ActivateCallback onActivate,
      SecondaryActivateCallback onSecondaryActivate
  )
      : m_style(style), m_cache(cache), m_onSelect(std::move(onSelect)), m_onActivate(std::move(onActivate)),
        m_onSecondaryActivate(std::move(onSecondaryActivate)) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setSelectedResultIndex(std::optional<std::size_t> index) { m_selectedResultIndex = index; }
  [[nodiscard]] bool setGridView(bool gridView) {
    if (m_gridView == gridView) {
      return false;
    }
    m_gridView = gridView;
    return true;
  }
  void setApplicationRowHeight(float height) {
    const float next = std::max(1.0F, height);
    if (std::abs(m_applicationRowHeight - next) < 0.5F) {
      return;
    }
    m_applicationRowHeight = next;
    ++m_revision;
  }

  void rebuild(const std::vector<LauncherCategory>& categories, std::string otherLabel) {
    m_rows.clear();
    m_visualResultOrder.clear();
    m_resultPositions.assign(m_results == nullptr ? 0U : m_results->size(), std::nullopt);
    ++m_revision;
    if (m_results == nullptr) {
      return;
    }

    std::vector<bool> assigned(m_results->size(), false);
    const auto appendSection =
        [this, &assigned](std::string label, std::string glyphName, const std::vector<std::size_t>& resultIndices) {
          if (resultIndices.empty()) {
            return;
          }

          LauncherSectionRowData header;
          header.kind = LauncherSectionRowData::Kind::Header;
          header.label = std::move(label);
          header.glyphName = std::move(glyphName);
          m_rows.push_back(std::move(header));

          const std::size_t columns = m_gridView ? kAppGridColumns : 1;
          for (std::size_t offset = 0; offset < resultIndices.size(); offset += columns) {
            LauncherSectionRowData row;
            row.kind = LauncherSectionRowData::Kind::Applications;
            row.resultCount = std::min(columns, resultIndices.size() - offset);
            const std::size_t rowIndex = m_rows.size();
            for (std::size_t column = 0; column < row.resultCount; ++column) {
              const std::size_t resultIndex = resultIndices[offset + column];
              row.resultIndices[column] = resultIndex;
              assigned[resultIndex] = true;
              m_resultPositions[resultIndex] = ResultPosition{
                  .row = rowIndex,
                  .column = column,
                  .visualOrder = m_visualResultOrder.size(),
              };
              m_visualResultOrder.push_back(resultIndex);
            }
            m_rows.push_back(std::move(row));
          }
        };

    for (const LauncherCategory& category : categories) {
      std::vector<std::size_t> categoryResults;
      for (std::size_t i = 0; i < m_results->size(); ++i) {
        if (!assigned[i] && (*m_results)[i].category == category.label) {
          categoryResults.push_back(i);
        }
      }
      appendSection(category.label, category.glyphName, categoryResults);
    }

    std::vector<std::size_t> otherResults;
    for (std::size_t i = 0; i < assigned.size(); ++i) {
      if (!assigned[i]) {
        otherResults.push_back(i);
      }
    }
    appendSection(std::move(otherLabel), "apps", otherResults);
  }

  [[nodiscard]] std::optional<std::size_t> firstResultIndex() const {
    return m_visualResultOrder.empty() ? std::nullopt : std::optional<std::size_t>(m_visualResultOrder.front());
  }

  [[nodiscard]] std::optional<std::size_t> rowForResult(std::size_t resultIndex) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return std::nullopt;
    }
    return m_resultPositions[resultIndex]->row;
  }

  [[nodiscard]] std::optional<std::size_t> moveHorizontal(std::size_t resultIndex, int direction) const {
    if (!m_gridView) {
      return resultIndex < m_resultPositions.size() && m_resultPositions[resultIndex].has_value()
          ? std::optional<std::size_t>(resultIndex)
          : firstResultIndex();
    }
    if (resultIndex >= m_resultPositions.size()
        || !m_resultPositions[resultIndex].has_value()
        || m_visualResultOrder.empty()) {
      return firstResultIndex();
    }
    const std::size_t order = m_resultPositions[resultIndex]->visualOrder;
    if (direction < 0) {
      return order == 0 ? std::optional<std::size_t>(resultIndex)
                        : std::optional<std::size_t>(m_visualResultOrder[order - 1]);
    }
    return order + 1 >= m_visualResultOrder.size() ? std::optional<std::size_t>(resultIndex)
                                                   : std::optional<std::size_t>(m_visualResultOrder[order + 1]);
  }

  [[nodiscard]] std::optional<std::size_t>
  moveVertical(std::size_t resultIndex, int direction, std::size_t applicationRows = 1) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return firstResultIndex();
    }
    const ResultPosition position = *m_resultPositions[resultIndex];
    std::size_t rowIndex = position.row;
    std::size_t rowsRemaining = std::max<std::size_t>(1, applicationRows);
    while (true) {
      if (direction < 0) {
        if (rowIndex == 0) {
          return resultIndex;
        }
        --rowIndex;
      } else {
        if (rowIndex + 1 >= m_rows.size()) {
          return resultIndex;
        }
        ++rowIndex;
      }
      const auto& row = m_rows[rowIndex];
      if (row.kind != LauncherSectionRowData::Kind::Applications || row.resultCount == 0) {
        continue;
      }
      --rowsRemaining;
      if (rowsRemaining == 0) {
        return row.resultIndices[std::min(position.column, row.resultCount - 1)];
      }
    }
  }

  [[nodiscard]] std::size_t estimatedVisibleApplicationRows(float viewportHeight) const {
    const float rowHeight = std::max(1.0F, m_applicationRowHeight);
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(viewportHeight / rowHeight)));
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
  [[nodiscard]] std::uint64_t itemRevision(std::size_t /*index*/) const override { return m_revision; }
  [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return false; }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_rows.size()) {
      return 1.0F;
    }
    if (m_rows[index].kind == LauncherSectionRowData::Kind::Header) {
      const float textHeight =
          stableLabelHeight(renderer.measureFont(Style::fontSizeBody * m_style.scale, FontWeight::SemiBold));
      return std::ceil(std::max(Style::fontSizeTitle * m_style.scale, textHeight) + Style::spaceSm * m_style.scale);
    }

    (void)width;
    return m_applicationRowHeight;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<LauncherSectionRow>(m_style, m_cache, m_onSelect, m_onActivate, m_onSecondaryActivate);
  }

  void bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (m_results == nullptr || index >= m_rows.size()) {
      return;
    }
    auto* row = static_cast<LauncherSectionRow*>(&item);
    row->setListStyle(m_style);
    row->setGridView(m_gridView);
    row->bind(m_rows[index], *m_results, m_selectedResultIndex, width, item.height());
  }

private:
  struct ResultPosition {
    std::size_t row = 0;
    std::size_t column = 0;
    std::size_t visualOrder = 0;
  };

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  SelectCallback m_onSelect;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  const std::vector<LauncherResult>* m_results = nullptr;
  std::vector<LauncherSectionRowData> m_rows;
  std::vector<std::optional<ResultPosition>> m_resultPositions;
  std::vector<std::size_t> m_visualResultOrder;
  std::optional<std::size_t> m_selectedResultIndex;
  std::uint64_t m_revision = 0;
  float m_applicationRowHeight = 1.0F;
  bool m_gridView = false;
};

LauncherPanel::LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures)
    : m_iconResolver(true), m_config(config), m_accounts(accounts), m_asyncTextures(asyncTextures) {}

LauncherPanel::~LauncherPanel() = default;

bool LauncherPanel::usesBrowserLayout() const { return m_scopedProviderId.empty(); }

float LauncherPanel::preferredWidth() const {
  const float browserWidth = m_config != nullptr ? static_cast<float>(m_config->config().shell.launcher.width)
                                                 : static_cast<float>(ShellConfig::LauncherConfig::kDefaultWidth);
  return scaled(usesBrowserLayout() ? browserWidth : 560.0F);
}

float LauncherPanel::preferredHeight() const { return scaled(usesBrowserLayout() ? kLauncherHeight : 500.0F); }

PanelPlacement LauncherPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.launcherPlacement : PanelPlacement::Floating;
}

void LauncherPanel::applyProviderConfig(LauncherProvider& provider) const {
  std::string triggerWord = std::string(provider.defaultPrefix());
  std::string prefix = "/";
  std::optional<bool> global;
  if (m_config != nullptr) {
    const auto& launcherCfg = m_config->config().shell.launcher;
    prefix = launcherCfg.providerPrefix;
    if (provider.allowCustomPrefix()) {
      const std::string key = StringUtils::toLower(std::string(provider.id()));
      auto it = std::ranges::find(launcherCfg.providers, key, &LauncherProviderConfig::name);
      if (it != launcherCfg.providers.end()) {
        if (!it->prefix.empty()) {
          triggerWord = it->prefix;
        }
        global = it->global;
      }
    }
  }

  if (provider.allowCustomPrefix()) {
    provider.setCustomPrefix(triggerWord.empty() ? std::string() : prefix + triggerWord);
    provider.setCustomIncludeInGlobalSearch(global);
  }
}

void LauncherPanel::finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied) {
  if (provider.trackUsage()) {
    m_usageTracker.record(provider.id(), resultId);
  }
  PanelManager::instance().closePanel(false);
  if (copied && provider.supportsAutoPaste() && m_onCopiedActivation) {
    m_onCopiedActivation();
  }
}

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  applyProviderConfig(*provider);
  provider->initialize();
  provider->setResultsChangedCallback([this]() { onProviderResultsChanged(); });
  provider->setQueryRequestedCallback([this](std::string query) { setQuery(std::move(query)); });
  LauncherProvider* providerPtr = provider.get();
  provider->setActivationDoneCallback([this, providerPtr](const std::string& resultId, bool copied) {
    finishActivation(*providerPtr, resultId, copied);
  });
  m_providers.push_back(std::move(provider));
}

void LauncherPanel::clearDynamicProviders() {
  std::erase_if(m_providers, [](const std::unique_ptr<LauncherProvider>& provider) { return provider->isDynamic(); });
}

void LauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const std::unique_ptr<LauncherProvider>& provider) {
    return provider->id().starts_with(prefix);
  });
}

void LauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
}

void LauncherPanel::create() {
  m_launcherRowHeight = 0.0F;
  m_browserMode = usesBrowserLayout();
  const float scale = contentScale();

  auto input = ui::input({
      .out = &m_input,
      .placeholder = m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder,
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceMd * scale,
      .clearButtonEnabled = true,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](const std::string& text) {
            if (m_launcherProviderViewId == kSessionProviderId) {
              m_sessionReturnView.reset();
            }
            m_launcherProviderViewId.clear();
            onInputChanged(text);
            if (m_input == nullptr) {
              return;
            }
            const std::string preview = singleLinePreview(text);
            if (preview != text) {
              m_input->setValue(preview);
            }
          },
      .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
  });
  if (m_input != nullptr && m_input->inputArea() != nullptr) {
    m_input->inputArea()->setTabFocusKey("launcher.search");
  }

  auto categoryFilter = ui::segmented({
      .out = &m_categoryFilter,
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .equalSegmentWidths = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
  });

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  if (m_browserMode) {
    body->setPadding(Style::spaceSm * scale);
    body->setGap(Style::spaceSm * scale);
    body->setCardStyle(scale, panelCardOpacity());
    auto navigationRow = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    navigationRow->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceXs * scale,
            },
            ui::button({
                .out = &m_launcherUsageButton,
                .glyph = m_config != nullptr && m_config->config().shell.launcher.sortByUsage
                    ? "sort-ascending-letters"
                    : "sort-descending-small-big",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .surfaceOpacity = panelCardOpacity(),
                .tooltip = i18n::tr(
                    m_config != nullptr && m_config->config().shell.launcher.sortByUsage
                        ? "launcher.browser.options.use-default-order"
                        : "launcher.browser.options.sort-by-frequency"
                ),
                .radius = Style::scaledRadiusMd(scale),
                .width = Style::controlHeightSm * scale,
                .onClick =
                    [this]() {
                      if (m_config == nullptr) {
                        return;
                      }
                      const bool enabled = !m_config->config().shell.launcher.sortByUsage;
                      (void)m_config->setOverride({"shell", "launcher", "sort_by_usage"}, enabled);
                    },
                .configure =
                    [scale](Button& button) {
                      button.setControlHeight(Style::controlHeightSm * scale);
                      button.inputArea()->setTabFocusKey("launcher.sort-by-usage");
                    },
            }),
            ui::button({
                .out = &m_launcherGridButton,
                .glyph = m_config != nullptr && m_config->config().shell.launcher.appGrid ? "list" : "layout-grid",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .surfaceOpacity = panelCardOpacity(),
                .tooltip = i18n::tr(
                    m_config != nullptr && m_config->config().shell.launcher.appGrid
                        ? "launcher.browser.options.show-list"
                        : "launcher.browser.options.show-grid"
                ),
                .radius = Style::scaledRadiusMd(scale),
                .width = Style::controlHeightSm * scale,
                .onClick =
                    [this]() {
                      if (m_config == nullptr) {
                        return;
                      }
                      const bool enabled = !m_config->config().shell.launcher.appGrid;
                      (void)m_config->setOverride({"shell", "launcher", "app_grid"}, enabled);
                    },
                .configure =
                    [scale](Button& button) {
                      button.setControlHeight(Style::controlHeightSm * scale);
                      button.inputArea()->setTabFocusKey("launcher.grid-view");
                    },
            })
        )
    );
    navigationRow->addChild(
        ui::segmented({
            .out = &m_launcherNavigation,
            .scale = scale,
            .compact = true,
            .surfaceOpacity = panelCardOpacity(),
            .equalSegmentWidths = false,
        })
    );
    if (m_launcherNavigation != nullptr && m_launcherNavigation->focusArea() != nullptr) {
      m_launcherNavigation->focusArea()->setTabFocusKey("launcher.navigation");
    }
    body->addChild(std::move(navigationRow));
    body->addChild(std::move(categoryFilter));
  }

  auto resultsViewport = ui::column({
      .out = &m_resultsViewport,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  const LauncherListStyle initialStyle = launcherListStyleFrom(m_config, scale, panelCardOpacity(), m_browserMode);
  m_listAdapter = std::make_unique<LauncherResultAdapter>(initialStyle, m_asyncTextures);
  m_gridAdapter = std::make_unique<LauncherAppGridAdapter>(initialStyle, m_asyncTextures);
  m_listAdapter->setResults(&m_results);
  m_gridAdapter->setResults(&m_results);
  const auto onActivate = [this](std::size_t index) { activateAt(index); };
  const auto onSecondaryActivate = [this](std::size_t index, float ax, float ay) {
    (void)openAppActionsMenu(index, ax, ay);
  };
  m_launcherSectionsAdapter = std::make_unique<LauncherSectionsAdapter>(
      initialStyle, m_asyncTextures, [this](std::size_t index) { selectResult(index, false); }, onActivate,
      onSecondaryActivate
  );
  m_launcherSectionsAdapter->setResults(&m_results);
  m_listAdapter->setOnActivate(onActivate);
  m_listAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_listAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });
  m_gridAdapter->setOnActivate(onActivate);
  m_gridAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_gridAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });

  resultsViewport->addChild(
      ui::virtualListView({
          .out = &m_launcherSections,
          .contentScale = scale,
          .itemGap = Style::spaceXs * scale,
          .overscanItems = kRowOverscan,
          .adapter = m_launcherSectionsAdapter.get(),
          .flexGrow = 1.0F,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](VirtualListView& list) {
            list.setFillWidth(true);
            list.scrollView().setReserveScrollbarGutter(true);
          },
      })
  );

  resultsViewport->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeightEstimate(initialStyle),
          .squareCells = false,
          .columnGap = 0.0F,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kRowOverscan,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure =
              [browser = m_browserMode](VirtualGridView& grid) {
                grid.setFillWidth(true);
                grid.scrollView().setReserveScrollbarGutter(browser);
              },
      })
  );

  auto detailScroll = ui::scrollView({
      .out = &m_detailScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceSm * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity);
      },
  });
  auto* detailContent = detailScroll->content();
  detailContent->setDirection(FlexDirection::Vertical);
  detailContent->setAlign(FlexAlign::Stretch);
  detailContent->setGap(Style::spaceSm * scale);
  detailContent->addChild(
      ui::label({
          .out = &m_detailSubtitle,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  detailContent->addChild(
      ui::label({
          .out = &m_detailBody,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 0,
          .flexGrow = 1.0F,
      })
  );
  resultsViewport->addChild(std::move(detailScroll));

  resultsViewport->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
      })
  );
  body->addChild(std::move(resultsViewport));

  if (m_browserMode) {
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .fillHeight = true,
    });

    auto toolbar = ui::row({
        .out = &m_launcherToolbar,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceSm * scale,
        .fillWidth = true,
        .configure = [scale, opacity = panelCardOpacity()](Flex& flex) { flex.setCardStyle(scale, opacity); },
    });
    auto profile = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .width = 180.0F * scale,
    });
    profile->addChild(
        ui::image({
            .out = &m_launcherAvatar,
            .fit = ImageFit::Cover,
            .radius = kLauncherAvatarSize * 0.5F * scale,
            .width = kLauncherAvatarSize * scale,
            .height = kLauncherAvatarSize * scale,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    profile->addChild(
        ui::glyph({
            .out = &m_launcherAvatarFallback,
            .glyph = "user-circle",
            .glyphSize = 38.0F * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    );
    m_launcherAvatar->setAsyncReadyCallback([this]() {
      if (m_launcherAvatar == nullptr || m_launcherAvatarFallback == nullptr || !m_launcherAvatar->hasImage()) {
        return;
      }
      m_launcherAvatar->setVisible(true);
      m_launcherAvatar->setParticipatesInLayout(true);
      m_launcherAvatarFallback->setVisible(false);
      m_launcherAvatarFallback->setParticipatesInLayout(false);
      PanelManager::instance().requestLayout();
      PanelManager::instance().requestRedraw();
    });
    profile->addChild(
        ui::column(
            {
                .align = FlexAlign::Start,
                .gap = 0.0F,
                .width = 120.0F * scale,
            },
            ui::label({
                .text = sessionDisplayName(),
                .fontSize = Style::fontSizeBody * scale,
                .fontWeight = FontWeight::SemiBold,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            }),
            ui::label({
                .text = distroLabel(),
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            })
        )
    );
    toolbar->addChild(std::move(profile));
    const bool showSettingsButton = m_config == nullptr || m_config->config().shell.launcher.showSettingsButton;
    const bool showSessionButton = m_config == nullptr || m_config->config().shell.launcher.showSessionButton;
    const bool searchOnTop = m_config != nullptr
        && m_config->config().shell.launcher.searchPosition == ShellConfig::LauncherSearchPosition::Top;
    toolbar->addChild(
        ui::separator({
            .spacing = Style::spaceXs * scale,
            .orientation = SeparatorOrientation::VerticalRule,
            .gradientEdges = false,
        })
    );
    input->setFlexGrow(1.0F);
    toolbar->addChild(std::move(input));
    if (showSettingsButton) {
      toolbar->addChild(
          ui::button({
              .out = &m_launcherSettingsButton,
              .glyph = "settings",
              .glyphSize = Style::fontSizeTitle * scale,
              .variant = ButtonVariant::Ghost,
              .surfaceOpacity = panelCardOpacity(),
              .tooltip = i18n::tr("launcher.browser.actions.settings"),
              .radius = Style::scaledRadiusMd(scale),
              .width = Style::controlHeight * scale,
              .onClick = []() { PanelManager::instance().openSettingsWindow(); },
              .configure =
                  [scale](Button& button) {
                    button.setControlHeight(Style::controlHeight * scale);
                    button.inputArea()->setTabFocusKey("launcher.settings");
                  },
          })
      );
    }
    if (showSessionButton) {
      toolbar->addChild(
          ui::button({
              .out = &m_launcherPowerButton,
              .glyph = "shutdown",
              .glyphSize = Style::fontSizeTitle * scale,
              .variant = ButtonVariant::Ghost,
              .surfaceOpacity = panelCardOpacity(),
              .tooltip = i18n::tr("launcher.browser.actions.power"),
              .radius = Style::scaledRadiusMd(scale),
              .width = Style::controlHeight * scale,
              .onClick = [this]() { showSessionActions(); },
              .configure =
                  [scale](Button& button) {
                    button.setControlHeight(Style::controlHeight * scale);
                    button.inputArea()->setTabFocusKey("launcher.power");
                  },
          })
      );
    }
    if (searchOnTop) {
      container->addChild(std::move(toolbar));
      container->addChild(std::move(body));
    } else {
      container->addChild(std::move(body));
      container->addChild(std::move(toolbar));
    }
    setRoot(std::move(container));
  } else {
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    container->addChild(std::move(input));
    container->addChild(std::move(categoryFilter));
    container->addChild(std::move(body));
    setRoot(std::move(container));
  }

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() { refreshLauncherAppIconColorization(); });

  syncLauncherListStyle();
}

void LauncherPanel::refreshLauncherAppIconColorization() {
  if (m_listAdapter == nullptr || m_gridAdapter == nullptr || m_grid == nullptr) {
    return;
  }
  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale(), panelCardOpacity(), m_browserMode);
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setListStyle(style);
  }
  m_grid->notifyDataChanged();
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

bool LauncherPanel::shouldUseAppGrid() const {
  if (!m_launcherShowIcons || m_config == nullptr || !m_config->config().shell.launcher.appGrid) {
    return false;
  }
  if (m_results.empty()) {
    return false;
  }
  if (m_showingProviderOverview) {
    return true;
  }
  if (m_launcherProviderViewId == kSessionProviderId) {
    return true;
  }
  if (m_browserMode) {
    if (!m_launcherProviderViewId.empty()) {
      return false;
    }
    const bool targetsProvider = std::ranges::any_of(m_providers, [this](const auto& provider) {
      const std::string_view prefix = provider->prefix();
      return !prefix.empty() && std::string_view(m_query).starts_with(prefix);
    });
    return !targetsProvider;
  }
  return std::ranges::all_of(m_results, [](const LauncherResult& result) {
    return result.providerId == kApplicationsProviderId;
  });
}

void LauncherPanel::queueResultsTransition(ResultsTransition transition) {
  if (!m_browserMode || transition == ResultsTransition::None) {
    return;
  }
  m_pendingResultsTransition = transition;
}

void LauncherPanel::animateResultsTransition() {
  const ResultsTransition transition = std::exchange(m_pendingResultsTransition, ResultsTransition::None);
  if (transition == ResultsTransition::None || m_resultsViewport == nullptr) {
    return;
  }

  AnimationManager* animations = m_resultsViewport->animationManager();
  if (animations == nullptr) {
    m_resultsViewport->setOpacity(1.0F);
    m_resultsViewport->setScale(1.0F);
    m_resultsTransitionActive = false;
    return;
  }

  animations->cancelForOwner(m_resultsViewport);
  if (m_resultsTransitionActive) {
    m_resultsViewport->setPosition(m_resultsTransitionBaseX, m_resultsViewport->y());
  }
  constexpr float startOpacity = 0.78F;
  const auto duration = static_cast<float>(Style::animFast);
  Flex* const viewport = m_resultsViewport;
  const float visualDirection = transition == ResultsTransition::NavigationForward ? 1.0F : -1.0F;
  const float direction = Style::rtl() ? -visualDirection : visualDirection;
  const float travel = std::min(viewport->width() * 0.12F, 72.0F * contentScale());
  m_resultsTransitionBaseX = viewport->x();
  m_resultsTransitionActive = true;
  viewport->setOpacity(startOpacity);
  viewport->setScale(1.0F);
  viewport->setPosition(m_resultsTransitionBaseX + direction * travel, viewport->y());
  animations->animate(
      0.0F, 1.0F, duration, Easing::EaseOutCubic,
      [this, viewport, startOpacity, direction, travel](float progress) {
        viewport->setOpacity(startOpacity + (1.0F - startOpacity) * progress);
        viewport->setPosition(m_resultsTransitionBaseX + direction * travel * (1.0F - progress), viewport->y());
      },
      [this, viewport]() {
        viewport->setOpacity(1.0F);
        viewport->setScale(1.0F);
        viewport->setPosition(m_resultsTransitionBaseX, viewport->y());
        m_resultsTransitionActive = false;
      },
      viewport
  );
}

bool LauncherPanel::shouldUseLauncherSections() const { return false; }

void LauncherPanel::rebuildLauncherSections() {
  if (m_launcherSectionsAdapter == nullptr || m_launcherSections == nullptr) {
    return;
  }
  if (shouldUseLauncherSections()) {
    (void)m_launcherSectionsAdapter->setGridView(
        m_config != nullptr && m_config->config().shell.launcher.appGrid && m_launcherShowIcons
    );
    m_launcherSectionsAdapter->rebuild(m_currentCategories, i18n::tr("launcher.browser.sections.other-apps"));
    m_launcherSectionsAdapter->setSelectedResultIndex(m_selectedIndex);
  } else {
    m_launcherSectionsAdapter->rebuild({}, "");
    m_launcherSectionsAdapter->setSelectedResultIndex(std::nullopt);
  }
  m_launcherSections->notifyDataChanged();
}

void LauncherPanel::syncLauncherViewLayout(Renderer* renderer) {
  if (m_grid == nullptr || m_listAdapter == nullptr || m_gridAdapter == nullptr) {
    return;
  }

  const bool useGrid = shouldUseAppGrid();
  const float scale = contentScale();
  const LauncherListStyle style = launcherListStyleFrom(m_config, scale, panelCardOpacity(), m_browserMode);
  m_grid->setScale(scale);
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setListStyle(style);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->setContentScale(scale);
  }
  const bool reorderEnabled = m_query.empty() && m_scopedProviderId.empty() && m_activeCategoryType == Pinned;
  const bool listReorderEnabledChanged = m_listAdapter->setReorderEnabled(reorderEnabled);
  const bool gridReorderEnabledChanged = m_gridAdapter->setReorderEnabled(reorderEnabled);
  const bool reorderEnabledChanged = listReorderEnabledChanged || gridReorderEnabledChanged;
  if (renderer != nullptr) {
    m_listAdapter->setRenderer(renderer);
    m_gridAdapter->setRenderer(renderer);
  }

  const bool modeChanged = useGrid != m_usingAppGrid;
  m_usingAppGrid = useGrid;
  if (modeChanged) {
    m_launcherRowHeight = 0.0F;
  }

  if (useGrid) {
    m_grid->setColumns(kAppGridColumns);
    m_grid->setSquareCells(false);
    m_grid->setColumnGap(Style::spaceSm * scale);
    m_grid->setRowGap(Style::spaceSm * scale);
    m_grid->setCellHeight(m_launcherRowHeight > 0.0F ? m_launcherRowHeight : launcherAppGridCellHeightEstimate(style));
    if (modeChanged) {
      m_grid->setAdapter(m_gridAdapter.get());
    }
  } else {
    m_grid->setColumns(1);
    m_grid->setColumnGap(0.0F);
    m_grid->setRowGap(Style::spaceXs * scale);
    const float listCellHeight = m_launcherRowHeight > 0.0F ? m_launcherRowHeight
        : renderer != nullptr                               ? launcherRowHeight(*renderer, style)
                                                            : launcherRowHeightEstimate(style);
    m_grid->setCellHeight(listCellHeight);
    if (modeChanged) {
      m_grid->setAdapter(m_listAdapter.get());
    }
  }

  if (modeChanged || reorderEnabledChanged) {
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::syncLauncherListStyle() {
  const bool showIcons = m_config == nullptr || m_config->config().shell.launcher.showIcons;
  const bool showAppOriginIndicator = m_config == nullptr || m_config->config().shell.launcher.showAppOriginIndicator;
  const bool compact = m_config != nullptr && m_config->config().shell.launcher.compact;
  const bool appGrid = m_config != nullptr && m_config->config().shell.launcher.appGrid;
  if (showIcons == m_launcherShowIcons
      && showAppOriginIndicator == m_launcherShowAppOriginIndicator
      && compact == m_launcherCompact
      && appGrid == m_launcherAppGrid
      && m_listAdapter != nullptr) {
    return;
  }
  m_launcherShowIcons = showIcons;
  m_launcherShowAppOriginIndicator = showAppOriginIndicator;
  m_launcherCompact = compact;
  m_launcherAppGrid = appGrid;
  m_launcherRowHeight = 0.0F;
  syncLauncherViewLayout(nullptr);
  rebuildLauncherSections();
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::updateLauncherGridMetrics(Renderer& renderer) {
  if (m_grid == nullptr) {
    return;
  }

  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale(), panelCardOpacity(), m_browserMode);
  float cellHeight = launcherRowHeight(renderer, style);
  if (m_usingAppGrid) {
    float wrapWidth = 0.0F;
    constexpr std::size_t columns = kAppGridColumns;
    const float viewportW = shouldUseLauncherSections() && m_launcherSections != nullptr
        ? m_launcherSections->scrollView().contentViewportWidth()
        : m_grid->scrollView().contentViewportWidth();
    const float gap = Style::spaceSm * contentScale();
    const float cellW = (viewportW - static_cast<float>(columns - 1) * gap) / static_cast<float>(columns);
    const float paddingH = Style::spaceSm * contentScale() * 2.0F;
    wrapWidth = std::max(0.0F, cellW - paddingH);
    cellHeight = launcherAppGridCellHeight(renderer, style, wrapWidth);
  }
  if (std::abs(cellHeight - m_launcherRowHeight) < 0.5F) {
    return;
  }

  m_launcherRowHeight = cellHeight;
  m_grid->setCellHeight(cellHeight);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setApplicationRowHeight(cellHeight);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->setSurfaceOpacity(opacity);
  }
  if (m_launcherNavigation != nullptr) {
    m_launcherNavigation->setSurfaceOpacity(opacity);
  }
  if (m_launcherToolbar != nullptr) {
    m_launcherToolbar->setCardStyle(contentScale(), opacity);
  }
  if (m_browserMode && m_body != nullptr) {
    m_body->setCardStyle(contentScale(), opacity);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
  for (Button* button : {
           m_launcherUsageButton,
           m_launcherGridButton,
           m_launcherSettingsButton,
           m_launcherPowerButton,
       }) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setCardStyle(contentScale(), opacity);
  }
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  if (m_categoryFilterRebuildPending) {
    rebuildCategoryFilter();
  }
  updateLauncherOptionButtons();
  syncLauncherListStyle();
  syncLauncherViewLayout(&renderer);

  m_container->setSize(width, height);
  m_container->layout(renderer);
  const float previousRowHeight = m_launcherRowHeight;
  updateLauncherGridMetrics(renderer);
  if (std::abs(previousRowHeight - m_launcherRowHeight) >= 0.5F) {
    m_container->layout(renderer);
  }
}

void LauncherPanel::doUpdate(Renderer& renderer) {
  if (m_browserMode) {
    syncLauncherAvatar(renderer);
  }
}

void LauncherPanel::syncLauncherAvatar(Renderer& renderer) {
  if (m_launcherAvatar == nullptr || m_launcherAvatarFallback == nullptr || m_config == nullptr) {
    return;
  }

  const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
  if (displayPath.empty()) {
    m_launcherAvatarPath.clear();
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
    return;
  }

  if (displayPath != m_launcherAvatarPath) {
    m_launcherAvatarPath = displayPath;
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
  }

  const int targetSize = static_cast<int>(std::round(kLauncherAvatarSize * contentScale()));
  const bool ready = m_asyncTextures != nullptr
      ? m_launcherAvatar->setSourceFileAsync(renderer, *m_asyncTextures, displayPath, targetSize, true)
      : m_launcherAvatar->setSourceFile(renderer, displayPath, targetSize, true);
  if (ready) {
    m_launcherAvatar->setVisible(true);
    m_launcherAvatar->setParticipatesInLayout(true);
    m_launcherAvatarFallback->setVisible(false);
    m_launcherAvatarFallback->setParticipatesInLayout(false);
  }
}

void LauncherPanel::showProviderOverview() {
  const std::string prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
  if (!prefix.empty()) {
    queueResultsTransition(ResultsTransition::NavigationForward);
    setQuery(prefix);
  }
}

void LauncherPanel::showSessionActions() {
  if (m_launcherProviderViewId == kSessionProviderId && m_sessionReturnView.has_value()) {
    BrowserViewState previous = std::move(*m_sessionReturnView);
    m_sessionReturnView.reset();
    m_activeCategoryType = previous.categoryType;
    m_activeCategorySlotIndex = previous.categorySlotIndex;
    m_activeCategory = std::move(previous.category);
    m_launcherProviderViewId = std::move(previous.providerViewId);
    queueResultsTransition(ResultsTransition::NavigationBackward);
    if (m_input != nullptr) {
      m_input->setValue(singleLinePreview(previous.query));
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0F);
    }
    if (m_launcherSections != nullptr) {
      m_launcherSections->scrollView().setScrollOffset(0.0F);
    }
    onInputChanged(previous.query);
    return;
  }

  const auto provider =
      std::ranges::find(m_providers, kSessionProviderId, [](const auto& candidate) { return candidate->id(); });
  if (provider == m_providers.end() || (*provider)->prefix().empty()) {
    return;
  }
  m_sessionReturnView = BrowserViewState{
      .categoryType = m_activeCategoryType,
      .categorySlotIndex = m_activeCategorySlotIndex,
      .category = m_activeCategory,
      .query = m_query,
      .providerViewId = m_launcherProviderViewId,
  };
  queueResultsTransition(ResultsTransition::NavigationForward);
  m_launcherProviderViewId = std::string((*provider)->id());
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged("");
}

void LauncherPanel::onOpen(std::string_view context) {
  for (auto& provider : m_providers) {
    applyProviderConfig(*provider);
  }

  // Pick up apps installed since the last scan (notably Nix profile swaps that
  // inotify cannot observe). Cheap stat-only check; only rescans on real change.
  refreshDesktopEntriesIfSourcesChanged();

  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = true;
  m_applyDefaultViewPending = m_browserMode;
  m_showingProviderOverview = false;
  m_launcherProviderViewId.clear();
  m_sessionReturnView.reset();

  const std::string initialValue(context);
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
    m_input->setValue(singleLinePreview(initialValue));
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(initialValue);
}

void LauncherPanel::onClose() {
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }

  for (auto& provider : m_providers) {
    provider->reset();
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = false;
  m_applyDefaultViewPending = false;
  m_syncingLauncherNavigation = false;
  m_showingProviderOverview = false;
  m_launcherProviderViewId.clear();
  m_sessionReturnView.reset();
  m_pendingResultsTransition = ResultsTransition::None;
  m_resultsTransitionActive = false;
  m_resultsTransitionBaseX = 0.0F;
  m_selectedIndex = 0;
  m_usingAppGrid = false;
  m_launcherRowHeight = 0.0F;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gridAdapter.reset();
  m_launcherSectionsAdapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_categoryFilter = nullptr;
  m_body = nullptr;
  m_resultsViewport = nullptr;
  m_launcherToolbar = nullptr;
  m_launcherNavigation = nullptr;
  m_launcherAvatar = nullptr;
  m_launcherAvatarFallback = nullptr;
  m_launcherSettingsButton = nullptr;
  m_launcherUsageButton = nullptr;
  m_launcherGridButton = nullptr;
  m_launcherPowerButton = nullptr;
  m_grid = nullptr;
  m_launcherSections = nullptr;
  m_detailScroll = nullptr;
  m_detailSubtitle = nullptr;
  m_detailBody = nullptr;
  m_emptyLabel = nullptr;
  m_launcherAvatarPath.clear();
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() { reapplyCurrentQuery(); }

void LauncherPanel::clearUsage() {
  m_usageTracker.clear();
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::syncUsageOrderingState() {
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::reapplyCurrentQuery() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerId;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerId == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

void LauncherPanel::setQuery(std::string query) {
  if (m_input == nullptr) {
    return;
  }
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_sessionReturnView.reset();
  }
  m_launcherProviderViewId.clear();
  m_input->setValue(singleLinePreview(query));
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(query);
}

void LauncherPanel::onProviderResultsChanged() {
  // Only re-gather while the panel is open and built; after onClose the scene
  // nodes are gone and a refresh would touch null grid/label pointers.
  if (m_input == nullptr) {
    return;
  }
  reapplyCurrentQuery();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool LauncherPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr) {
    const bool onInput = (m_input != nullptr && focused == m_input->inputArea());
    const bool inResults = (m_grid != nullptr && isDescendantOf(focused, m_grid))
        || (m_launcherSections != nullptr && isDescendantOf(focused, m_launcherSections));
    if (!onInput && !inResults) {
      return false;
    }
  }

  if (m_categoryFilter != nullptr && m_categoryFilter->visible()) {
    if (focused == m_categoryFilter->focusArea()) {
      return false;
    }
  }

  return handleKeyEvent(sym, modifiers);
}

void LauncherPanel::onInputChanged(const std::string& text) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    m_iconResolver.invalidateMissingCache();
    m_desktopEntriesVersion = desktopVersion;
  }

  m_query = text;
  m_showingProviderOverview = false;
  if (m_browserMode && !text.empty() && m_activeCategoryType != All) {
    m_activeCategoryType = All;
    const auto allSlot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
    m_activeCategorySlotIndex =
        allSlot == m_categoryFilterSlots.end() ? 0 : static_cast<std::size_t>(allSlot - m_categoryFilterSlots.begin());
    m_activeCategory.clear();
  }
  m_allResults.clear();

  std::vector<LauncherCategory> newCategories;
  bool hasRecentlyUsed = false;
  bool anyProviderLoading = false;

  if (!m_scopedProviderId.empty()) {
    for (auto& provider : m_providers) {
      if (provider->id() != m_scopedProviderId) {
        continue;
      }
      m_allResults = provider->query(text);
      anyProviderLoading = provider->isLoading();
      for (auto& result : m_allResults) {
        result.providerId = provider->id();
      }
      sortResultsByScore(m_allResults);
      break;
    }
  } else {
    // Route query to providers
    LauncherProvider* activeProvider = nullptr;
    std::string_view queryText = text;

    if (!m_launcherProviderViewId.empty()) {
      const auto provider =
          std::ranges::find(m_providers, std::string_view(m_launcherProviderViewId), [](const auto& candidate) {
            return candidate->id();
          });
      if (provider != m_providers.end()) {
        activeProvider = provider->get();
      } else {
        m_launcherProviderViewId.clear();
      }
    }

    if (activeProvider == nullptr) {
      // Check for prefix match (longest first)
      for (auto& provider : m_providers) {
        auto prefix = provider->prefix();
        if (prefix.empty()) {
          continue;
        }
        if (text.size() >= prefix.size()
            && std::string_view(text).starts_with(prefix)
            && (activeProvider == nullptr || prefix.size() > activeProvider->prefix().size())) {
          activeProvider = provider.get();
          queryText = std::string_view(text).substr(prefix.size());
        }
      }
    }
    // Trim leading space after prefix
    if (activeProvider != nullptr && !queryText.empty() && queryText.front() == ' ') {
      queryText = queryText.substr(1);
    }

    const bool typedQuery = !queryText.empty();
    const bool sortByUsage = m_config != nullptr && m_config->config().shell.launcher.sortByUsage;

    auto applyUsageMetadata = [&](std::vector<LauncherResult>& results, const LauncherProvider& provider) {
      for (auto& result : results) {
        if (sortByUsage) {
          const int usageCount = m_usageTracker.getCount(provider.id(), result.id);
          result.score += usageBoostForScore(result.score, usageCount, typedQuery);
        }
        result.recentlyUsedIndex = m_usageTracker.getRecentlyUsedIndex(provider.id(), result.id);
      }
    };

    if (activeProvider != nullptr) {
      m_allResults = activeProvider->queryPrefixed(queryText);
      anyProviderLoading = activeProvider->isLoading();
      if (activeProvider->trackUsage()) {
        applyUsageMetadata(m_allResults, *activeProvider);
        if (m_usageTracker.getRecentlyUsedCount(activeProvider->id()) > 0) {
          hasRecentlyUsed = true;
        }
      }
      for (auto& result : m_allResults) {
        result.providerId = activeProvider->id();
      }
      sortResultsByScore(m_allResults);
      newCategories = activeProvider->categories();
    } else if (startsWithLauncherPrefix(text)) {
      m_allResults = providerOverviewResults(text);
      m_showingProviderOverview = true;
    } else {
      // Query default providers (empty prefix), plus prefixed providers that opt into global search.
      // Prefixed opt-in providers (e.g. Session) only contribute once the query is long enough,
      // so opening the launcher with no/short input does not flood it with their entries.
      const bool allowGlobalOptIn =
          StringUtils::trimRightView(StringUtils::trimLeftView(queryText)).size() >= kGlobalOptInMinChars;
      const bool browserAppBrowse = m_browserMode && text.empty();
      for (auto& provider : m_providers) {
        if (browserAppBrowse && provider->id() != kApplicationsProviderId) {
          continue;
        }
        const bool isDefault = provider->prefix().empty();
        if (!isDefault && (!provider->includeInGlobalSearch() || !allowGlobalOptIn)) {
          continue;
        }
        auto results = provider->query(queryText);
        if (provider->isLoading()) {
          anyProviderLoading = true;
        }
        if (provider->trackUsage()) {
          applyUsageMetadata(results, *provider);
          if (m_usageTracker.getRecentlyUsedCount(provider->id()) > 0) {
            hasRecentlyUsed = true;
          }
        }
        for (auto& result : results) {
          result.providerId = provider->id();
        }
        m_allResults.insert(
            m_allResults.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
        );
        auto providerCats = provider->categories();
        for (auto& cat : providerCats) {
          newCategories.push_back(std::move(cat));
        }
      }
      sortResultsByScore(m_allResults);
    }
  }

  if (m_browserMode) {
    const auto applications =
        std::ranges::find(m_providers, kApplicationsProviderId, [](const auto& provider) { return provider->id(); });
    if (applications != m_providers.end()) {
      newCategories = (*applications)->categories();
      hasRecentlyUsed = m_usageTracker.getRecentlyUsedCount((*applications)->id()) > 0;
    }
  }

  const int iconTargetSize = static_cast<int>(
      std::round(launcherIconSize(launcherListStyleFrom(m_config, contentScale(), panelCardOpacity(), m_browserMode)))
  );
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  updatePinnedApplicationState();
  const bool hasPinnedApplications = m_config != nullptr
      && !shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned).empty();

  bool categoriesChanged =
      (m_browserMode && m_categoryFilterSlots.empty()) || newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }

  if (hasRecentlyUsed != m_hasRecentlyUsed) {
    m_hasRecentlyUsed = hasRecentlyUsed;
    categoriesChanged = true;
  }
  if (m_browserMode && hasPinnedApplications != m_hasPinnedApplications) {
    m_hasPinnedApplications = hasPinnedApplications;
    categoriesChanged = true;
  }

  if (categoriesChanged) {
    updateCategoryFilterModel(newCategories);
  }

  if (m_applyDefaultViewPending) {
    applyDefaultView();
  }

  m_anyProviderLoading = anyProviderLoading;

  applyActiveCategory();
}

void LauncherPanel::applyPinnedApplicationOrder() {
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  if (pinnedEntries.empty()) {
    return;
  }

  std::vector<std::string> pinnedPaths;
  pinnedPaths.reserve(pinnedEntries.size());
  for (const DesktopEntry& entry : pinnedEntries) {
    if (!entry.path.empty()) {
      pinnedPaths.push_back(entry.path);
    }
  }

  std::ranges::stable_sort(m_results, [&pinnedPaths](const LauncherResult& a, const LauncherResult& b) {
    const auto rank = [&pinnedPaths](const LauncherResult& result) {
      if (result.providerId != kApplicationsProviderId) {
        return pinnedPaths.size();
      }
      const auto it = std::ranges::find(pinnedPaths, result.desktopEntryPath);
      return it == pinnedPaths.end() ? pinnedPaths.size() : static_cast<std::size_t>(it - pinnedPaths.begin());
    };
    return rank(a) < rank(b);
  });
}

void LauncherPanel::updatePinnedApplicationState() {
  for (LauncherResult& result : m_allResults) {
    result.pinned = false;
  }
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  for (LauncherResult& result : m_allResults) {
    if (result.providerId != kApplicationsProviderId) {
      continue;
    }
    result.pinned = std::ranges::any_of(pinnedEntries, [&result](const DesktopEntry& entry) {
      return entry.path == result.desktopEntryPath;
    });
  }
}

void LauncherPanel::reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath) {
  if (m_config == nullptr || sourcePath.empty() || targetPath.empty() || sourcePath == targetPath) {
    return;
  }

  std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
  const auto entries = shell::dock::pinned_apps::resolveEntries(pinned);
  const auto indexForPath = [&entries](std::string_view path) -> std::optional<std::size_t> {
    const auto it = std::ranges::find(entries, path, &DesktopEntry::path);
    if (it == entries.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(it - entries.begin());
  };

  const auto sourceIndex = indexForPath(sourcePath);
  const auto targetIndex = indexForPath(targetPath);
  if (!sourceIndex.has_value() || !targetIndex.has_value() || *sourceIndex == *targetIndex) {
    return;
  }

  std::string moved = std::move(pinned[*sourceIndex]);
  pinned.erase(pinned.begin() + static_cast<std::ptrdiff_t>(*sourceIndex));
  pinned.insert(pinned.begin() + static_cast<std::ptrdiff_t>(*targetIndex), std::move(moved));

  if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::updateCategoryFilterModel(const std::vector<LauncherCategory>& categories) {
  m_currentCategories = categories;
  m_categoryFilterSlots.clear();
  if (!m_browserMode && categories.empty() && !m_hasRecentlyUsed) {
    m_categoryFilterRebuildPending = true;
    PanelManager::instance().requestLayout();
    return;
  }

  if (m_browserMode) {
    if (m_hasRecentlyUsed) {
      m_categoryFilterSlots.push_back({RecentlyUsed, 0});
    }
    if (m_hasPinnedApplications) {
      m_categoryFilterSlots.push_back({Pinned, 0});
    }
    m_categoryFilterSlots.push_back({All, 0});
    if (!categories.empty()) {
      m_categoryFilterSlots.push_back({Category, 0});
    }
  } else {
    m_categoryFilterSlots.push_back({All, 0});
    if (m_hasRecentlyUsed) {
      m_categoryFilterSlots.push_back({RecentlyUsed, 0});
    }
    for (std::size_t i = 0; i < categories.size(); ++i) {
      m_categoryFilterSlots.push_back({Category, i});
    }
  }

  auto activeSlot = m_categoryFilterSlots.end();
  if (m_activeCategoryType == Category) {
    const auto activeCategory = std::ranges::find(categories, m_activeCategory, &LauncherCategory::label);
    if (activeCategory != categories.end()) {
      if (m_browserMode) {
        activeSlot = std::ranges::find(m_categoryFilterSlots, Category, &CategoryFilterSlot::type);
      } else {
        const std::size_t categoryIndex = static_cast<std::size_t>(activeCategory - categories.begin());
        activeSlot = std::ranges::find_if(m_categoryFilterSlots, [categoryIndex](const CategoryFilterSlot& slot) {
          return slot.type == Category && slot.categoryIndex == categoryIndex;
        });
      }
    }
  } else {
    activeSlot = std::ranges::find(m_categoryFilterSlots, m_activeCategoryType, &CategoryFilterSlot::type);
  }

  if (activeSlot == m_categoryFilterSlots.end()) {
    m_activeCategoryType = All;
    m_activeCategory.clear();
    activeSlot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
  }
  m_activeCategorySlotIndex = activeSlot == m_categoryFilterSlots.end()
      ? 0
      : static_cast<std::size_t>(activeSlot - m_categoryFilterSlots.begin());

  m_categoryFilterRebuildPending = true;
  PanelManager::instance().requestLayout();
}

void LauncherPanel::rebuildCategoryFilter() {
  uiAssertNotRendering("LauncherPanel::rebuildCategoryFilter");
  m_categoryFilterRebuildPending = false;

  if (m_categoryFilter == nullptr) {
    rebuildLauncherNavigation();
    return;
  }

  m_categoryFilter->clearOptions();
  if (m_browserMode) {
    std::size_t selectedCategory = 0;
    for (std::size_t i = 0; i < m_currentCategories.size(); ++i) {
      const LauncherCategory& category = m_currentCategories[i];
      m_categoryFilter->addOption(category.label, category.glyphName);
      m_categoryFilter->setOptionTooltip(i, category.label);
      if (category.label == m_activeCategory) {
        selectedCategory = i;
      }
    }
    m_categoryFilter->setSelectedIndex(selectedCategory);
    m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveBrowserCategory(idx); });
  } else {
    for (std::size_t i = 0; i < m_categoryFilterSlots.size(); ++i) {
      const auto& slot = m_categoryFilterSlots[i];
      switch (slot.type) {
      case All:
        m_categoryFilter->addOption("", "layout-grid");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.all"));
        break;
      case Pinned:
        m_categoryFilter->addOption("", "pin-filled");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.browser.navigation.pinned"));
        break;
      case RecentlyUsed:
        m_categoryFilter->addOption("", "history");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.recently-used"));
        break;
      case Category:
        m_categoryFilter->addOption("", m_currentCategories[slot.categoryIndex].glyphName);
        m_categoryFilter->setOptionTooltip(i, m_currentCategories[slot.categoryIndex].label);
        break;
      }
    }
    m_categoryFilter->setSelectedIndex(m_activeCategorySlotIndex);
    m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveCategorySlot(idx); });
  }
  setCategoryFilterVisible(true);
  rebuildLauncherNavigation();
  updateCategorySelectionChrome();
}

void LauncherPanel::rebuildLauncherNavigation() {
  uiAssertNotRendering("LauncherPanel::rebuildLauncherNavigation");
  if (!m_browserMode || m_launcherNavigation == nullptr) {
    return;
  }

  m_launcherNavigation->setOnChange({});
  m_launcherNavigation->clearOptions();
  const bool iconOnly = m_config != nullptr && m_config->config().shell.launcher.width < kCompactNavigationWidth;

  std::size_t navigationIndex = 0;
  for (const CategoryFilterSlot& slot : m_categoryFilterSlots) {
    std::string label;
    std::string glyph;
    switch (slot.type) {
    case All:
      label = i18n::tr("launcher.browser.navigation.all-apps");
      glyph = "apps";
      break;
    case Pinned:
      label = i18n::tr("launcher.browser.navigation.pinned");
      glyph = "pin-filled";
      break;
    case RecentlyUsed:
      label = i18n::tr("launcher.browser.navigation.recent");
      glyph = "history";
      break;
    case Category:
      label = i18n::tr("launcher.browser.navigation.categories");
      glyph = "category";
      break;
    }
    m_launcherNavigation->addOption(iconOnly ? "" : label, glyph);
    m_launcherNavigation->setOptionTooltip(navigationIndex, label);
    ++navigationIndex;
  }
  const std::string providersLabel = i18n::tr("launcher.browser.navigation.providers");
  m_launcherNavigation->addOption(iconOnly ? "" : providersLabel, "sparkles");
  m_launcherNavigation->setOptionTooltip(navigationIndex, providersLabel);
  const std::size_t selectedIndex =
      m_showingProviderOverview ? m_categoryFilterSlots.size() : m_activeCategorySlotIndex;
  m_launcherNavigation->setSelectedIndex(selectedIndex);
  m_launcherNavigation->setOnChange([this](std::size_t index) {
    if (m_syncingLauncherNavigation) {
      return;
    }
    DeferredCall::callLater([this, index]() {
      if (index < m_categoryFilterSlots.size()) {
        setActiveCategorySlot(index);
      } else {
        showProviderOverview();
      }
    });
  });

  updateCategorySelectionChrome();
}

void LauncherPanel::applyDefaultView() {
  m_applyDefaultViewPending = false;
  if (!m_browserMode || !m_query.empty()) {
    return;
  }

  ActiveCategoryType requested = All;
  if (m_config != nullptr) {
    switch (m_config->config().shell.launcher.defaultView) {
    case ShellConfig::LauncherDefaultView::Recent:
      requested = RecentlyUsed;
      break;
    case ShellConfig::LauncherDefaultView::Pinned:
      requested = Pinned;
      break;
    case ShellConfig::LauncherDefaultView::AllApps:
      requested = All;
      break;
    case ShellConfig::LauncherDefaultView::Categories:
      requested = Category;
      break;
    }
  }

  auto slot = std::ranges::find(m_categoryFilterSlots, requested, &CategoryFilterSlot::type);
  if (slot == m_categoryFilterSlots.end()) {
    slot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
  }
  if (slot == m_categoryFilterSlots.end()) {
    return;
  }

  m_activeCategorySlotIndex = static_cast<std::size_t>(slot - m_categoryFilterSlots.begin());
  m_activeCategoryType = slot->type;
  if (m_activeCategoryType == Category && !m_currentCategories.empty()) {
    m_activeCategory = m_currentCategories.front().label;
  } else {
    m_activeCategory.clear();
  }
}

void LauncherPanel::setActiveCategorySlot(std::size_t slotIndex) {
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_sessionReturnView.reset();
  }
  const bool leavingProvider = m_browserMode && (!m_query.empty() || !m_launcherProviderViewId.empty());
  const std::size_t previousSlotIndex = m_activeCategorySlotIndex;
  if (leavingProvider) {
    setQuery("");
  }
  if (slotIndex >= m_categoryFilterSlots.size()) {
    return;
  }

  const auto& slot = m_categoryFilterSlots[slotIndex];
  m_activeCategorySlotIndex = slotIndex;
  m_activeCategoryType = slot.type;
  if (slot.type == Category && !m_currentCategories.empty()) {
    const auto active = std::ranges::find(m_currentCategories, m_activeCategory, &LauncherCategory::label);
    if (active == m_currentCategories.end()) {
      m_activeCategory = m_currentCategories.front().label;
    }
  } else {
    m_activeCategory.clear();
  }
  if (!m_browserMode && m_categoryFilter != nullptr && m_categoryFilter->selectedIndex() != slotIndex) {
    m_categoryFilter->setSelectedIndex(slotIndex);
  }
  updateCategorySelectionChrome();
  if (leavingProvider || slotIndex < previousSlotIndex) {
    queueResultsTransition(ResultsTransition::NavigationBackward);
  } else if (slotIndex > previousSlotIndex) {
    queueResultsTransition(ResultsTransition::NavigationForward);
  }
  applyActiveCategory();
}

void LauncherPanel::setActiveBrowserCategory(std::size_t categoryIndex) {
  if (!m_browserMode || categoryIndex >= m_currentCategories.size()) {
    return;
  }
  const auto previousCategory = std::ranges::find(m_currentCategories, m_activeCategory, &LauncherCategory::label);
  const std::size_t previousCategoryIndex = previousCategory == m_currentCategories.end()
      ? categoryIndex
      : static_cast<std::size_t>(previousCategory - m_currentCategories.begin());
  m_activeCategoryType = Category;
  m_activeCategory = m_currentCategories[categoryIndex].label;
  const auto slot = std::ranges::find(m_categoryFilterSlots, Category, &CategoryFilterSlot::type);
  if (slot != m_categoryFilterSlots.end()) {
    m_activeCategorySlotIndex = static_cast<std::size_t>(slot - m_categoryFilterSlots.begin());
  }
  updateCategorySelectionChrome();
  if (categoryIndex < previousCategoryIndex) {
    queueResultsTransition(ResultsTransition::NavigationBackward);
  } else if (categoryIndex > previousCategoryIndex) {
    queueResultsTransition(ResultsTransition::NavigationForward);
  }
  applyActiveCategory();
}

void LauncherPanel::updateCategorySelectionChrome() {
  if (m_launcherNavigation != nullptr && !m_categoryFilterSlots.empty()) {
    const std::size_t selectedIndex =
        m_showingProviderOverview ? m_categoryFilterSlots.size() : m_activeCategorySlotIndex;
    if (m_launcherNavigation->selectedIndex() != selectedIndex) {
      m_syncingLauncherNavigation = true;
      m_launcherNavigation->setSelectedIndex(selectedIndex);
      m_syncingLauncherNavigation = false;
    }
  }
  if (m_launcherPowerButton != nullptr) {
    m_launcherPowerButton->setSelected(m_launcherProviderViewId == kSessionProviderId);
  }
  setCategoryFilterVisible(true);
}

void LauncherPanel::updateLauncherOptionButtons() {
  if (m_config == nullptr) {
    return;
  }
  const auto& launcher = m_config->config().shell.launcher;
  if (m_launcherUsageButton != nullptr) {
    const bool orderingAvailable = !m_showingProviderOverview
        && m_launcherProviderViewId.empty()
        && (m_activeCategoryType == All || m_activeCategoryType == Category);
    m_launcherUsageButton->setVisible(orderingAvailable);
    m_launcherUsageButton->setParticipatesInLayout(orderingAvailable);
    m_launcherUsageButton->setEnabled(orderingAvailable);
    m_launcherUsageButton->setSelected(false);
    m_launcherUsageButton->setGlyph(launcher.sortByUsage ? "sort-ascending-letters" : "sort-descending-small-big");
    m_launcherUsageButton->setTooltip(
        i18n::tr(
            launcher.sortByUsage ? "launcher.browser.options.use-default-order"
                                 : "launcher.browser.options.sort-by-frequency"
        )
    );
  }
  if (m_launcherGridButton != nullptr) {
    m_launcherGridButton->setSelected(false);
    m_launcherGridButton->setGlyph(launcher.appGrid ? "list" : "layout-grid");
    m_launcherGridButton->setTooltip(
        i18n::tr(launcher.appGrid ? "launcher.browser.options.show-list" : "launcher.browser.options.show-grid")
    );
  }
}

void LauncherPanel::setCategoryFilterVisible(bool visible) {
  if (m_categoryFilter == nullptr) {
    return;
  }
  const bool show = m_browserMode ? visible
          && m_activeCategoryType == Category
          && m_query.empty()
          && m_launcherProviderViewId.empty()
          && !m_currentCategories.empty()
                                  : visible && !m_categoryFilterSlots.empty();
  m_categoryFilter->setVisible(show);
  m_categoryFilter->setParticipatesInLayout(show);
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}

bool LauncherPanel::startsWithLauncherPrefix(std::string_view text) const {
  const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
  return text.starts_with(prefix);
}

std::vector<LauncherResult> LauncherPanel::providerOverviewResults(std::string_view text) const {
  std::string filter;
  if (startsWithLauncherPrefix(text)) {
    const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
    filter = StringUtils::toLower(StringUtils::trim(text.substr(prefix.size())));
  }

  std::vector<LauncherResult> results;
  results.reserve(m_providers.size());
  for (const auto& provider : m_providers) {
    const std::string_view prefix = provider->prefix();
    if (prefix.empty()) {
      continue;
    }

    const std::string title(provider->displayName());
    const std::string prefixText(prefix);
    const std::string searchable = StringUtils::toLower(title + " " + prefixText);
    const double score = filter.empty() ? 0.0 : FuzzyMatch::score(filter, searchable);
    if (!filter.empty() && !FuzzyMatch::isMatch(score)) {
      continue;
    }

    LauncherResult result;
    result.id = providerOverviewId(prefix);
    result.providerId = std::string(kProviderOverviewProviderId);
    result.title = title;
    result.subtitle = prefixText;
    result.glyphName = std::string(provider->defaultGlyphName());
    result.score = score;
    results.push_back(std::move(result));
  }

  if (!filter.empty()) {
    sortResultsByScore(results);
  }
  return results;
}

void LauncherPanel::applyActiveCategory() {
  m_results.clear();
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_results = m_allResults;
    m_selectedIndex = 0;
    updateCategorySelectionChrome();
    refreshResults();
    return;
  }
  switch (m_activeCategoryType) {
  case All:
    m_results = m_allResults;
    break;
  case Pinned:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& result) {
      return result.pinned;
    });
    applyPinnedApplicationOrder();
    break;
  case RecentlyUsed:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& r) {
      return r.recentlyUsedIndex > 0;
    });
    std::ranges::sort(m_results, [](const LauncherResult& a, const LauncherResult& b) {
      return a.recentlyUsedIndex > b.recentlyUsedIndex
          || (a.recentlyUsedIndex == b.recentlyUsedIndex
              && std::tie(a.providerId, a.id) < std::tie(b.providerId, b.id));
    });
    break;
  case Category:
    for (const auto& r : m_allResults) {
      if (r.category == m_activeCategory) {
        m_results.push_back(r);
      }
    }
    break;
  }
  m_selectedIndex = 0;
  updateCategorySelectionChrome();
  refreshResults();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  syncLauncherViewLayout(nullptr);
  rebuildLauncherSections();
  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0F);
    if (m_launcherSections != nullptr) {
      m_launcherSections->scrollView().setScrollOffset(0.0F);
    }
  } else {
    selectResult(std::min(m_selectedIndex, m_results.size() - 1), false);
  }
  bindDetailResult();
  applyEmptyState();
  animateResultsTransition();
}

void LauncherPanel::selectResult(std::size_t index, bool scrollToSelection) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  if (shouldUseLauncherSections() && m_launcherSectionsAdapter != nullptr && m_launcherSections != nullptr) {
    m_grid->setSelectedIndex(std::nullopt);
    m_launcherSectionsAdapter->setSelectedResultIndex(index);
    m_launcherSections->notifyDataChanged();
    if (scrollToSelection) {
      if (const auto row = m_launcherSectionsAdapter->rowForResult(index)) {
        m_launcherSections->scrollToIndex(*row);
      }
    }
    return;
  }
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(index);
  }
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  const bool detail = !empty && shouldUseDetailPresentation();
  const bool sections = !empty && !detail && shouldUseLauncherSections();
  m_grid->setVisible(!empty && !detail && !sections);
  m_grid->setParticipatesInLayout(!empty && !detail && !sections);
  if (m_launcherSections != nullptr) {
    m_launcherSections->setVisible(sections);
    m_launcherSections->setParticipatesInLayout(sections);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setVisible(detail);
    m_detailScroll->setParticipatesInLayout(detail);
  }
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  if (empty) {
    if (m_anyProviderLoading && !m_query.empty()) {
      m_emptyLabel->setText(i18n::tr("launcher.empty.loading"));
    } else {
      m_emptyLabel->setText(
          m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
      );
    }
  }
}

bool LauncherPanel::shouldUseDetailPresentation() const {
  return m_results.size() == 1 && isDetailPresentation(m_results.front());
}

void LauncherPanel::bindDetailResult() {
  if (!shouldUseDetailPresentation()
      || m_detailScroll == nullptr
      || m_detailSubtitle == nullptr
      || m_detailBody == nullptr) {
    return;
  }

  const LauncherResult& result = m_results.front();
  const bool hasSubtitle = !result.subtitle.empty();
  m_detailSubtitle->setVisible(hasSubtitle);
  m_detailSubtitle->setParticipatesInLayout(hasSubtitle);
  m_detailSubtitle->setText(singleLinePreview(result.subtitle));
  m_detailBody->setText(result.title.empty() ? result.id : result.title);
  m_detailScroll->setScrollOffset(0.0F);
}

bool LauncherPanel::openAppActionsMenu(std::size_t index, float anchorX, float anchorY) {
  if (index >= m_results.size()) {
    return false;
  }
  const LauncherResult& base = m_results[index];

  const DesktopEntry* match = nullptr;
  for (const auto& e : desktopEntries()) {
    if (e.path == base.desktopEntryPath) {
      match = &e;
      break;
    }
  }
  if (match == nullptr) {
    return false;
  }

  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  if (wl == nullptr || rc == nullptr) {
    return false;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return false;
  }

  if (m_actionsMenu == nullptr) {
    m_actionsMenu = std::make_unique<ContextMenuPopup>(*wl, *rc);
  }

  std::vector<DesktopAction> actionsCopy = match->actions;
  const bool launcherPinned =
      m_config != nullptr && shell::dock::pinned_apps::containsEntry(m_config->config().shell.launcher.pinned, *match);
  const bool canPin = m_config != nullptr && !launcherPinned;
  const bool canUnpin = m_config != nullptr && launcherPinned;

  constexpr std::int32_t kActionOpen = -1;
  constexpr std::int32_t kActionPin = -2;
  constexpr std::int32_t kActionUnpin = -3;

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(actionsCopy.size() + 2);
  entries.push_back(
      ContextMenuControlEntry{
          .id = kActionOpen,
          .label = i18n::tr("launcher.context-menu.open"),
          .enabled = true,
          .separator = false,
          .hasSubmenu = false,
      }
  );
  // Desktop actions sit above pin/unpin so pin stays last in the menu.
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(actionsCopy.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = actionsCopy[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }
  if (canPin) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionPin,
            .label = i18n::tr("launcher.context-menu.pin"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  } else if (canUnpin) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionUnpin,
            .label = i18n::tr("launcher.context-menu.unpin"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }

  const float scale = contentScale();
  constexpr float kMenuWidth = 240.0F;
  const float minMenuWidth = kMenuWidth * scale;

  if (m_config != nullptr) {
    m_actionsMenu->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenu.get());

  m_actionsMenu->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_actionsMenu->setOnActivate([this, base, actionsCopy = std::move(actionsCopy),
                                entryForPin = *match](const ContextMenuControlEntry& entry) {
    LauncherResult result = base;
    if (entry.id == kActionPin) {
      if (m_config == nullptr
          || entryForPin.id.empty()
          || shell::dock::pinned_apps::containsEntry(m_config->config().shell.launcher.pinned, entryForPin)) {
        return;
      }
      std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
      pinned.push_back(entryForPin.id);
      if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (entry.id == kActionUnpin) {
      if (m_config == nullptr) {
        return;
      }
      std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
      shell::dock::pinned_apps::removeEntry(pinned, entryForPin);
      if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (entry.id >= 0 && entry.id < static_cast<std::int32_t>(actionsCopy.size())) {
      const DesktopAction& action = actionsCopy[static_cast<std::size_t>(entry.id)];
      result.id = AppProvider::actionResultId(result.desktopEntryPath, action.id);
      result.desktopActionId = action.id;
    } else if (entry.id != kActionOpen) {
      return;
    }

    for (auto& provider : m_providers) {
      if (provider->id() != std::string_view(result.providerId)) {
        continue;
      }
      if (!provider->activate(result)) {
        return;
      }
      finishActivation(*provider, result.id, provider->supportsAutoPaste());
      return;
    }
    return;
  });

  const float inset = std::round(std::max(4.0F, Style::spaceXs * scale));
  const auto ax = static_cast<std::int32_t>(std::round(anchorX - inset));
  const auto ay = static_cast<std::int32_t>(std::round(anchorY - inset));
  const auto aw = static_cast<std::int32_t>(std::round(inset * 2.0F));
  const auto ah = static_cast<std::int32_t>(std::round(inset * 2.0F));

  m_actionsMenu->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = minMenuWidth,
          .maxMenuWidth = Style::menuAutoMaxWidth * scale,
          .maxVisible = 12,
          .anchor =
              PopupAnchorRect{
                  .x = ax,
                  .y = ay,
                  .width = std::max(1, aw),
                  .height = std::max(1, ah),
              },
          .parent = PopupSurfaceParent{
              .layerSurface = parentCtx->layerSurface,
              .output = parentCtx->output,
          },
      }
  );
  return true;
}

void LauncherPanel::activateAt(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  selectResult(index, false);
  activateSelected();
}

void LauncherPanel::activateSelected() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }

  const auto& result = m_results[m_selectedIndex];
  if (result.providerId == kProviderOverviewProviderId && result.id.starts_with(kProviderOverviewResultPrefix)) {
    std::string prefix = result.id.substr(kProviderOverviewResultPrefix.size());
    if (!prefix.empty()) {
      prefix += ' ';
    }
    if (m_input != nullptr) {
      m_input->setValue(prefix);
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0F);
    }
    onInputChanged(prefix);
    return;
  }

  // Dispatch only to the provider that produced this result. Providers can use
  // overlapping id shapes, so probing every provider risks side effects.
  for (auto& provider : m_providers) {
    if (provider->id() != std::string_view(result.providerId)) {
      continue;
    }

    if (!provider->activate(result)) {
      return;
    }

    finishActivation(*provider, result.id, provider->supportsAutoPaste());
    return;
  }
}

bool LauncherPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  const bool sectionNav =
      shouldUseLauncherSections() && m_launcherSectionsAdapter != nullptr && m_launcherSections != nullptr;
  const bool gridNav = !sectionNav && m_usingAppGrid && m_grid != nullptr;
  const int columns = gridNav ? static_cast<int>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;

  const auto moveSelection = [this](int delta) {
    if (m_results.empty()) {
      return;
    }
    const int last = static_cast<int>(m_results.size() - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    if (next == static_cast<int>(m_selectedIndex)) {
      return;
    }
    selectResult(static_cast<std::size_t>(next), true);
  };

  const auto moveSectionSelection = [this](bool vertical, int direction, std::size_t rows) {
    if (m_launcherSectionsAdapter == nullptr || m_results.empty()) {
      return;
    }
    const std::optional<std::size_t> next = vertical
        ? m_launcherSectionsAdapter->moveVertical(m_selectedIndex, direction, rows)
        : m_launcherSectionsAdapter->moveHorizontal(m_selectedIndex, direction);
    if (next.has_value()) {
      selectResult(*next, true);
    }
  };

  if (KeySymbol::isPageUp(sym)) {
    if (sectionNav) {
      const std::size_t rows = m_launcherSectionsAdapter->estimatedVisibleApplicationRows(
          m_launcherSections->scrollView().contentViewportHeight()
      );
      moveSectionSelection(true, -1, rows);
      return true;
    }
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    if (sectionNav) {
      const std::size_t rows = m_launcherSectionsAdapter->estimatedVisibleApplicationRows(
          m_launcherSections->scrollView().contentViewportHeight()
      );
      moveSectionSelection(true, 1, rows);
      return true;
    }
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(true, -1, 1);
      return true;
    }
    moveSelection(gridNav ? -columns : -1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(true, 1, 1);
      return true;
    }
    moveSelection(gridNav ? columns : 1);
    return true;
  }

  if ((gridNav || sectionNav) && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(false, -1, 1);
    } else {
      moveSelection(-1);
    }
    return true;
  }

  if ((gridNav || sectionNav) && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(false, 1, 1);
    } else {
      moveSelection(1);
    }
    return true;
  }

  // Validate+Shift opens the app context menu (Shift layered on the configured
  // Validate chord). Menu navigation uses the Up/Down/Validate/Cancel keybinds.
  if ((modifiers & KeyMod::Shift) != 0
      && KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers & ~KeyMod::Shift)) {
    float anchorX = 0.0F;
    float anchorY = 0.0F;
    if (sectionNav && m_launcherSections != nullptr) {
      Node::absolutePosition(m_launcherSections, anchorX, anchorY);
      anchorX += m_launcherSections->width() * 0.5F;
      anchorY += m_launcherSections->height() * 0.5F;
    } else if (m_grid == nullptr || !m_grid->absoluteAnchorForIndex(m_selectedIndex, anchorX, anchorY)) {
      if (m_grid != nullptr) {
        Node::absolutePosition(m_grid, anchorX, anchorY);
        anchorX += m_grid->width() * 0.5F;
        anchorY += m_grid->height() * 0.5F;
      }
    }
    return openAppActionsMenu(m_selectedIndex, anchorX, anchorY);
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}
