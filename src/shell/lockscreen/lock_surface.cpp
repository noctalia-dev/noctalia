#include "shell/lockscreen/lock_surface.h"

#include "capture/screencopy_capture.h"
#include "core/ui_phase.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/lockscreen/lockscreen_widgets_host.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>

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

} // namespace

LockSurface::LockSurface(WaylandConnection& connection, ConfigService* config) : Surface(connection), m_config(config) {
  {
    auto backgroundLayer = std::make_unique<Node>();
    backgroundLayer->setZIndex(0);
    m_backgroundLayer = m_root.addChild(std::move(backgroundLayer));
  }

  auto wallpaper = std::make_unique<WallpaperNode>();
  m_wallpaper = static_cast<WallpaperNode*>(m_backgroundLayer->addChild(std::move(wallpaper)));
  m_wallpaper->setZIndex(0);

  m_backgroundLayer->addChild(
      ui::box({
          .out = &m_tintOverlay,
          .visible = false,
          .configure = [](Box& box) { box.setZIndex(1); },
      })
  );

  m_backgroundLayer->addChild(
      ui::box({
          .out = &m_backdrop,
          .configure = [](Box& box) { box.setZIndex(-1); },
      })
  );

  {
    auto widgetLayer = std::make_unique<Node>();
    widgetLayer->setZIndex(2);
    m_widgetLayer = m_root.addChild(std::move(widgetLayer));
  }

  m_root.addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_loginPanel,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Center,
              .gap = Style::spaceXs,
              .paddingV = 0.0f,
              .paddingH = Style::spaceLg,
              .configure = [](Flex& flex) { flex.setZIndex(2); },
          }
      )
  );

  m_loginPanel->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_loginContentRow,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
          }
      )
  );

  m_loginContentRow->addChild(
      ui::button({
          .out = &m_layoutChip,
          .text = "",
          .fontSize = Style::fontSizeCaption,
          .variant = ButtonVariant::Secondary,
          .visible = false,
          .onClick =
              [this]() {
                if (m_onCycleLayout) {
                  m_onCycleLayout();
                }
              },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  m_loginContentRow->addChild(
      ui::input({
          .out = &m_passwordField,
          .placeholder = i18n::tr("lockscreen.password-placeholder"),
          .passwordMode = true,
          .onChange =
              [this](const std::string& value) {
                if (m_onPasswordChanged) {
                  m_onPasswordChanged(value);
                }
              },
          .onSubmit =
              [this](const std::string& /*value*/) {
                if (m_onLogin) {
                  m_onLogin();
                }
              },
          .configure =
              [](Input& input) {
                input.setZIndex(2);
                input.setFlexGrow(1.0f);
              },
      })
  );

  m_loginContentRow->addChild(
      ui::button({
          .out = &m_loginButton,
          .text = "",
          .glyph = "check",
          .glyphSize = 16.0f,
          .variant = ButtonVariant::Primary,
          .onClick =
              [this]() {
                if (m_onLogin) {
                  m_onLogin();
                }
              },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  m_loginPanel->addChild(
      ui::label({
          .out = &m_statusLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Center,
          .visible = false,
          .configure = [](Label& label) { label.setZIndex(2); },
      })
  );

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
  releaseCaptureTextures();
  if (m_wallpaperTexture.id != 0) {
    releaseWallpaperTextureRef(m_textureWallpaperPath);
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
  m_inputDispatcher.setTextInputContext(m_surface, m_connection.textInputService());

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
  if (m_locked) {
    focusPasswordField();
  } else {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestUpdate();
}

bool LockSurface::passwordFieldContainsPoint(float sceneX, float sceneY) const {
  return m_passwordField != nullptr && m_passwordField->containsScenePoint(sceneX, sceneY);
}

void LockSurface::focusPasswordField() {
  if (!m_locked || m_blackout || m_passwordField == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(m_passwordField->inputArea());
}

void LockSurface::setPromptState(
    std::string user, std::string password, std::string status, bool error, bool authenticating
) {
  if (m_user == user
      && m_password == password
      && m_status == status
      && m_error == error
      && m_authenticating == authenticating) {
    return;
  }
  m_user = std::move(user);
  m_password = std::move(password);
  m_status = std::move(status);
  m_error = error;
  m_authenticating = authenticating;
  requestUpdate();
}

void LockSurface::setKeyboardIndicators(
    bool capsLock, bool hasMultipleLayouts, bool layoutSwitchable, std::string layoutLabel
) {
  if (m_capsLock == capsLock
      && m_hasMultipleLayouts == hasMultipleLayouts
      && m_layoutSwitchable == layoutSwitchable
      && m_layoutLabel == layoutLabel) {
    return;
  }
  m_capsLock = capsLock;
  m_hasMultipleLayouts = hasMultipleLayouts;
  m_layoutSwitchable = layoutSwitchable;
  m_layoutLabel = std::move(layoutLabel);
  requestUpdate();
}

void LockSurface::setWallpaperPath(std::string wallpaperPath) {
  if (m_wallpaperPath == wallpaperPath) {
    return;
  }

  if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }

  // Keep the current wallpaper visible until applyWallpaperTexture() loads the new path.
  m_wallpaperPath = std::move(wallpaperPath);
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

void LockSurface::setDesktopCapture(std::optional<ScreencopyImage> capture) {
  m_desktopCapture = std::move(capture);
  m_captureDirty = true;
  releaseCaptureTextures();
  requestLayout();
}

bool LockSurface::hasDesktopCapture() const noexcept {
  return m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty();
}

void LockSurface::setBackgroundStyle(float blurIntensity, float tintIntensity) {
  if (m_blurIntensity == blurIntensity && m_tintIntensity == tintIntensity) {
    return;
  }
  m_blurIntensity = blurIntensity;
  m_tintIntensity = tintIntensity;
  m_captureDirty = true;
  m_blurCache.invalidate();
  m_wallpaperDirty = true;
  m_wallpaperBlurCache.invalidate();
  requestLayout();
}

void LockSurface::setBlackout(bool blackout) {
  if (m_blackout == blackout) {
    return;
  }
  m_blackout = blackout;
  if (m_blackout) {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestLayout();
}

void LockSurface::setOnLogin(std::function<void()> onLogin) { m_onLogin = std::move(onLogin); }

void LockSurface::setOnCycleLayout(std::function<void()> onCycleLayout) { m_onCycleLayout = std::move(onCycleLayout); }

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
  if (m_blackout) {
    return;
  }

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
  case PointerEvent::Type::Button: {
    const bool pressed = event.state == WL_POINTER_BUTTON_STATE_PRESSED;
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
    }
    m_inputDispatcher.pointerButton(x, y, event.button, pressed);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
      requestRedraw();
    }
    break;
  }
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

void LockSurface::onThemeChanged() {
  m_captureDirty = true;
  requestLayout();
}

void LockSurface::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_blackout) {
    return;
  }

  if (m_locked
      && event.pressed
      && m_passwordField != nullptr
      && m_inputDispatcher.focusedArea() != m_passwordField->inputArea()) {
    focusPasswordField();
  }
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
  if (self->width() != width || self->height() != height) {
    self->m_firstFrameRendered = false;
  }
  ext_session_lock_surface_v1_ack_configure(lockSurface, serial);
  self->Surface::onConfigure(width, height);
}

void LockSurface::prepareFrame(bool needsUpdate, bool needsLayout) {
  auto* renderer = renderContext();
  if (renderer == nullptr || width() == 0 || height() == 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());

  if (m_widgetsHost != nullptr) {
    m_widgetsHost->prepareFrame(*this, needsUpdate, needsLayout);
  }

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

  const auto sw = static_cast<float>(width);
  const auto sh = static_cast<float>(height);

  if (m_blackout) {
    m_root.setSize(sw, sh);
    m_backgroundLayer->setPosition(0.0f, 0.0f);
    m_backgroundLayer->setSize(sw, sh);
    m_wallpaper->setVisible(false);
    m_tintOverlay->setVisible(false);
    m_backdrop->setPosition(0.0f, 0.0f);
    m_backdrop->setSize(sw, sh);
    m_backdrop->setVisible(true);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = rgba(0.0f, 0.0f, 0.0f, 1.0f),
            .fillMode = FillMode::Solid,
        }
    );
    m_widgetLayer->setVisible(false);
    m_loginPanel->setVisible(false);
    m_passwordField->setVisible(false);
    m_loginButton->setVisible(false);
    if (m_layoutChip != nullptr) {
      m_layoutChip->setVisible(false);
    }
    if (m_statusLabel != nullptr) {
      m_statusLabel->setVisible(false);
    }
    return;
  }

  applyWallpaperTexture();

  m_wallpaper->setVisible(true);
  m_widgetLayer->setVisible(true);
  const bool loginVisible = isLoginBoxEnabled();
  m_loginPanel->setVisible(loginVisible);
  m_loginContentRow->setVisible(loginVisible);
  m_passwordField->setVisible(loginVisible);
  m_loginButton->setVisible(loginVisible && resolveLoginStyle().showLoginButton);
  float panelHeight = lockscreen_login_box::defaultPanelHeight();
  float panelWidth = lockscreen_login_box::defaultPanelWidth(sw);
  float panelX = std::round((sw - panelWidth) * 0.5f);
  float panelY = std::max(Style::spaceLg, sh - panelHeight - 84.0f);
  if (m_config != nullptr) {
    if (const DesktopWidgetState* loginBox =
            lockscreen_login_box::findForOutput(m_config->config().lockscreenWidgets.widgets, m_outputKey);
        loginBox != nullptr) {
      float cx = loginBox->cx;
      float cy = loginBox->cy;
      lockscreen_login_box::panelOriginFromCenter(
          cx, cy, sw, loginBox->boxWidth, loginBox->boxHeight, panelX, panelY, panelWidth, panelHeight
      );
    }
  }

  panelX = std::clamp(panelX, Style::spaceLg, sw - panelWidth - Style::spaceLg);
  panelY = std::clamp(panelY, Style::spaceLg, sh - panelHeight - Style::spaceLg);

  m_root.setSize(sw, sh);

  m_backgroundLayer->setPosition(0.0f, 0.0f);
  m_backgroundLayer->setSize(sw, sh);

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

  if (m_tintOverlay != nullptr) {
    m_tintOverlay->setPosition(0.0f, 0.0f);
    m_tintOverlay->setSize(sw, sh);
    const float tintIntensity = m_tintIntensity;
    const bool showTint = tintIntensity > 0.0f;
    m_tintOverlay->setVisible(showTint);
    if (showTint) {
      m_tintOverlay->setStyle(
          RoundedRectStyle{
              .fill = colorForRole(ColorRole::Surface, tintIntensity),
              .fillMode = FillMode::Solid,
          }
      );
    }
  }

  const lockscreen_login_box::LoginBoxStyle loginStyle = resolveLoginStyle();

  m_loginPanel->setFill(loginStyle.panelFill);
  m_loginPanel->setBorder(colorForRole(ColorRole::Outline), Style::borderWidth);
  m_loginPanel->setRadius(Style::scaledRadius(loginStyle.panelRadius));
  m_loginPanel->setSoftness(1.0f);

  const float controlHeight = std::clamp(panelHeight - Style::spaceSm * 2.0f, 0.0f, Style::controlHeight);

  m_loginContentRow->setMinHeight(controlHeight);

  const bool showChip = loginVisible && m_layoutChip != nullptr && m_layoutChip->visible();
  if (showChip) {
    const float rowContentWidth = std::max(0.0f, panelWidth - 2.0f * Style::spaceLg);
    m_layoutChip->setRadius(Style::scaledRadius(loginStyle.inputRadius));
    m_layoutChip->setMaxWidth(rowContentWidth * 0.5f);
  }

  m_passwordField->setSurfaceOpacity(loginStyle.inputOpacity);
  m_passwordField->setFrameRadius(loginStyle.inputRadius);

  const bool showLoginButton = loginVisible && loginStyle.showLoginButton;
  m_loginButton->setVisible(showLoginButton);
  if (showLoginButton) {
    m_loginButton->setRadius(Style::scaledRadius(loginStyle.inputRadius));
    m_loginButton->setSize(controlHeight, controlHeight);
  }

  if (m_statusLabel != nullptr && m_statusLabel->visible()) {
    m_statusLabel->setMaxWidth(std::max(0.0f, panelWidth - 2.0f * Style::spaceLg));
  }

  m_loginPanel->arrange(*renderer, LayoutRect{panelX, panelY, panelWidth, panelHeight});
}

lockscreen_login_box::LoginBoxStyle LockSurface::resolveLoginStyle() const {
  if (m_config == nullptr) {
    return lockscreen_login_box::LoginBoxStyle{};
  }
  if (const DesktopWidgetState* loginBox =
          lockscreen_login_box::findForOutput(m_config->config().lockscreenWidgets.widgets, m_outputKey);
      loginBox != nullptr) {
    return lockscreen_login_box::resolveStyle(loginBox->settings);
  }
  return lockscreen_login_box::LoginBoxStyle{};
}

bool LockSurface::isLoginBoxEnabled() const {
  if (m_config == nullptr) {
    return true;
  }
  if (const DesktopWidgetState* loginBox =
          lockscreen_login_box::findForOutput(m_config->config().lockscreenWidgets.widgets, m_outputKey);
      loginBox != nullptr) {
    return loginBox->enabled;
  }
  return true;
}

std::string LockSurface::resolveStatusText(const lockscreen_login_box::LoginBoxStyle& style, bool& isError) const {
  isError = false;
  // A live authentication/error message always wins, then any other transient status
  // (e.g. "password cleared"), then the caps-lock warning, then the idle password hint.
  if (m_authenticating || m_error) {
    isError = m_error;
    return m_status;
  }
  if (!m_status.empty()) {
    return m_status;
  }
  if (m_capsLock && style.showCapsLock) {
    isError = true;
    return i18n::tr("lockscreen.caps-lock-on");
  }
  if (style.showPasswordHint) {
    return i18n::tr("lockscreen.ready");
  }
  return {};
}

void LockSurface::updateCopy() {
  m_passwordField->setValue(m_password);
  m_passwordField->setEnabled(!m_authenticating);
  if (m_loginButton != nullptr) {
    m_loginButton->setEnabled(!m_authenticating);
  }

  const lockscreen_login_box::LoginBoxStyle style = resolveLoginStyle();

  if (m_statusLabel != nullptr) {
    bool isError = false;
    const std::string text = resolveStatusText(style, isError);
    const bool show = m_locked && !m_blackout && !text.empty() && isLoginBoxEnabled();
    m_statusLabel->setVisible(show);
    if (show) {
      m_statusLabel->setText(text);
      m_statusLabel->setColor(colorSpecFromRole(isError ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    }
  }

  if (m_layoutChip != nullptr) {
    const bool show =
        m_locked && !m_blackout && style.showKeyboardLayout && m_hasMultipleLayouts && isLoginBoxEnabled();
    m_layoutChip->setVisible(show);
    if (show) {
      m_layoutChip->setText(m_layoutLabel);
      m_layoutChip->setEnabled(m_layoutSwitchable);
    }
  }
}

void LockSurface::releaseWallpaperTextureRef(const std::string& path) {
  if (m_wallpaperTexture.id == 0) {
    return;
  }
  const std::string& releasePath = !path.empty() ? path : m_textureWallpaperPath;
  if (m_textureCache != nullptr && m_textureCache->shared()) {
    if (releasePath.empty()) {
      m_wallpaperTexture = {};
      return;
    }
    m_textureCache->release(m_wallpaperTexture, releasePath);
  } else if (renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(m_wallpaperTexture);
    m_wallpaperTexture = {};
  }
  if (m_textureWallpaperPath == releasePath || path.empty()) {
    m_textureWallpaperPath.clear();
  }
}

void LockSurface::applyWallpaperTexture() {
  if (m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty()) {
    applyBlurredDesktopTexture();
    if (m_blurredDesktopTexture.id != 0) {
      return;
    }
  }

  if (!m_wallpaperDirty) {
    return;
  }

  bool loaded = true;
  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  if (parseColorWallpaperPath(m_wallpaperPath, color)) {
    if (m_wallpaperTexture.id != 0) {
      releaseWallpaperTextureRef(m_textureWallpaperPath);
    }
    if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
      renderContext()->backend().makeCurrentNoSurface();
      renderContext()->textureManager().unload(m_blurredWallpaperTexture);
      m_blurredWallpaperTexture = {};
    }
    m_wallpaper->setSources(
        WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.0f,
        0.0f, 0.0f
    );
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  } else if (m_textureCache != nullptr && !m_wallpaperPath.empty()) {
    const bool needsReload = m_wallpaperTexture.id == 0 || m_textureWallpaperPath != m_wallpaperPath;
    TextureHandle newTexture = m_wallpaperTexture;
    if (needsReload) {
      newTexture = m_textureCache->acquire(m_wallpaperPath);
      if (newTexture.id == 0 && !m_textureCache->shared() && renderContext() != nullptr) {
        renderContext()->backend().makeCurrentNoSurface();
        newTexture = renderContext()->textureManager().loadFromFile(m_wallpaperPath, 0, true);
      }
    }

    if (newTexture.id == 0) {
      loaded = false;
    } else {
      if (needsReload && m_wallpaperTexture.id != 0 && m_textureWallpaperPath != m_wallpaperPath) {
        releaseWallpaperTextureRef(m_textureWallpaperPath);
      }
      m_wallpaperTexture = newTexture;
      m_textureWallpaperPath = m_wallpaperPath;

      TextureHandle textureToDisplay = m_wallpaperTexture;
      if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
        renderContext()->backend().makeCurrentNoSurface();
        renderContext()->textureManager().unload(m_blurredWallpaperTexture);
        m_blurredWallpaperTexture = {};
      }
      if (m_blurIntensity > 0.0f && renderContext() != nullptr) {
        auto* renderer = renderContext();
        renderer->makeCurrent(renderTarget());
        static constexpr int kBlurRounds = 3;
        const float blurRadius = m_blurIntensity * 40.0f;
        m_blurredWallpaperTexture = m_wallpaperBlurCache.get(
            renderer->backend(), m_wallpaperTexture, static_cast<std::uint32_t>(m_wallpaperTexture.width),
            static_cast<std::uint32_t>(m_wallpaperTexture.height), blurRadius, kBlurRounds
        );
        if (m_blurredWallpaperTexture.id != 0) {
          textureToDisplay = m_blurredWallpaperTexture;
        }
      }
      m_wallpaper->setTextures(
          textureToDisplay.id, {}, static_cast<float>(textureToDisplay.width),
          static_cast<float>(textureToDisplay.height), 0.0f, 0.0f
      );
      m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
      m_wallpaper->setFillMode(m_wallpaperFillMode);
      m_wallpaper->setFillColor(m_wallpaperFillColor);
    }
  } else if (m_wallpaperPath.empty()) {
    if (m_wallpaperTexture.id != 0) {
      releaseWallpaperTextureRef(m_textureWallpaperPath);
    }
    m_wallpaper->setTextures({}, {}, 0.0f, 0.0f, 0.0f, 0.0f);
  } else {
    loaded = false;
  }

  m_wallpaperDirty = !loaded;
}

void LockSurface::releaseCaptureTextures() {
  if (renderContext() == nullptr) {
    m_blurredWallpaperTexture = {};
    m_captureSourceTexture = {};
    m_blurredDesktopTexture = {};
    m_blurCache.destroy();
    m_wallpaperBlurCache.destroy();
    return;
  }

  auto& tm = renderContext()->textureManager();
  renderContext()->backend().makeCurrentNoSurface();
  if (m_blurredWallpaperTexture.id != 0) {
    tm.unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }
  m_blurCache.destroy();
  m_wallpaperBlurCache.destroy();
}

void LockSurface::applyBlurredDesktopTexture() {
  if (!m_captureDirty || !m_desktopCapture.has_value() || m_desktopCapture->rgba.empty()) {
    return;
  }

  auto* renderer = renderContext();
  if (renderer == nullptr) {
    return;
  }

  const ScreencopyImage& capture = *m_desktopCapture;
  const int texW = capture.width;
  const int texH = capture.height;
  if (texW <= 0 || texH <= 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());
  auto& tm = renderer->textureManager();
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }

  m_captureSourceTexture = tm.loadFromRgba(capture.rgba.data(), texW, texH, false);
  if (m_captureSourceTexture.id == 0) {
    return;
  }

  static constexpr int kBlurRounds = 3;
  const float blurRadius = m_blurIntensity * 40.0f;
  m_blurredDesktopTexture = m_blurCache.get(
      renderer->backend(), m_captureSourceTexture, static_cast<std::uint32_t>(texW), static_cast<std::uint32_t>(texH),
      blurRadius, kBlurRounds
  );
  if (m_blurredDesktopTexture.id == 0) {
    return;
  }

  m_wallpaper->setTextures(
      m_blurredDesktopTexture.id, {}, static_cast<float>(m_blurredDesktopTexture.width),
      static_cast<float>(m_blurredDesktopTexture.height), 0.0f, 0.0f
  );
  m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  m_backdrop->setVisible(false);
  m_captureDirty = false;
  m_wallpaperDirty = false;
}

void LockSurface::onGpuResourcesInvalidated() {
  releaseCaptureTextures();

  if (m_wallpaperTexture.id != 0 && m_textureCache != nullptr) {
    if (m_textureCache->shared()) {
      m_wallpaperTexture = m_textureCache->peek(m_wallpaperPath);
    } else if (renderContext() != nullptr) {
      renderContext()->backend().textureManager().unload(m_wallpaperTexture);
      if (!m_wallpaperPath.empty()) {
        m_wallpaperTexture = renderContext()->backend().textureManager().loadFromFile(m_wallpaperPath, 0, true);
      }
    }
  }

  m_captureDirty = true;
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::render() {
  Surface::render();
  if (!m_firstFrameRendered) {
    m_firstFrameRendered = true;
    if (m_renderCallback) {
      m_renderCallback();
    }
  }
}
