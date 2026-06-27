#include "ui/visuals/fancy_audio_visualizer.h"

#include "ui/palette.h"

#include <algorithm>
#include <cmath>

FancyAudioVisualizer::FancyAudioVisualizer() {
  syncPalette();
  m_paletteConn = paletteChanged().connect([this] { syncPalette(); });
}

bool FancyAudioVisualizer::setValues(TextureManager& textures, std::span<const float> values) {
  return setSpectrumValues(textures, values);
}

bool FancyAudioVisualizer::setValues(TextureManager& textures, const std::vector<float>& values) {
  return setValues(textures, std::span<const float>(values.data(), values.size()));
}

void FancyAudioVisualizer::setVisualizationMode(FancyAudioVisualizerMode mode) {
  auto next = style();
  next.mode = mode;
  setStyle(next);
}

void FancyAudioVisualizer::setPrimaryColor(const ColorSpec& color) {
  m_primaryColor = color;
  syncPalette();
}

void FancyAudioVisualizer::setSecondaryColor(const ColorSpec& color) {
  m_secondaryColor = color;
  syncPalette();
}

void FancyAudioVisualizer::setSensitivity(float sensitivity) {
  auto next = style();
  next.sensitivity = std::clamp(sensitivity, 0.5f, 3.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setRotationSpeed(float speed) {
  auto next = style();
  next.rotationSpeed = std::clamp(speed, 0.0f, 2.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setBarWidth(float width) {
  auto next = style();
  next.barWidth = std::clamp(width, 0.2f, 1.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setRingOpacity(float opacity) {
  auto next = style();
  next.ringOpacity = std::clamp(opacity, 0.0f, 1.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setBloomIntensity(float intensity) {
  auto next = style();
  next.bloomIntensity = std::clamp(intensity, 0.0f, 1.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setWaveThickness(float thickness) {
  auto next = style();
  next.waveThickness = std::clamp(thickness, 0.3f, 2.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setInnerDiameter(float diameter) {
  auto next = style();
  next.innerDiameter = std::clamp(diameter, 0.0f, 1.0f);
  setStyle(next);
}

void FancyAudioVisualizer::setCornerRadius(float radius) {
  auto next = style();
  next.cornerRadius = std::max(0.0f, radius);
  setStyle(next);
}

void FancyAudioVisualizer::setTime(float time) {
  auto next = style();
  next.time = std::fmod(std::max(0.0f, time), 3600.0f);
  setStyle(next);
}

void FancyAudioVisualizer::syncPalette() {
  auto next = style();
  next.primaryColor = resolveColorSpec(m_primaryColor);
  next.secondaryColor = resolveColorSpec(m_secondaryColor);
  setStyle(next);
}

void FancyAudioVisualizer::doLayout(Renderer& /*renderer*/) {}
