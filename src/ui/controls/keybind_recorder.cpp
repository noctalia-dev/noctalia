#include "ui/controls/keybind_recorder.h"

#include "core/key_chord.h"
#include "core/key_modifiers.h"
#include "core/key_symbols.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

namespace {

  constexpr float kRecorderMinWidth = 220.0f;

} // namespace

KeybindRecorder::KeybindRecorder() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Style::spaceXs);
  setPadding(Style::spaceXs, Style::spaceSm);
  setRadius(Style::scaledRadiusMd());
  setMinHeight(Style::controlHeightSm);
  setMinWidth(kRecorderMinWidth);

  auto label = std::make_unique<Label>();
  label->setFontSize(Style::fontSizeCaption);
  label->setColor(colorSpecFromRole(ColorRole::OnSurface));
  m_label = static_cast<Label*>(addChild(std::move(label)));

  auto glyph = std::make_unique<Glyph>();
  glyph->setGlyph("keyboard");
  glyph->setGlyphSize(Style::fontSizeCaption);
  glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setOnFocusGain([this]() { enterRecording(); });
  area->setOnFocusLoss([this]() { exitRecording(false); });
  area->setOnPress([this](const InputArea::PointerData& data) {
    // Re-clicking when already focused does not refire focus gain.
    if (data.pressed && m_enabled && !m_recording) {
      enterRecording();
    }
  });
  area->setOnKeyDown([this](const InputArea::KeyData& data) {
    if (data.preedit) {
      return;
    }
    handleKeyDown(data.sym, data.modifiers);
  });
  area->setOnKeyUp([this](const InputArea::KeyData& data) {
    if (data.preedit) {
      return;
    }
    handleKeyUp(data.sym, data.modifiers);
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);
  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(width(), height());

  applyVisualState(VisualState::Idle);
  refreshLabel();
}

KeybindRecorder::~KeybindRecorder() = default;

void KeybindRecorder::setChord(std::optional<KeyChord> chord) {
  m_chord = chord;
  refreshLabel();
}

void KeybindRecorder::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  setGap(Style::spaceXs * m_scale);
  setPadding(Style::spaceXs * m_scale, Style::spaceSm * m_scale);
  setRadius(Style::scaledRadiusMd(m_scale));
  setMinHeight(Style::controlHeightSm * m_scale);
  setMinWidth(kRecorderMinWidth * m_scale);
  if (m_label != nullptr) {
    m_label->setFontSize(Style::fontSizeCaption * m_scale);
  }
  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::fontSizeCaption * m_scale);
  }
}

void KeybindRecorder::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  if (!enabled && m_recording) {
    exitRecording(false);
  }
  applyVisualState(m_visualState);
}

void KeybindRecorder::setUnsetPlaceholder(std::string_view text) {
  m_unsetPlaceholder = std::string(text);
  refreshLabel();
}

void KeybindRecorder::setRecordingPlaceholder(std::string_view text) {
  m_recordingPlaceholder = std::string(text);
  refreshLabel();
}

void KeybindRecorder::setOnCommit(std::function<void(KeyChord)> callback) { m_onCommit = std::move(callback); }

void KeybindRecorder::setModifierPolicy(ModifierPolicy policy) { m_modifierPolicy = policy; }

