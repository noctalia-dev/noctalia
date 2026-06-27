#include "shell/desktop/widgets/desktop_fancy_audio_visualizer_widget.h"

#include "pipewire/pipewire_spectrum.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/style.h"
#include "ui/visuals/fancy_audio_visualizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>

namespace {

  constexpr float kBaseSize = 300.0f;
  constexpr int kBandCount = FancyAudioVisualizerNode::kBandCount;

  [[nodiscard]] float valueAsFloat(const WidgetSettingValue& value, float fallback) {
    if (const auto* v = std::get_if<double>(&value)) {
      return static_cast<float>(*v);
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      return static_cast<float>(*v);
    }
    return fallback;
  }

} // namespace

DesktopFancyAudioVisualizerWidget::DesktopFancyAudioVisualizerWidget(PipeWireSpectrum* spectrum, Options options)
    : m_spectrum(spectrum), m_mode(options.mode), m_sensitivity(options.sensitivity),
      m_rotationSpeed(options.rotationSpeed), m_barWidth(options.barWidth), m_ringOpacity(options.ringOpacity),
      m_bloomIntensity(options.bloomIntensity), m_waveThickness(options.waveThickness),
      m_innerDiameter(options.innerDiameter), m_fadeWhenIdle(options.fadeWhenIdle),
      m_primaryColor(options.primaryColor), m_secondaryColor(options.secondaryColor) {}

DesktopFancyAudioVisualizerWidget::~DesktopFancyAudioVisualizerWidget() {
  cancelVisibilityAnimation();
  if (m_spectrum != nullptr && m_listenerId != 0) {
    m_spectrum->removeChangeListener(m_listenerId);
  }
}

void DesktopFancyAudioVisualizerWidget::create() {
  auto rootNode = std::make_unique<Node>();
  rootNode->setClipChildren(true);

  auto visualizer = std::make_unique<FancyAudioVisualizer>();
  m_visualizer = visualizer.get();
  rootNode->addChild(std::move(visualizer));
  syncStyle();

  if (m_spectrum != nullptr) {
    m_listenerId = m_spectrum->addChangeListener(kBandCount, [this]() {
      m_pendingSpectrumUpdate = true;
      // Spectrum animation is paint-only; the frame tick pulls new values and repaints. Only a
      // visibility change needs layout.
      if (applyVisibility()) {
        requestLayout();
      }
      requestFrameTick();
      requestRedraw();
    });
  }

  setRoot(std::move(rootNode));
}

bool DesktopFancyAudioVisualizerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (m_visualizer == nullptr) {
    return false;
  }

  if (key == "visualization_mode") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (*v == "bars") {
        m_mode = FancyAudioVisualizerMode::Bars;
      } else if (*v == "wave") {
        m_mode = FancyAudioVisualizerMode::Wave;
      } else if (*v == "rings") {
        m_mode = FancyAudioVisualizerMode::Rings;
      } else if (*v == "bars_rings") {
        m_mode = FancyAudioVisualizerMode::BarsRings;
      } else if (*v == "wave_rings") {
        m_mode = FancyAudioVisualizerMode::WaveRings;
      } else if (*v == "all") {
        m_mode = FancyAudioVisualizerMode::All;
      } else {
        return false;
      }
      syncStyle();
      requestRedraw();
      return true;
    }
    return false;
  }

  if (key == "primary_color" || key == "secondary_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (key == "primary_color") {
        m_primaryColor = colorSpecFromConfigString(*v, key);
      } else {
        m_secondaryColor = colorSpecFromConfigString(*v, key);
      }
      syncStyle();
      requestRedraw();
      return true;
    }
    return false;
  }

  if (key == "fade_when_idle") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_fadeWhenIdle = *v;
      if (applyVisibility()) {
        requestLayout();
      }
      requestFrameTick();
      requestRedraw();
      return true;
    }
    return false;
  }

  if (key == "sensitivity"
      || key == "rotation_speed"
      || key == "bar_width"
      || key == "ring_opacity"
      || key == "bloom_intensity"
      || key == "wave_thickness"
      || key == "inner_diameter") {
    const float next = valueAsFloat(value, 0.0f);
    if (key == "sensitivity") {
      m_sensitivity = next;
    } else if (key == "rotation_speed") {
      m_rotationSpeed = next;
    } else if (key == "bar_width") {
      m_barWidth = next;
    } else if (key == "ring_opacity") {
      m_ringOpacity = next;
    } else if (key == "bloom_intensity") {
      m_bloomIntensity = next;
    } else if (key == "wave_thickness") {
      m_waveThickness = next;
    } else if (key == "inner_diameter") {
      m_innerDiameter = next;
    }
    syncStyle();
    requestRedraw();
    return true;
  }

  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopFancyAudioVisualizerWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  m_pendingSpectrumUpdate = true;
  if (root() == nullptr) {
    return;
  }
  if (applyVisibility()) {
    requestLayout();
  }
  requestFrameTick();
  requestRedraw();
}

