#include "shell/bar/widgets/audio_visualizer_widget.h"

#include "pipewire/pipewire_spectrum.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/style.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <memory>

AudioVisualizerWidget::AudioVisualizerWidget(PipeWireSpectrum* spectrum, Options options)
    : m_spectrum(spectrum), m_width(options.width), m_bands(std::max(1, options.bands)), m_mirrored(options.mirrored),
      m_centered(options.centered), m_showWhenIdle(options.showWhenIdle), m_color1(options.color1),
      m_color2(options.color2) {}

AudioVisualizerWidget::~AudioVisualizerWidget() {
  cancelVisibilityAnimation();
  if (m_spectrum != nullptr && m_listenerId != 0) {
    m_spectrum->removeChangeListener(m_listenerId);
  }
}

void AudioVisualizerWidget::create() {
  auto root = std::make_unique<InputArea>();
  root->setEnabled(false);
  root->setClipChildren(true);

  auto visualizer = std::make_unique<AudioVisualizer>();
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizer->setCentered(m_centered);
  visualizer->setMirrored(m_mirrored);
  visualizer->setGradient(m_color1, m_color2);
  m_visualizer = visualizer.get();
  root->addChild(std::move(visualizer));

  if (m_spectrum != nullptr) {
    m_listenerId = m_spectrum->addChangeListener(m_bands, [this]() {
      m_pendingSpectrumUpdate = true;
      requestFrameTick();
    });
  }

  setRoot(std::move(root));
}

void AudioVisualizerWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (root() == nullptr) {
    return;
  }
  applyVisibility();
  if (!m_visible) {
    root()->setParticipatesInLayout(false);
    return;
  }

  // containerWidth/Height are the bar's logical cross/main extents (not the widget slot).
  const bool barIsVertical = containerHeight > containerWidth;
  const float crossLimit = std::max(1.0f, barIsVertical ? containerWidth : containerHeight);
  const float bodyExtent = renderer.fontRowExtent(Style::fontSizeBody * m_contentScale);
  const float crossExtent = std::min(bodyExtent, crossLimit);
  const float width = std::max(1.0f, barIsVertical ? crossExtent : m_width * m_contentScale);
  const float height = std::max(1.0f, barIsVertical ? m_width * m_contentScale : crossExtent);
  if (m_visualizer != nullptr) {
    m_visualizer->setOrientation(
        barIsVertical ? AudioSpectrumOrientation::Vertical : AudioSpectrumOrientation::Horizontal
    );
    m_visualizer->setPosition(0.0f, 0.0f);
    m_visualizer->setSize(width, height);
  }
  root()->setSize(width, height);
}

void AudioVisualizerWidget::doUpdate(Renderer& /*renderer*/) {
  if (applyVisibility()) {
    if (root() != nullptr) {
      root()->markLayoutDirty();
    }
    requestUpdate();
  }
  syncSpectrum();
}

void AudioVisualizerWidget::onFrameTick(float deltaMs) {
  if (m_visualizer == nullptr) {
    return;
  }
  if (applyVisibility()) {
    requestUpdate();
  }
  if (!m_visible) {
    return;
  }
  syncSpectrum();
  m_visualizer->tick(deltaMs);
  if (m_visible && (!m_visualizer->converged() || (m_spectrum != nullptr && !m_spectrum->idle()))) {
    requestRedraw();
  }
}

bool AudioVisualizerWidget::needsFrameTick() const {
  if (m_visualizer == nullptr) {
    return false;
  }
  if (m_pendingSpectrumUpdate || shouldBeVisible() != m_visible || m_fadingOut || m_visibilityAnimId != 0) {
    return true;
  }
  if (!m_visible) {
    return false;
  }
  if (!m_visualizer->converged()) {
    return true;
  }
  // Keep ticking while audio is active so every visualizer reads fresh band data.
  return m_spectrum != nullptr && !m_spectrum->idle();
}

void AudioVisualizerWidget::syncSpectrum() {
  if (m_visualizer == nullptr || m_spectrum == nullptr || m_listenerId == 0) {
    return;
  }
  const bool shouldPull = m_pendingSpectrumUpdate || (m_visible && !m_spectrum->idle());
  if (!shouldPull) {
    return;
  }

  m_visualizer->setValues(m_spectrum->values(m_listenerId));
  m_pendingSpectrumUpdate = false;
}

bool AudioVisualizerWidget::shouldBeVisible() const {
  return m_spectrum != nullptr && (m_showWhenIdle || !m_spectrum->idle());
}

bool AudioVisualizerWidget::applyVisibility() {
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

void AudioVisualizerWidget::cancelVisibilityAnimation() {
  if (m_visibilityAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_visibilityAnimId);
  }
  m_visibilityAnimId = 0;
}

void AudioVisualizerWidget::setVisibilityCollapsed(bool collapsed) {
  if (root() == nullptr) {
    return;
  }
  root()->setVisible(!collapsed);
  root()->setParticipatesInLayout(!collapsed);
  if (m_visualizer != nullptr) {
    m_visualizer->setVisible(!collapsed);
  }
}

void AudioVisualizerWidget::startOpacityAnimation(float targetOpacity, bool collapseOnComplete) {
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
        requestUpdate();
      },
      this
  );
  requestRedraw();
}
