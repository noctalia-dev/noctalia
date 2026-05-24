#include "shell/desktop/widgets/desktop_audio_visualizer_widget.h"

#include "pipewire/pipewire_spectrum.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/controls/audio_spectrum.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr float kDefaultVisualizerArea = 240.0f * 96.0f;

  float clampAspectRatio(float aspectRatio) { return std::max(0.05f, aspectRatio); }

  float visualizerBaseWidth(float aspectRatio) {
    return std::sqrt(kDefaultVisualizerArea * clampAspectRatio(aspectRatio));
  }

  float visualizerBaseHeight(float aspectRatio) {
    return std::sqrt(kDefaultVisualizerArea / clampAspectRatio(aspectRatio));
  }

} // namespace

DesktopAudioVisualizerWidget::DesktopAudioVisualizerWidget(
    PipeWireSpectrum* spectrum, float aspectRatio, int bands, bool mirrored, ColorSpec lowColor, ColorSpec highColor,
    bool centered, bool showWhenIdle
)
    : m_spectrum(spectrum), m_aspectRatio(clampAspectRatio(aspectRatio)), m_bands(std::max(1, bands)),
      m_mirrored(mirrored), m_centered(centered), m_showWhenIdle(showWhenIdle), m_lowColor(lowColor),
      m_highColor(highColor) {}

DesktopAudioVisualizerWidget::~DesktopAudioVisualizerWidget() {
  cancelVisibilityAnimation();
  if (m_spectrum != nullptr && m_listenerId != 0) {
    m_spectrum->removeChangeListener(m_listenerId);
  }
}

void DesktopAudioVisualizerWidget::create() {
  auto rootNode = std::make_unique<Node>();
  rootNode->setClipChildren(true);

  auto visualizer = std::make_unique<AudioSpectrum>();
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizer->setCentered(m_centered);
  visualizer->setMirrored(m_mirrored);
  visualizer->setGradient(m_lowColor, m_highColor);
  m_visualizer = visualizer.get();
  rootNode->addChild(std::move(visualizer));

  if (m_spectrum != nullptr) {
    m_listenerId = m_spectrum->addChangeListener(m_bands, [this]() {
      m_pendingSpectrumUpdate = true;
      requestFrameTick();
    });
  }

  setRoot(std::move(rootNode));
}

bool DesktopAudioVisualizerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (m_visualizer == nullptr) {
    return false;
  }
  if (key == "low_color" || key == "high_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (key == "low_color") {
        m_lowColor = colorSpecFromConfigString(*v, key);
      } else {
        m_highColor = colorSpecFromConfigString(*v, key);
      }
      m_visualizer->setGradient(m_lowColor, m_highColor);
      return true;
    }
    return false;
  }
  if (key == "mirrored") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_mirrored = *v;
      m_visualizer->setMirrored(m_mirrored);
      return true;
    }
    return false;
  }
  if (key == "centered") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_centered = *v;
      m_visualizer->setCentered(m_centered);
      return true;
    }
    return false;
  }
  if (key == "show_when_idle") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showWhenIdle = *v;
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopAudioVisualizerWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  if (root() == nullptr) {
    return;
  }
  if (enabled) {
    pullSpectrumValues();
  }
  if (applyVisibility()) {
    requestLayout();
  } else if (enabled && m_visible) {
    requestFrameTick();
    requestRedraw();
  }
}

bool DesktopAudioVisualizerWidget::needsFrameTick() const {
  return m_visualizer != nullptr &&
         (m_pendingSpectrumUpdate || (m_editorPreview && m_visible) || (m_visible && !m_visualizer->converged()) ||
          shouldBeVisible() != m_visible || m_fadingOut || m_visibilityAnimId != 0);
}

void DesktopAudioVisualizerWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  if (m_visualizer == nullptr) {
    return;
  }
  applyVisibility();
  if (!m_visible) {
    return;
  }
  syncSpectrum(&renderer);
  m_visualizer->tick(deltaMs);
}

void DesktopAudioVisualizerWidget::layoutContentSize(Renderer& renderer) {
  const float width = visualizerBaseWidth(m_aspectRatio) * m_contentScale;
  const float height = visualizerBaseHeight(m_aspectRatio) * m_contentScale;
  if (m_visualizer != nullptr) {
    if (m_visible) {
      syncSpectrum(&renderer);
    }
    m_visualizer->setPosition(0.0f, 0.0f);
    m_visualizer->setSize(width, height);
  }
  if (root() != nullptr) {
    root()->setSize(width, height);
  }
}

void DesktopAudioVisualizerWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr) {
    return;
  }
  applyVisibility();
  // Keep the desktop footprint at the design size even when idle-hidden. Collapsing
  // to 0x0 left only background padding visible as a tiny circle in the editor.
  layoutContentSize(renderer);
}

void DesktopAudioVisualizerWidget::doUpdate(Renderer& renderer) {
  if (applyVisibility()) {
    requestLayout();
  }
  syncSpectrum(&renderer);
}

void DesktopAudioVisualizerWidget::pullSpectrumValues() {
  if (m_visualizer == nullptr || m_spectrum == nullptr || m_listenerId == 0) {
    return;
  }

  const auto& spectrumValues = m_spectrum->values(m_listenerId);
  if (spectrumValues.empty()) {
    return;
  }

  m_visualizer->setValues(spectrumValues);
  m_pendingSpectrumUpdate = false;
}

void DesktopAudioVisualizerWidget::syncSpectrum(Renderer* /*renderer*/) {
  if (m_visualizer == nullptr || m_spectrum == nullptr || m_listenerId == 0) {
    return;
  }
  if (!m_pendingSpectrumUpdate && !m_editorPreview) {
    return;
  }

  pullSpectrumValues();
}

bool DesktopAudioVisualizerWidget::shouldBeVisible() const {
  if (m_spectrum == nullptr) {
    return false;
  }
  return m_editorPreview || m_showWhenIdle || !m_spectrum->idle();
}

bool DesktopAudioVisualizerWidget::applyVisibility() {
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
    return !m_visible;
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

void DesktopAudioVisualizerWidget::cancelVisibilityAnimation() {
  if (m_visibilityAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_visibilityAnimId);
  }
  m_visibilityAnimId = 0;
}

void DesktopAudioVisualizerWidget::setVisibilityCollapsed(bool collapsed) {
  if (root() == nullptr) {
    return;
  }
  if (m_visualizer != nullptr) {
    m_visualizer->setVisible(!collapsed);
  }
}

void DesktopAudioVisualizerWidget::startOpacityAnimation(float targetOpacity, bool collapseOnComplete) {
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
      root()->opacity(), targetOpacity, Style::animNormal, Easing::EaseOutCubic,
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
        if (root() != nullptr) {
          root()->markLayoutDirty();
        }
        requestLayout();
      },
      this
  );
  requestFrameTick();
}