bool DesktopFancyAudioVisualizerWidget::needsFrameTick() const {
  return m_visualizer != nullptr
      && (m_pendingSpectrumUpdate
          || !m_spectrumTextureInitialized
          || m_visualizer->textureId() == 0
          || shouldAnimateTime()
          || shouldBeVisible() != m_visible
          || m_fadingOut
          || m_visibilityAnimId != 0);
}

void DesktopFancyAudioVisualizerWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  if (m_visualizer == nullptr) {
    return;
  }

  applyVisibility();
  ensureSpectrumTexture(renderer);
  if (m_visible) {
    syncSpectrum(renderer);
    if (shouldAnimateTime()) {
      m_shaderTime = std::fmod(m_shaderTime + std::max(0.0f, deltaMs) * 0.001f, 3600.0f);
      m_visualizer->setTime(m_shaderTime);
    }
  }
}

void DesktopFancyAudioVisualizerWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr) {
    return;
  }
  applyVisibility();
  layoutContentSize(renderer);
}

void DesktopFancyAudioVisualizerWidget::doUpdate(Renderer& renderer) {
  ensureSpectrumTexture(renderer);
  if (applyVisibility()) {
    requestLayout();
  }
  syncSpectrum(renderer);
}

void DesktopFancyAudioVisualizerWidget::layoutContentSize(Renderer& renderer) {
  (void)renderer;
  const float size = std::round(kBaseSize * m_contentScale);
  if (m_visualizer != nullptr) {
    m_visualizer->setPosition(0.0f, 0.0f);
    m_visualizer->setSize(size, size);
    m_visualizer->setCornerRadius(Style::scaledRadiusLg(m_contentScale));
  }
  if (root() != nullptr) {
    root()->setSize(size, size);
  }
}

void DesktopFancyAudioVisualizerWidget::ensureSpectrumTexture(Renderer& renderer) {
  if (m_visualizer == nullptr) {
    return;
  }
  if (m_spectrumTextureInitialized && m_visualizer->textureId() != 0) {
    return;
  }
  const std::array<float, kBandCount> empty{};
  if (m_visualizer->setValues(renderer.textureManager(), std::span<const float>(empty.data(), empty.size()))) {
    m_spectrumTextureInitialized = true;
  }
}

void DesktopFancyAudioVisualizerWidget::pullSpectrumValues(Renderer& renderer) {
  if (m_visualizer == nullptr || m_spectrum == nullptr || m_listenerId == 0) {
    ensureSpectrumTexture(renderer);
    m_pendingSpectrumUpdate = false;
    return;
  }

  const auto& values = m_spectrum->values(m_listenerId);
  if (values.empty()) {
    ensureSpectrumTexture(renderer);
    m_pendingSpectrumUpdate = false;
    return;
  }

  m_visualizer->setValues(renderer.textureManager(), values);
  m_spectrumTextureInitialized = true;
  m_pendingSpectrumUpdate = false;
}