void KeybindRecorder::doLayout(Renderer& renderer) {
  if (m_label != nullptr) {
    m_label->measure(renderer);
  }
  if (m_glyph != nullptr) {
    m_glyph->measure(renderer);
  }
  Flex::doLayout(renderer);
  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize KeybindRecorder::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void KeybindRecorder::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void KeybindRecorder::handleKeyDown(std::uint32_t sym, std::uint32_t modifiers) {
  if (!m_recording) {
    return;
  }

  // Must run before the modifier-preview branch so a bare Super press is also caught.
  if ((modifiers & KeyMod::Super) != 0 || KeySymbol::isSuperModifier(sym)) {
    notify::error(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.keybind-invalid-title"),
        i18n::tr("notifications.internal.keybind-invalid-super")
    );
    exitRecording(false);
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Forbidden && KeySymbol::isModifier(sym)) {
    notify::error(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.keybind-invalid-title"),
        i18n::tr("notifications.internal.keybind-invalid-modifier")
    );
    exitRecording(false);
    return;
  }

  if (KeySymbol::isModifier(sym)) {
    m_pendingModifiers = modifiers;
    refreshLabel();
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Required && modifiers == 0 && isPrintableKey(sym)) {
    notify::error(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.keybind-invalid-title"),
        i18n::tr("notifications.internal.keybind-invalid-printable")
    );
    exitRecording(false);
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Forbidden && modifiers != 0) {
    notify::error(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.keybind-invalid-title"),
        i18n::tr("notifications.internal.keybind-invalid-modifier")
    );
    exitRecording(false);
    return;
  }

  // Shift-only + printable is just a character variant (e.g. Shift+l = L),
  // not a meaningful shortcut. Reject it in both Required and Optional modes.
  if (modifiers == KeyMod::Shift && isPrintableKey(sym)) {
    notify::error(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.keybind-invalid-title"),
        i18n::tr("notifications.internal.keybind-invalid-printable")
    );
    exitRecording(false);
    return;
  }

  const std::uint32_t cleanModifiers = modifiers & ~KeyMod::Super;
  KeyChord chord{.sym = sym, .modifiers = cleanModifiers};
  m_chord = chord;
  if (m_onCommit) {
    m_onCommit(chord);
  }
  exitRecording(true);
}

void KeybindRecorder::handleKeyUp(std::uint32_t sym, std::uint32_t modifiers) {
  if (!m_recording) {
    return;
  }
  if (KeySymbol::isModifier(sym)) {
    m_pendingModifiers = modifiers;
    refreshLabel();
  }
}

void KeybindRecorder::enterRecording() {
  if (!m_enabled || m_recording) {
    return;
  }
  m_recording = true;
  m_pendingModifiers = 0;
  applyVisualState(VisualState::Recording);
  refreshLabel();
}

void KeybindRecorder::exitRecording(bool /*commit*/) {
  if (!m_recording) {
    return;
  }
  m_recording = false;
  m_pendingModifiers = 0;
  applyVisualState(VisualState::Idle);
  refreshLabel();
}

void KeybindRecorder::refreshLabel() {
  if (m_label == nullptr) {
    return;
  }

  if (m_recording) {
    if (m_pendingModifiers != 0) {
      const KeyChord preview{.sym = 0, .modifiers = m_pendingModifiers};
      std::string text;
      if ((preview.modifiers & KeyMod::Ctrl) != 0) {
        text += "Ctrl + ";
      }
      if ((preview.modifiers & KeyMod::Alt) != 0) {
        text += "Alt + ";
      }
      if ((preview.modifiers & KeyMod::Shift) != 0) {
        text += "Shift + ";
      }
      text += "…";
      m_label->setText(text);
    } else {
      m_label->setText(m_recordingPlaceholder);
    }
  } else if (m_chord.has_value() && m_chord->sym != 0) {
    m_label->setText(keyChordDisplayLabel(*m_chord));
  } else {
    m_label->setText(m_unsetPlaceholder);
  }
}

void KeybindRecorder::applyVisualState(VisualState state) {
  m_visualState = state;
  if (!m_enabled) {
    setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.4f));
    setBorder(colorSpecFromRole(ColorRole::Outline, 0.4f), Style::borderWidth);
    if (m_label != nullptr) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55f));
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55f));
    }
    return;
  }

  switch (state) {
  case VisualState::Idle:
    setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    if (m_label != nullptr) {
      const bool placeholder = !m_chord.has_value() || m_chord->sym == 0;
      m_label->setColor(
          placeholder ? colorSpecFromRole(ColorRole::OnSurfaceVariant) : colorSpecFromRole(ColorRole::OnSurface)
      );
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }
    break;
  case VisualState::Recording:
    setFill(colorSpecFromRole(ColorRole::Primary, 0.12f));
    setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    if (m_label != nullptr) {
      m_label->setColor(colorSpecFromRole(ColorRole::Primary));
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::Primary));
    }
    break;
  }
}
