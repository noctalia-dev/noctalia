#include "shell/lockscreen/lock_surface.h"

#include "core/ui_phase.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/wallpaper_node.h"
#include "time/time_format.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <wayland-client.h>

namespace {

  const ext_session_lock_surface_v1_listener kLockSurfaceListener = {
      .configure = &LockSurface::handleConfigure,
  };

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  const char* shellTimeFormat(const ConfigService* config) {
    return config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
  }

} // namespace

LockSurface::LockSurface(WaylandConnection& connection, ConfigService* config) : Surface(connection), m_config(config) {
  auto wallpaper = std::make_unique<WallpaperNode>();
  m_wallpaper = static_cast<WallpaperNode*>(m_root.addChild(std::move(wallpaper)));
  m_wallpaper->setZIndex(0);

  auto backdrop = std::make_unique<Box>();
  m_backdrop = static_cast<Box*>(m_root.addChild(std::move(backdrop)));
  m_backdrop->setZIndex(-1);

  auto clockShadow = std::make_unique<Label>();
  m_clockShadow = static_cast<Label*>(m_root.addChild(std::move(clockShadow)));

  auto clock = std::make_unique<Label>();
  clock->setColor(colorSpecFromRole(ColorRole::Primary));
  m_clock = static_cast<Label*>(m_root.addChild(std::move(clock)));

  auto loginPanel = std::make_unique<Box>();
  m_loginPanel = static_cast<Box*>(m_root.addChild(std::move(loginPanel)));

  auto passwordField = std::make_unique<Input>();
  passwordField->setPlaceholder(i18n::tr("lockscreen.password-placeholder"));
  passwordField->setPasswordMode(true);
  passwordField->setOnChange([this](const std::string& value) {
    if (m_onPasswordChanged) {
      m_onPasswordChanged(value);
    }
  });
  passwordField->setOnSubmit([this](const std::string& /*value*/) {
    if (m_onLogin) {
      m_onLogin();
    }
  });
  m_passwordField = static_cast<Input*>(m_root.addChild(std::move(passwordField)));

  auto loginButton = std::make_unique<Button>();
  loginButton->setText("");
  loginButton->setGlyph("check");
  loginButton->setGlyphSize(16.0f);
  loginButton->setVariant(ButtonVariant::Primary);
  loginButton->setOnClick([this]() {
    if (m_onLogin) {
      m_onLogin();
    }
  });
  m_loginButton = static_cast<Button*>(m_root.addChild(std::move(loginButton)));

  m_inputDispatcher.setSceneRoot(&m_root);
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.setCursorShape(serial, shape);
  });

  setSceneRoot(&m_root);
  setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) { requestLayout(); });
  setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
  requestUpdate();
}

LockSurface::~LockSurface() {
  if (m_textureCache != nullptr && m_wallpaperTexture.id != 0) {
    m_textureCache->release(m_wallpaperTexture, m_wallpaperPath);
  }
  m_connection.unregisterSurface(m_surface);
  if (m_lockSurface != nullptr) {
    ext_session_lock_surface_v1_destroy(m_lockSurface);
    m_lockSurface = nullptr;
  }
}

bool LockSurface::initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale) {
  if (lock == nullptr || output == nullptr || renderContext() == nullptr) {
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  m_output = output;
  m_connection.registerSurfaceOutput(m_surface, output);
  setBufferScale(scale);

  m_lockSurface = ext_session_lock_v1_get_lock_surface(lock, m_surface, output);
  if (m_lockSurface == nullptr) {
    destroySurface();
    return false;
  }

  if (ext_session_lock_surface_v1_add_listener(m_lockSurface, &kLockSurfaceListener, this) != 0) {
    ext_session_lock_surface_v1_destroy(m_lockSurface);
    m_lockSurface = nullptr;
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void LockSurface::setLockedState(bool locked) {
  if (m_locked == locked) {
    return;
  }
  m_locked = locked;
  if (m_locked && m_passwordField != nullptr) {
    m_inputDispatcher.setFocus(m_passwordField->inputArea());
  } else {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestUpdate();
}

void LockSurface::setPromptState(std::string user, std::string password, std::string status, bool error) {
  if (m_user == user && m_password == password && m_status == status && m_error == error) {
    return;
  }
  m_user = std::move(user);
  m_password = std::move(password);
  m_status = std::move(status);
  m_error = error;
  requestUpdate();
}

void LockSurface::setWallpaperPath(std::string wallpaperPath) {
  if (m_wallpaperPath == wallpaperPath) {
    return;
  }
  if (m_textureCache != nullptr && m_wallpaperTexture.id != 0) {
    m_textureCache->release(m_wallpaperTexture, m_wallpaperPath);
  }
  m_wallpaperPath = std::move(wallpaperPath);
  m_wallpaperTexture = {};
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::setWallpaperFillMode(WallpaperFillMode fillMode) {
  if (m_wallpaperFillMode == fillMode) {
    return;
  }
  m_wallpaperFillMode = fillMode;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillMode(m_wallpaperFillMode);
  }
  requestRedraw();
}

void LockSurface::setWallpaperFillColor(Color fillColor) {
  if (m_wallpaperFillColor == fillColor) {
    return;
  }
  m_wallpaperFillColor = fillColor;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  }
  if (m_backdrop != nullptr) {
    m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0f);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = m_wallpaperFillColor,
            .fillMode = FillMode::Solid,
        }
    );
  }
  requestRedraw();
}

void LockSurface::setOnLogin(std::function<void()> onLogin) { m_onLogin = std::move(onLogin); }

void LockSurface::setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged) {
  m_onPasswordChanged = std::move(onPasswordChanged);
}

