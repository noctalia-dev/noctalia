#include "shell/bar/widgets/privacy_widget.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace {

  void addNonEmpty(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
      return;
    }
    values.push_back(std::move(value));
  }

  void sortUnique(std::vector<std::string>& values) {
    std::ranges::sort(values);
    const auto duplicates = std::ranges::unique(values);
    values.erase(duplicates.begin(), duplicates.end());
  }

  [[nodiscard]] std::string joinApps(const std::vector<std::string>& apps) {
    auto joined = apps | std::views::join_with(std::string_view{", "});
    return {joined.begin(), joined.end()};
  }

} // namespace

PrivacyWidget::PrivacyWidget(PipeWireService* pipewire, ConfigService* configService, PrivacyWidgetConfig config)
    : m_pipewire(pipewire), m_configService(configService), m_config(config) {
  m_config.iconSpacing = std::clamp(m_config.iconSpacing, 0, 48);
}

void PrivacyWidget::create() {
  setRoot(
      ui::inputArea(
          {
              .out = &m_area,
              .tooltipProvider = [this]() -> TooltipContent {
                std::vector<TooltipRow> rows = buildTooltipRows();
                if (rows.empty()) {
                  return std::monostate{};
                }
                return TooltipContent{std::move(rows)};
              },
          },
          ui::glyph({
              .out = &m_micGlyph,
              .glyph = "microphone-off",
              .glyphSize = Style::baseGlyphSize * m_contentScale,
              .color = m_config.inactiveColor,
          }),
          ui::glyph({
              .out = &m_cameraGlyph,
              .glyph = "camera-off",
              .glyphSize = Style::baseGlyphSize * m_contentScale,
              .color = m_config.inactiveColor,
          }),
          ui::glyph({
              .out = &m_screenGlyph,
              .glyph = "screen-share-off",
              .glyphSize = Style::baseGlyphSize * m_contentScale,
              .color = m_config.inactiveColor,
          })
      )
  );
}

void PrivacyWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }

  m_isVertical = containerHeight > containerWidth;
  syncState();

  if (!rootNode->visible()) {
    rootNode->setSize(0.0f, 0.0f);
    return;
  }

  std::vector<Glyph*> glyphs;
  for (Glyph* glyph : {m_micGlyph, m_cameraGlyph, m_screenGlyph}) {
    if (glyph != nullptr && glyph->visible()) {
      glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      glyph->measure(renderer);
      glyphs.push_back(glyph);
    }
  }

  if (glyphs.empty()) {
    rootNode->setSize(0.0f, 0.0f);
    return;
  }

  const float spacing = static_cast<float>(m_config.iconSpacing) * m_contentScale;
  float width = 0.0f;
  float height = 0.0f;

  if (m_isVertical) {
    for (Glyph* glyph : glyphs) {
      width = std::max(width, glyph->width());
      height += glyph->height();
    }
    height += spacing * static_cast<float>(glyphs.size() - 1);

    float y = 0.0f;
    for (Glyph* glyph : glyphs) {
      glyph->setPosition(std::round((width - glyph->width()) * 0.5f), y);
      y += glyph->height() + spacing;
    }
  } else {
    for (Glyph* glyph : glyphs) {
      width += glyph->width();
      height = std::max(height, glyph->height());
    }
    width += spacing * static_cast<float>(glyphs.size() - 1);

    float x = 0.0f;
    for (Glyph* glyph : glyphs) {
      glyph->setPosition(x, std::round((height - glyph->height()) * 0.5f));
      x += glyph->width() + spacing;
    }
  }

  rootNode->setSize(width, height);
}

void PrivacyWidget::doUpdate(Renderer&) { syncState(); }

void PrivacyWidget::syncState() {
  Snapshot current = snapshot();
  current.vertical = m_isVertical;
  if (m_cachedSnapshot.has_value() && *m_cachedSnapshot == current) {
    return;
  }

  m_cachedSnapshot = current;

  auto applyGlyph = [this](Glyph* glyph, bool active, std::string_view activeGlyph, std::string_view inactiveGlyph) {
    if (glyph == nullptr) {
      return;
    }
    const bool visible = active || !m_config.hideInactive;
    glyph->setVisible(visible);
    glyph->setParticipatesInLayout(visible);
    glyph->setGlyph(active ? activeGlyph : inactiveGlyph);
    glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    glyph->setColor(active ? m_config.activeColor : m_config.inactiveColor);
  };

  applyGlyph(m_micGlyph, current.micActive(), "microphone", "microphone-off");
  applyGlyph(m_cameraGlyph, current.cameraActive(), "camera", "camera-off");
  applyGlyph(m_screenGlyph, current.screenActive(), "screen-share", "screen-share-off");

  if (auto* rootNode = root(); rootNode != nullptr) {
    const bool visible = current.visible(m_config.hideInactive);
    rootNode->setVisible(visible);
    rootNode->setParticipatesInLayout(visible);
    rootNode->markLayoutDirty();
  }
  if (m_area != nullptr) {
    m_area->requestTooltipRefresh();
  }

  requestRedraw();
}

void PrivacyWidget::refreshFilters() const {
  if (m_configService == nullptr) {
    return;
  }
  const ShellConfig::PrivacyConfig& privacy = m_configService->config().shell.privacy;
  m_micFilter.update("shell.privacy.mic_filter_regex", privacy.micFilterRegex);
  m_camFilter.update("shell.privacy.cam_filter_regex", privacy.camFilterRegex);
  m_screenFilter.update("shell.privacy.screen_filter_regex", privacy.screenFilterRegex);
}

PrivacyWidget::Snapshot PrivacyWidget::snapshot() const {
  Snapshot out;
  refreshFilters();

  if (m_pipewire != nullptr) {
    const PrivacyState& state = m_pipewire->privacyState();
    for (const auto& capture : state.captures) {
      if (capture.kind == PrivacyCaptureKind::Microphone) {
        if (!m_micFilter.matches(capture.appName)) {
          addNonEmpty(out.micApps, capture.appName);
        }
      } else if (capture.kind == PrivacyCaptureKind::Camera) {
        if (!m_camFilter.matches(capture.appName)) {
          addNonEmpty(out.cameraApps, capture.appName);
        }
      } else if (capture.kind == PrivacyCaptureKind::Screen) {
        if (!m_screenFilter.matches(capture.appName)) {
          addNonEmpty(out.screenApps, capture.appName);
        }
      }
    }
  }

  sortUnique(out.micApps);
  sortUnique(out.cameraApps);
  sortUnique(out.screenApps);
  return out;
}

std::vector<TooltipRow> PrivacyWidget::buildTooltipRows() const {
  const Snapshot current = snapshot();
  std::vector<TooltipRow> rows;
  if (current.micActive()) {
    rows.push_back({i18n::tr("bar.widgets.privacy.microphone"), joinApps(current.micApps)});
  }
  if (current.cameraActive()) {
    rows.push_back({i18n::tr("bar.widgets.privacy.camera"), joinApps(current.cameraApps)});
  }
  if (current.screenActive()) {
    rows.push_back({i18n::tr("bar.widgets.privacy.screen-sharing"), joinApps(current.screenApps)});
  }
  return rows;
}