void DesktopFancyAudioVisualizerWidget::syncSpectrum(Renderer& renderer) {
  if (!m_pendingSpectrumUpdate && !m_editorPreview) {
    return;
  }
  pullSpectrumValues(renderer);
}

void DesktopFancyAudioVisualizerWidget::syncStyle() {
  if (m_visualizer == nullptr) {
    return;
  }
  m_visualizer->setVisualizationMode(m_mode);
  m_visualizer->setPrimaryColor(m_primaryColor);
  m_visualizer->setSecondaryColor(m_secondaryColor);
  m_visualizer->setSensitivity(m_sensitivity);
  m_visualizer->setRotationSpeed(m_rotationSpeed);
  m_visualizer->setBarWidth(m_barWidth);
  m_visualizer->setRingOpacity(m_ringOpacity);
  m_visualizer->setBloomIntensity(m_bloomIntensity);
  m_visualizer->setWaveThickness(m_waveThickness);
  m_visualizer->setInnerDiameter(m_innerDiameter);
}

bool DesktopFancyAudioVisualizerWidget::shouldBeVisible() const {
  if (m_spectrum == nullptr) {
    return false;
  }
  return m_editorPreview || !m_fadeWhenIdle || !m_spectrum->idle();
}

bool DesktopFancyAudioVisualizerWidget::shouldAnimateTime() const {
  if (m_spectrum == nullptr) {
    return m_editorPreview;
  }
  return m_editorPreview || !m_spectrum->idle();
}

bool DesktopFancyAudioVisualizerWidget::applyVisibility() {
  if (root() == nullptr) {
    return false;
  }
  const bool nextVisible = shouldBeVisible();
  if (!m_visibilityInitialized) {
    m_visibilityInitialized = true;
    m_fadingOut = false;
    m_visible = nextVisible;
    setVisibilityCollapsed(!m_visible);
    root()->setOpacity(m_visible ? 1.0f : 0.0f);
    return false;
  }

  if (!nextVisible) {
    if (!m_visible || m_fadingOut) {
      return false;
    }
    m_fadingOut = true;
    startOpacityAnimation(0.0f, true);
    return false;
  }

  if (m_visible && !m_fadingOut) {
    return false;
  }

  const bool wasCollapsed = !m_visible;
  cancelVisibilityAnimation();
  m_fadingOut = false;
  m_visible = true;
  setVisibilityCollapsed(false);
  startOpacityAnimation(1.0f, false);
  return wasCollapsed;
}

void DesktopFancyAudioVisualizerWidget::cancelVisibilityAnimation() {
  if (m_visibilityAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_visibilityAnimId);
  }
  m_visibilityAnimId = 0;
}

void DesktopFancyAudioVisualizerWidget::setVisibilityCollapsed(bool collapsed) {
  if (m_visualizer != nullptr) {
    m_visualizer->setVisible(!collapsed);
  }
}

void DesktopFancyAudioVisualizerWidget::startOpacityAnimation(float targetOpacity, bool collapseOnComplete) {
  if (root() == nullptr) {
    return;
  }
  cancelVisibilityAnimation();

  if (m_animations == nullptr) {
    root()->setOpacity(targetOpacity);
    if (collapseOnComplete) {
      m_fadingOut = false;
      m_visible = false;
      setVisibilityCollapsed(true);
    }
    return;
  }

  m_visibilityAnimId = m_animations->animate(
      root()->opacity(), targetOpacity, Style::animSlow, Easing::EaseOutCubic,
      [this](float opacity) {
        if (root() != nullptr) {
          root()->setOpacity(opacity);
        }
      },
      [this, collapseOnComplete]() {
        m_visibilityAnimId = 0;
        if (!collapseOnComplete) {
          return;
        }
        if (shouldBeVisible()) {
          m_fadingOut = false;
          applyVisibility();
          return;
        }
        m_fadingOut = false;
        m_visible = false;
        setVisibilityCollapsed(true);
        requestRedraw();
      },
      this
  );
  requestFrameTick();
}