void LockSurface::selectAllPassword() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->selectAll();
  requestLayout();
}

void LockSurface::clearPasswordSelection() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->clearSelection();
  requestLayout();
}

void LockSurface::onPointerEvent(const PointerEvent& event) {
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Button:
    m_inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button,
        event.state == WL_POINTER_BUTTON_STATE_PRESSED
    );
    break;
  case PointerEvent::Type::Axis:
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::onSecondTick() {
  const auto text = formatLocalTime(shellTimeFormat(m_config));
  if (m_clock != nullptr && m_clock->text() != text) {
    requestUpdate();
  }
}

void LockSurface::onThemeChanged() { requestLayout(); }

void LockSurface::onKeyboardEvent(const KeyboardEvent& event) {
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::handleConfigure(
    void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
    std::uint32_t height
) {
  auto* self = static_cast<LockSurface*>(data);
  ext_session_lock_surface_v1_ack_configure(lockSurface, serial);
  self->Surface::onConfigure(width, height);
}

void LockSurface::prepareFrame(bool needsUpdate, bool needsLayout) {
  auto* renderer = renderContext();
  if (renderer == nullptr || width() == 0 || height() == 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateCopy();
  }

  if (needsUpdate || needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutScene(width(), height());
  }
}

void LockSurface::layoutScene(std::uint32_t width, std::uint32_t height) {
  auto* renderer = renderContext();
  if (renderer == nullptr) {
    return;
  }
  applyWallpaperTexture();

  const float sw = static_cast<float>(width);
  const float sh = static_cast<float>(height);
  const float panelWidth = std::min(sw - Style::spaceLg * 2.0f, 520.0f);
  const float panelHeight = 78.0f;
  const float panelX = std::round((sw - panelWidth) * 0.5f);
  const float panelY = std::max(Style::spaceLg, sh - panelHeight - 84.0f);

  m_root.setSize(sw, sh);

  m_wallpaper->setPosition(0.0f, 0.0f);
  m_wallpaper->setSize(sw, sh);
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(m_wallpaperFillColor);

  m_backdrop->setPosition(0.0f, 0.0f);
  m_backdrop->setSize(sw, sh);
  m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0f);
  m_backdrop->setStyle(
      RoundedRectStyle{
          .fill = m_wallpaperFillColor,
          .fillMode = FillMode::Solid,
      }
  );

  constexpr float kClockFontSize = 64.0f;
  m_clock->setFontSize(kClockFontSize);
  m_clock->setFontWeight(FontWeight::Bold);
  m_clock->measure(*renderer);
  const float clockX = sw - 48.0f - m_clock->width();
  const float clockY = 86.0f;

  m_clockShadow->setVisible(m_clockShadowEnabled);
  m_clockShadow->setFontSize(kClockFontSize);
  m_clockShadow->setFontWeight(FontWeight::Bold);
  m_clockShadow->setColor(colorSpecFromRole(ColorRole::Shadow, 0.55f));
  m_clockShadow->setText(m_clock->text());
  m_clockShadow->measure(*renderer);
  m_clockShadow->setPosition(clockX + 3.0f, clockY + 4.0f);
  m_clock->setPosition(clockX, clockY);

  m_loginPanel->setPosition(panelX, panelY);
  m_loginPanel->setSize(panelWidth, panelHeight);
  m_loginPanel->setStyle(
      RoundedRectStyle{
          .fill = colorForRole(ColorRole::SurfaceVariant, 0.88f),
          .border = colorForRole(ColorRole::Outline, 0.95f),
          .fillMode = FillMode::Solid,
          .radius = Style::scaledRadiusXl(),
          .softness = 1.0f,
          .borderWidth = Style::borderWidth,
      }
  );

  const float contentLeft = panelX + Style::spaceLg;
  const float contentTop = panelY + 22.0f;
  const float rightInset = Style::spaceLg + Style::spaceSm;
  const float contentWidth = panelWidth - Style::spaceLg - rightInset;
  const float buttonWidth = Style::controlHeight;
  const float gap = Style::spaceSm;
  const float inputWidth = std::max(120.0f, contentWidth - buttonWidth - gap);

  m_passwordField->setSize(inputWidth, 0.0f);
  m_passwordField->setPosition(contentLeft, contentTop);
  m_passwordField->layout(*renderer);

  m_loginButton->setSize(buttonWidth, Style::controlHeight);
  m_loginButton->setPosition(contentLeft + inputWidth + gap, contentTop);
  m_loginButton->layout(*renderer);
}

void LockSurface::updateCopy() {
  m_passwordField->setValue(m_password);
  updateClockText();
}

void LockSurface::applyWallpaperTexture() {
  if (!m_wallpaperDirty) {
    return;
  }

  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  if (parseColorWallpaperPath(m_wallpaperPath, color)) {
    m_wallpaperTexture = {};
    m_wallpaper->setSources(
        WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.0f,
        0.0f, 0.0f
    );
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  } else if (m_textureCache != nullptr && !m_wallpaperPath.empty()) {
    m_wallpaperTexture = m_textureCache->acquire(m_wallpaperPath);
    m_wallpaper->setTextures(
        m_wallpaperTexture.id, {}, static_cast<float>(m_wallpaperTexture.width),
        static_cast<float>(m_wallpaperTexture.height), 0.0f, 0.0f
    );
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  } else {
    m_wallpaperTexture = {};
    m_wallpaper->setTextures({}, {}, 0.0f, 0.0f, 0.0f, 0.0f);
  }

  m_wallpaperDirty = false;
}

void LockSurface::updateClockText() { m_clock->setText(formatLocalTime(shellTimeFormat(m_config))); }
