#include "shell/lockscreen/lock_surface.h"

#include "capture/screencopy_capture.h"
#include "core/ui_phase.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/lockscreen/lockscreen_widgets_host.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <string_view>
#include <tuple>

namespace {

  const ext_session_lock_surface_v1_listener kLockSurfaceListener = {
      .configure = &LockSurface::handleConfigure,
  };

  constexpr float kMediaArtSize = lockscreen_login_box::kRegularMediaArtSize;
  constexpr float kWeatherGlyphSize = 28.0f;
  constexpr float kForecastGlyphSize = lockscreen_login_box::kRegularForecastGlyphSize;
  constexpr float kLayoutChipMaxWidth = 96.0f;
  constexpr int kForecastDayCountPaired = 3;
  constexpr int kForecastDayCountAlone = 5;
  constexpr float kWeatherCurrentMinText = 56.0f;

  [[nodiscard]] int fitForecastDays(
      Renderer& renderer, float weatherBudget, float weatherGlyphSize, float forecastGlyphSize, float captionSize,
      int maxDays
  ) {
    if (maxDays <= 0 || weatherBudget <= 0.0f) {
      return 0;
    }
    // Vertical forecast column width is dominated by the hi/lo caption.
    const float tempsW = renderer.measureText("99°/99°", captionSize).width;
    const float dayW = renderer.measureText("Wed", captionSize).width;
    const float colW = std::max({tempsW, dayW, forecastGlyphSize}) + Style::spaceXs;
    const float currentMin = weatherGlyphSize + Style::spaceSm + kWeatherCurrentMinText;
    float remaining = weatherBudget - currentMin - Style::spaceMd;
    if (remaining < colW) {
      return 0;
    }
    const int fit = 1 + static_cast<int>((remaining - colW) / (colW + Style::spaceSm));
    return std::clamp(fit, 0, maxDays);
  }

  [[nodiscard]] float forecastBlockWidth(Renderer& renderer, int dayCount, float forecastGlyphSize, float captionSize) {
    if (dayCount <= 0) {
      return 0.0f;
    }
    const float tempsW = renderer.measureText("99°/99°", captionSize).width;
    const float dayW = renderer.measureText("Wed", captionSize).width;
    const float colW = std::max({tempsW, dayW, forecastGlyphSize}) + Style::spaceXs;
    return colW * static_cast<float>(dayCount) + Style::spaceSm * static_cast<float>(dayCount - 1);
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  [[nodiscard]] ButtonVariant buttonVariantFor(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
      return ButtonVariant::Default;
    case SessionActionButtonVariant::Primary:
      return ButtonVariant::Primary;
    case SessionActionButtonVariant::Secondary:
      return ButtonVariant::Secondary;
    case SessionActionButtonVariant::Destructive:
      return ButtonVariant::Destructive;
    case SessionActionButtonVariant::Outline:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

  // Default/Ghost use SurfaceVariant or clear fills that disappear on the
  // SurfaceVariant login panel — Outline gives a readable Surface chip.
  [[nodiscard]] ButtonVariant lockscreenSessionVariant(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Primary:
    case SessionActionButtonVariant::Secondary:
    case SessionActionButtonVariant::Destructive:
    case SessionActionButtonVariant::Outline:
      return buttonVariantFor(variant);
    }
    return ButtonVariant::Outline;
  }

  [[nodiscard]] std::string weekdayAbbrev(const std::string& isoDate) {
    if (isoDate.size() != 10) {
      return isoDate;
    }
    std::tm tm{};
    tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(isoDate.substr(8, 2));
    if (std::mktime(&tm) == -1) {
      return isoDate;
    }
    const std::string weekday = formatStrftime("%a", tm);
    return weekday.empty() ? isoDate : weekday;
  }

  [[nodiscard]] std::string todayIso(std::int32_t utcOffsetSeconds) {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    return formatStrftime("%Y-%m-%d", tm);
  }

} // namespace

LockSurface::LockSurface(WaylandConnection& connection, ConfigService* config) : Surface(connection), m_config(config) {
  {
    auto backgroundLayer = ui::node({});
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
    auto widgetLayer = ui::node({});
    widgetLayer->setZIndex(2);
    m_widgetLayer = m_root.addChild(std::move(widgetLayer));
  }

  m_root.addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_loginPanel,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .paddingV = Style::spaceLg,
              .paddingH = Style::spaceLg,
              .configure = [](Flex& flex) { flex.setZIndex(2); },
          }
      )
  );

  m_loginPanel->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_infoRow,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceMd,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .visible = false,
          }
      )
  );

  m_infoRow->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_mediaBlock,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
              .flexGrow = 1.0f,
              .visible = false,
          }
      )
  );
  m_mediaBlock->addChild(
      ui::image({
          .out = &m_mediaArt,
          .fit = ImageFit::Cover,
          .radius = kMediaArtSize * 0.5f,
          .width = kMediaArtSize,
          .height = kMediaArtSize,
          .visible = false,
      })
  );
  m_mediaBlock->addChild(
      ui::glyph({
          .out = &m_mediaFallbackGlyph,
          .glyph = "disc",
          .glyphSize = 18.0f,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );
  m_mediaBlock->addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_mediaTextColumn,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Center,
              .gap = 2.0f,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
              .flexGrow = 1.0f,
          }
      )
  );
  m_mediaTextColumn->addChild(
      ui::label({
          .out = &m_mediaTitle,
          .fontSize = Style::fontSizeBody,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Start,
      })
  );
  m_mediaTextColumn->addChild(
      ui::label({
          .out = &m_mediaArtist,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .textAlign = TextAlign::Start,
      })
  );

  m_infoRow->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_weatherBlock,
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceMd,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
              .flexGrow = 1.0f,
              .visible = false,
          }
      )
  );
  m_weatherBlock->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_weatherCurrent,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .widthPolicy = FlexSizePolicy::Content,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
          }
      )
  );
  m_weatherCurrent->addChild(
      ui::glyph({
          .out = &m_weatherGlyph,
          .glyph = "weather-cloud",
          .glyphSize = kWeatherGlyphSize,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  m_weatherCurrent->addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_weatherTextColumn,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Center,
              .gap = 2.0f,
              .widthPolicy = FlexSizePolicy::Content,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
          }
      )
  );
  m_weatherTextColumn->addChild(
      ui::label({
          .out = &m_weatherTemp,
          .fontSize = Style::fontSizeBody,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
          .textAlign = TextAlign::Start,
      })
  );
  m_weatherTextColumn->addChild(
      ui::label({
          .out = &m_weatherMeta,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .textAlign = TextAlign::Start,
      })
  );
  m_weatherBlock->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_forecastRow,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .widthPolicy = FlexSizePolicy::Content,
              .heightPolicy = FlexSizePolicy::Content,
              .clipChildren = true,
          }
      )
  );
  for (auto& column : m_forecastColumns) {
    m_forecastRow->addChild(
        ui::flex(
            FlexDirection::Vertical,
            {
                .out = &column.column,
                .align = FlexAlign::Center,
                .justify = FlexJustify::Center,
                .gap = 2.0f,
                .widthPolicy = FlexSizePolicy::Content,
                .heightPolicy = FlexSizePolicy::Content,
                .visible = false,
            }
        )
    );
    column.column->addChild(
        ui::label({
            .out = &column.day,
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    column.column->addChild(
        ui::glyph({
            .out = &column.glyph,
            .glyph = "weather-cloud",
            .glyphSize = kForecastGlyphSize,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    );
    column.column->addChild(
        ui::label({
            .out = &column.temps,
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
  }

  m_loginPanel->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_statusPanel,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .gap = Style::spaceXs,
              .paddingV = Style::spaceXs,
              .paddingH = Style::spaceSm,
              .fill = colorSpecFromRole(ColorRole::Surface, 0.55f),
              .radius = Style::radiusSm,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .visible = false,
          }
      )
  );
  m_statusPanel->addChild(
      ui::label({
          .out = &m_statusLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
          .configure = [](Label& label) {
            label.setZIndex(2);
            label.setFlexGrow(1.0f);
          },
      })
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
          .glyph = "keyboard",
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
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_sessionRow,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
              .visible = false,
          }
      )
  );

  m_inputDispatcher.setSceneRoot(&m_root);
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.setCursorShape(serial, shape);
  });

  setSceneRoot(&m_root);
  setAnimationManager(&m_animations);
  m_root.setAnimationManager(&m_animations);
  setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) { requestLayout(); });
  setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
  requestUpdate();
}

LockSurface::~LockSurface() {
  m_aliveGuard.reset();
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

void LockSurface::setLoginBoxServices(
    SessionActionRunner* sessionActions, MprisService* mpris, const WeatherService* weather, HttpClient* httpClient
) {
  m_sessionActions = sessionActions;
  m_mpris = mpris;
  m_weather = weather;
  m_httpClient = httpClient;
  rebuildSessionButtons();
  requestUpdate();
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
    const bool pressed = event.pressed;
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
    syncRegularExtras(*renderer);
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
    if (m_infoRow != nullptr) {
      m_infoRow->setVisible(false);
    }
    if (m_sessionRow != nullptr) {
      m_sessionRow->setVisible(false);
    }
    if (m_layoutChip != nullptr) {
      m_layoutChip->setVisible(false);
    }
    if (m_statusPanel != nullptr) {
      m_statusPanel->setVisible(false);
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
  const lockscreen_login_box::LoginBoxStyle loginStyle = resolveLoginStyle();
  m_loginPanel->setVisible(loginVisible);
  m_loginContentRow->setVisible(loginVisible);
  m_passwordField->setVisible(loginVisible);
  m_loginButton->setVisible(loginVisible && loginStyle.showLoginButton);

  const bool regular = loginVisible && loginStyle.layout == lockscreen_login_box::LayoutMode::Regular;
  ensureLayoutChipInPasswordRow();
  if (regular && loginStyle.showSessionButtons) {
    rebuildSessionButtons();
  }
  const bool showSession = regular && loginStyle.showSessionButtons && !m_sessionButtons.empty();
  const bool showInfoConfigured = regular && lockscreen_login_box::styleShowsInfoExtras(loginStyle);
  bool statusErrorEarly = false;
  const bool hasStatusText = !resolveStatusText(loginStyle, statusErrorEarly).empty() && isLoginBoxEnabled();
  const bool showStatus = loginVisible && hasStatusText;
  const bool reserveStatus = !regular || lockscreen_login_box::styleReservesStatus(loginStyle, showStatus);

  float panelHeight =
      lockscreen_login_box::defaultPanelHeight(loginStyle.layout, showSession, showInfoConfigured, reserveStatus);
  float panelWidth = lockscreen_login_box::defaultPanelWidth(sw, loginStyle.layout);
  float panelX = std::round((sw - panelWidth) * 0.5f);
  float panelY = std::max(Style::spaceLg, sh - panelHeight - 84.0f);
  if (m_config != nullptr) {
    if (const DesktopWidgetState* loginBox =
            lockscreen_login_box::findForOutput(m_config->config().lockscreenWidgets.widgets, m_outputKey);
        loginBox != nullptr) {
      float cx = loginBox->cx;
      float cy = loginBox->cy;
      // Fit height to visible chrome so disabling hint/media/weather does not leave empty panel space.
      panelHeight =
          lockscreen_login_box::defaultPanelHeight(loginStyle.layout, showSession, showInfoConfigured, reserveStatus);
      panelWidth = lockscreen_login_box::resolvePanelWidth(sw, loginBox->boxWidth, loginStyle.layout);
      panelX = cx - panelWidth * 0.5f;
      panelY = cy - panelHeight * 0.5f;
    }
  }

  panelX = util::clampOrdered(panelX, Style::spaceLg, sw - panelWidth - Style::spaceLg);
  panelY = util::clampOrdered(panelY, Style::spaceLg, sh - panelHeight - Style::spaceLg);

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

  m_loginPanel->setFill(loginStyle.panelFill);
  m_loginPanel->setBorder(colorForRole(ColorRole::Outline, loginStyle.panelOpacity), Style::borderWidth);
  m_loginPanel->setRadius(Style::scaledRadius(loginStyle.panelRadius));
  m_loginPanel->setSoftness(1.0f);
  m_loginPanel->setClipChildren(true);

  const float contentWidth = std::max(0.0f, panelWidth - 2.0f * Style::spaceLg);

  m_loginPanel->setJustify(regular ? FlexJustify::Start : FlexJustify::Center);

  const bool mediaReady = m_mpris != nullptr && m_mpris->activePlayer().has_value();
  const bool weatherReady = m_weather != nullptr && m_weather->enabled() && m_weather->hasData();
  const auto extras = lockscreen_login_box::resolveInfoExtrasVisibility(regular, loginStyle, mediaReady, weatherReady);
  const bool showMedia = extras.showMedia;
  const bool showWeather = extras.showWeather;
  const bool showInfoExtras = showMedia || showWeather;
  const bool weatherAlone = showWeather && !showMedia;
  const bool mediaAlone = showMedia && !showWeather;
  const float mediaBudget = lockscreen_login_box::infoExtraBudget(contentWidth, showMedia, showWeather);
  const float weatherBudget = lockscreen_login_box::infoExtraBudget(contentWidth, showWeather, showMedia);
  const int maxForecastDays = weatherAlone ? kForecastDayCountAlone : kForecastDayCountPaired;
  // Placeholder until fonts/glyphs are scaled below; recomputed after contentScale.
  int forecastDaysFit = 0;
  const bool showLayoutChip =
      loginVisible && loginStyle.showKeyboardLayout && m_layoutChip != nullptr && m_layoutChip->visible();

  const lockscreen_login_box::RegularRowHeights rows = regular
      ? lockscreen_login_box::regularRowHeights(panelHeight, showSession, reserveStatus, showInfoExtras)
      : lockscreen_login_box::RegularRowHeights{};
  const float compactStatusHeight = reserveStatus ? lockscreen_login_box::regularStatusContentHeight() : 0.0f;
  const float contentScale = regular ? rows.scale : 1.0f;
  m_regularContentScale = contentScale;
  const float captionSize = Style::fontSizeCaption * contentScale;
  const float bodySize = Style::fontSizeBody * contentScale;
  const float mediaArtSize = kMediaArtSize * contentScale;
  const float weatherGlyphSize = kWeatherGlyphSize * contentScale;
  const float forecastGlyphSize = kForecastGlyphSize * contentScale;
  const float sessionGlyphSize = 16.0f * contentScale;

  forecastDaysFit = showWeather
      ? fitForecastDays(*renderer, weatherBudget, weatherGlyphSize, forecastGlyphSize, captionSize, maxForecastDays)
      : 0;
  const bool showForecast = forecastDaysFit > 0;
  const float forecastWidth =
      showForecast ? forecastBlockWidth(*renderer, forecastDaysFit, forecastGlyphSize, captionSize) : 0.0f;

  if (m_mediaArt != nullptr) {
    m_mediaArt->setSize(mediaArtSize, mediaArtSize);
    m_mediaArt->setRadius(mediaArtSize * 0.5f);
  }
  if (m_mediaFallbackGlyph != nullptr) {
    m_mediaFallbackGlyph->setGlyphSize(18.0f * contentScale);
  }
  if (m_mediaTitle != nullptr) {
    m_mediaTitle->setFontSize(bodySize);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setFontSize(captionSize);
  }
  if (m_mediaBlock != nullptr) {
    m_mediaBlock->setVisible(showMedia);
    m_mediaBlock->setMaxWidth(mediaBudget);
    m_mediaBlock->setFlexGrow(showMedia ? 1.0f : 0.0f);
    m_mediaBlock->setJustify(mediaAlone ? FlexJustify::Center : FlexJustify::Start);
  }
  if (m_weatherBlock != nullptr) {
    m_weatherBlock->setVisible(showWeather);
    m_weatherBlock->setMaxWidth(weatherBudget);
    m_weatherBlock->setFlexGrow(showWeather ? 1.0f : 0.0f);
    m_weatherBlock->setJustify(weatherAlone ? FlexJustify::Center : FlexJustify::End);
  }
  if (m_forecastRow != nullptr) {
    m_forecastRow->setVisible(showForecast);
  }
  for (std::size_t i = 0; i < m_forecastColumns.size(); ++i) {
    auto& column = m_forecastColumns[i];
    if (column.column == nullptr) {
      continue;
    }
    if (!showForecast || i >= static_cast<std::size_t>(forecastDaysFit)) {
      column.column->setVisible(false);
      continue;
    }
    // Re-show columns that sync filled but a prior narrower layout hid.
    if (column.day != nullptr && !column.day->text().empty()) {
      column.column->setVisible(true);
    }
  }
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->setGlyphSize(weatherGlyphSize);
  }
  if (m_weatherTemp != nullptr) {
    m_weatherTemp->setFontSize(bodySize);
  }
  if (m_weatherMeta != nullptr) {
    m_weatherMeta->setFontSize(captionSize);
  }
  for (auto& column : m_forecastColumns) {
    if (column.day != nullptr) {
      column.day->setFontSize(captionSize);
    }
    if (column.glyph != nullptr) {
      column.glyph->setGlyphSize(forecastGlyphSize);
    }
    if (column.temps != nullptr) {
      column.temps->setFontSize(captionSize);
    }
  }

  const float mediaTextMax = std::max(48.0f, mediaBudget - mediaArtSize - Style::spaceSm);
  if (m_mediaTitle != nullptr) {
    m_mediaTitle->setMaxWidth(mediaTextMax);
    m_mediaTitle->setEllipsize(TextEllipsize::End);
    m_mediaTitle->setAutoScroll(false);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setMaxWidth(mediaTextMax);
    m_mediaArtist->setEllipsize(TextEllipsize::End);
  }

  // Reserve measured forecast width so current weather + forecast never overflow the half-row.
  const float weatherTextMax = showForecast
      ? std::max(
            kWeatherCurrentMinText, weatherBudget - weatherGlyphSize - Style::spaceSm - Style::spaceMd - forecastWidth
        )
      : std::max(kWeatherCurrentMinText, weatherBudget - weatherGlyphSize - Style::spaceSm);
  if (m_weatherTemp != nullptr) {
    m_weatherTemp->setMaxWidth(weatherTextMax);
    m_weatherTemp->setEllipsize(TextEllipsize::End);
  }
  if (m_weatherMeta != nullptr) {
    m_weatherMeta->setMaxWidth(weatherTextMax);
    m_weatherMeta->setEllipsize(TextEllipsize::End);
    m_weatherMeta->setVisible(showWeather && weatherBudget >= 160.0f && !m_weatherMeta->text().empty());
  }

  // Scale info / status / password / session by the same factor so extra panel
  // height is shared, not absorbed only by session buttons.
  if (m_infoRow != nullptr) {
    m_infoRow->setVisible(regular && showInfoExtras);
    m_infoRow->setMaxWidth(contentWidth);
    m_infoRow->setMinHeight(regular && showInfoExtras ? rows.info : 0.0f);
    m_infoRow->setMaxHeight(regular && showInfoExtras ? rows.info : 0.0f);
    m_infoRow->setFlexGrow(0.0f);
    m_infoRow->setJustify((mediaAlone || weatherAlone) ? FlexJustify::Center : FlexJustify::Start);
  }

  if (m_statusPanel != nullptr) {
    m_statusPanel->setMaxWidth(contentWidth);
    m_statusPanel->setRadius(Style::scaledRadius(loginStyle.inputRadius));
    const float statusHeight = regular ? (reserveStatus ? rows.status : 0.0f) : compactStatusHeight;
    m_statusPanel->setMinHeight(statusHeight);
    m_statusPanel->setMaxHeight(statusHeight);
    m_statusPanel->setFlexGrow(0.0f);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setFontSize(captionSize);
    if (m_statusLabel->visible()) {
      m_statusLabel->setMaxWidth(std::max(0.0f, contentWidth - Style::spaceSm * 2.0f));
      m_statusLabel->setEllipsize(TextEllipsize::End);
    }
  }

  const float controlHeight = regular ? rows.password : Style::controlHeight;
  m_loginContentRow->setMinHeight(controlHeight);
  m_loginContentRow->setMaxHeight(controlHeight);
  m_loginContentRow->setMaxWidth(contentWidth);
  m_loginContentRow->setFlexGrow(0.0f);

  if (m_sessionRow != nullptr) {
    m_sessionRow->setVisible(showSession);
    m_sessionRow->setMaxWidth(contentWidth);
    m_sessionRow->setMinHeight(showSession ? rows.session : 0.0f);
    m_sessionRow->setMaxHeight(showSession ? rows.session : 0.0f);
    m_sessionRow->setFlexGrow(0.0f);
    if (showSession) {
      const float gaps = Style::spaceSm * static_cast<float>(std::max<std::size_t>(1, m_sessionButtons.size()) - 1);
      const float buttonMaxWidth = m_sessionButtons.empty()
          ? contentWidth
          : std::max(48.0f, (contentWidth - gaps) / static_cast<float>(m_sessionButtons.size()));
      for (Button* button : m_sessionButtons) {
        if (button == nullptr) {
          continue;
        }
        button->setMaxWidth(buttonMaxWidth);
        button->setMinHeight(0.0f);
        button->setMaxHeight(0.0f);
        button->setFillHeight(true);
        button->setGlyphSize(sessionGlyphSize);
        button->setFontSize(captionSize);
        button->setRadius(Style::scaledRadius(loginStyle.inputRadius));
      }
    }
  }

  if (showLayoutChip) {
    m_layoutChip->setRadius(Style::scaledRadius(loginStyle.inputRadius));
    m_layoutChip->setMaxWidth(kLayoutChipMaxWidth);
    m_layoutChip->setFontSize(captionSize);
    m_layoutChip->setGlyphSize(captionSize);
    m_layoutChip->setMinHeight(controlHeight);
    m_layoutChip->setMaxHeight(controlHeight);
  }

  m_passwordField->setSurfaceOpacity(loginStyle.inputOpacity);
  m_passwordField->setFrameRadius(loginStyle.inputRadius);
  m_passwordField->setTextAlign(loginStyle.centerPasswordText ? TextAlign::Center : TextAlign::Start);
  m_passwordField->setControlHeight(controlHeight);
  m_passwordField->setFontSize(bodySize);

  const bool showLoginButton = loginVisible && loginStyle.showLoginButton;
  m_loginButton->setVisible(showLoginButton);
  if (showLoginButton) {
    m_loginButton->setRadius(Style::scaledRadius(loginStyle.inputRadius));
    m_loginButton->setSize(controlHeight, controlHeight);
    m_loginButton->setGlyphSize(sessionGlyphSize);
  }

  m_loginPanel->arrange(*renderer, LayoutRect{panelX, panelY, panelWidth, panelHeight});
}

void LockSurface::updateCopy() {
  m_passwordField->setValue(m_password);
  m_passwordField->setEnabled(!m_authenticating);
  if (m_loginButton != nullptr) {
    m_loginButton->setEnabled(!m_authenticating);
  }
  for (Button* button : m_sessionButtons) {
    if (button != nullptr) {
      button->setEnabled(!m_authenticating);
    }
  }

  const lockscreen_login_box::LoginBoxStyle style = resolveLoginStyle();

  if (m_statusPanel != nullptr && m_statusLabel != nullptr) {
    bool isError = false;
    const std::string text = resolveStatusText(style, isError);
    const bool show = m_locked && !m_blackout && !text.empty() && isLoginBoxEnabled();
    m_statusPanel->setVisible(show);
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
      m_layoutChip->setGlyph("keyboard");
      m_layoutChip->setText(m_layoutLabel.empty() ? "—" : m_layoutLabel);
      m_layoutChip->setEnabled(m_layoutSwitchable);
    }
  }
}

std::vector<SessionPanelActionConfig> LockSurface::resolveSessionActions() const {
  std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();

  std::vector<SessionPanelActionConfig> out;
  out.reserve(src.size());
  for (const auto& row : src) {
    if (!row.enabled) {
      continue;
    }
    if (!session_action::isKnown(row.action)) {
      continue;
    }
    if (row.action == "lock" || row.action == "lock_and_suspend") {
      continue;
    }
    if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
      continue;
    }
    out.push_back(row);
  }
  if (out.empty()) {
    for (const auto& row : defaultSessionPanelActions()) {
      if (row.action == "lock" || row.action == "lock_and_suspend") {
        continue;
      }
      out.push_back(row);
    }
  }
  return out;
}

void LockSurface::ensureLayoutChipInPasswordRow() {
  if (m_layoutChip == nullptr || m_loginContentRow == nullptr || m_passwordField == nullptr) {
    return;
  }
  if (m_layoutChip->parent() == m_loginContentRow) {
    // Keep the keyboard chip before the password field.
    const auto& children = m_loginContentRow->children();
    if (!children.empty() && children.front().get() == m_layoutChip) {
      return;
    }
  }

  Node* currentParent = m_layoutChip->parent();
  if (currentParent == nullptr) {
    return;
  }

  std::unique_ptr<Node> owned = currentParent->removeChild(m_layoutChip);
  if (owned == nullptr) {
    return;
  }

  std::unique_ptr<Node> password;
  std::unique_ptr<Node> loginButton;
  if (m_passwordField->parent() == m_loginContentRow) {
    password = m_loginContentRow->removeChild(m_passwordField);
  }
  if (m_loginButton != nullptr && m_loginButton->parent() == m_loginContentRow) {
    loginButton = m_loginContentRow->removeChild(m_loginButton);
  }

  m_loginContentRow->addChild(std::move(owned));
  if (password != nullptr) {
    m_loginContentRow->addChild(std::move(password));
  }
  if (loginButton != nullptr) {
    m_loginContentRow->addChild(std::move(loginButton));
  }
}

void LockSurface::rebuildSessionButtons() {
  if (m_sessionRow == nullptr) {
    return;
  }

  const auto actions = resolveSessionActions();
  std::vector<std::string> keys;
  keys.reserve(actions.size());
  for (const auto& action : actions) {
    keys.push_back(action.action + "|" + (action.label.value_or("")) + "|" + (action.glyph.value_or("")));
  }
  if (keys == m_lastSessionActionKeys && !m_sessionButtons.empty()) {
    return;
  }
  m_lastSessionActionKeys = std::move(keys);

  while (!m_sessionRow->children().empty()) {
    m_sessionRow->removeChild(m_sessionRow->children().front().get());
  }
  m_sessionButtons.clear();

  if (m_sessionActions == nullptr) {
    return;
  }

  for (const auto& cfg : actions) {
    const std::string labelText =
        cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(session_action::labelKey(cfg.action));
    auto button = ui::button({
        .out = nullptr,
        .text = labelText,
        .glyph = cfg.glyph.has_value() && !cfg.glyph->empty() ? *cfg.glyph : session_action::defaultGlyph(cfg.action),
        .fontSize = Style::fontSizeCaption,
        .glyphSize = 16.0f,
        .contentAlign = ButtonContentAlign::Center,
        .variant = lockscreenSessionVariant(cfg.variant),
        .minHeight = 0.0f,
        .maxHeight = 0.0f,
        .paddingV = Style::spaceXs,
        .paddingH = Style::spaceSm,
        .gap = Style::spaceXs,
        .radius = Style::scaledRadiusMd(),
        .flexGrow = 1.0f,
        .onClick =
            [this, cfg]() {
              if (m_sessionActions != nullptr) {
                m_sessionActions->invoke(cfg);
              }
            },
        .configure =
            [](Button& control) {
              control.setDirection(FlexDirection::Horizontal);
              control.setAlign(FlexAlign::Center);
              control.setJustify(FlexJustify::Center);
              control.setFillHeight(true);
            },
    });
    m_sessionButtons.push_back(button.get());
    m_sessionRow->addChild(std::move(button));
  }
}

void LockSurface::syncRegularExtras(Renderer& renderer) {
  const lockscreen_login_box::LoginBoxStyle style = resolveLoginStyle();
  const bool regular =
      m_locked && !m_blackout && isLoginBoxEnabled() && style.layout == lockscreen_login_box::LayoutMode::Regular;
  const bool mediaReady = m_mpris != nullptr && m_mpris->activePlayer().has_value();
  const bool weatherReady = m_weather != nullptr && m_weather->enabled() && m_weather->hasData();
  const auto extras = lockscreen_login_box::resolveInfoExtrasVisibility(regular, style, mediaReady, weatherReady);

  if (m_mediaBlock != nullptr && extras.showMedia && m_mediaTitle != nullptr && m_mediaArtist != nullptr) {
    const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
    std::string title;
    std::string artist;
    std::string artUrl;
    if (active.has_value()) {
      title = active->title;
      artist = mpris::joinArtists(active->artists);
      artUrl = mpris::effectiveArtUrl(*active);
    }
    if (title.empty()) {
      title = i18n::tr("desktop-widgets.media.nothing-playing");
    }
    const bool titleChanged = title != m_lastMediaTitle;
    const bool artistChanged = artist != m_lastMediaArtist;
    const bool artChanged = artUrl != m_lastArtUrl;
    if (titleChanged) {
      m_lastMediaTitle = title;
      m_mediaTitle->setText(title);
    }
    if (artistChanged) {
      m_lastMediaArtist = artist;
      m_mediaArtist->setText(artist);
      m_mediaArtist->setVisible(!artist.empty());
    }
    if (artChanged || (m_mediaArt != nullptr && !artUrl.empty() && !m_mediaArt->hasImage())) {
      m_lastArtUrl = artUrl;
      const int targetPx = static_cast<int>(std::round(kMediaArtSize * std::max(1.0f, m_regularContentScale)));
      bool hasArt = false;
      if (m_mediaArt != nullptr) {
        if (!artUrl.empty()) {
          const std::string artPath = mpris::resolveArtworkSource(
              m_httpClient, m_pendingArtDownloads, artUrl, [this] { requestUpdate(); }, m_aliveGuard
          );
          if (!artPath.empty()) {
            hasArt = m_mediaArt->setSourceFile(renderer, artPath, targetPx, true, true);
          }
          if (!hasArt) {
            m_mediaArt->clear(renderer);
          }
        } else {
          m_mediaArt->clear(renderer);
        }
        m_mediaArt->setVisible(hasArt);
      }
      if (m_mediaFallbackGlyph != nullptr) {
        m_mediaFallbackGlyph->setVisible(!hasArt);
      }
    }
  }

  if (m_weatherBlock != nullptr) {
    // Visibility/budgets are owned by layout; only push content when shown.
    if (extras.showWeather && m_weatherGlyph != nullptr && m_weatherTemp != nullptr && m_weatherMeta != nullptr) {
      std::string fingerprint;
      std::string glyphName = "weather-cloud";
      std::string tempText = "--";
      std::string metaText;
      std::vector<std::tuple<std::string, std::string, std::string>> forecast;

      if (weatherReady) {
        const auto& snapshot = m_weather->snapshot();
        glyphName = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
        const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
        tempText = std::format("{}{}", temp, m_weather->displayTemperatureUnit());
        const bool imperial = m_weather->useImperial();
        const double wind = imperial ? snapshot.current.windSpeedKmh * 0.621371 : snapshot.current.windSpeedKmh;
        const char* windUnit = imperial
            ? "mph"
            : (snapshot.currentUnits.windSpeed.empty() ? "km/h" : snapshot.currentUnits.windSpeed.c_str());
        metaText = std::format("{} {}", static_cast<int>(std::lround(wind)), windUnit);
        const bool showLocation = m_config == nullptr || m_config->config().shell.showLocation;
        if (showLocation && !snapshot.locationName.empty()) {
          metaText = std::format("{} · {}", metaText, snapshot.locationName);
        }

        const bool weatherAlone = extras.showWeather && !extras.showMedia;
        const int forecastDayCount = weatherAlone ? kForecastDayCountAlone : kForecastDayCountPaired;
        const bool firstForecastIsToday = !snapshot.forecastDays.empty()
            && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
        const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
        for (int i = 0; i < forecastDayCount; ++i) {
          const std::size_t dayIndex = forecastStart + static_cast<std::size_t>(i);
          if (dayIndex >= snapshot.forecastDays.size()) {
            break;
          }
          const auto& day = snapshot.forecastDays[dayIndex];
          forecast.emplace_back(
              weekdayAbbrev(day.dateIso), WeatherService::glyphForCode(day.weatherCode, true),
              std::format(
                  "{}°/{}°", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
                  static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC)))
              )
          );
        }
        fingerprint = std::format(
            "{}|{}|{}|{}|{}|{}", glyphName, tempText, metaText, forecast.size(), showLocation, weatherAlone
        );
      } else {
        fingerprint = "nodata";
      }

      if (fingerprint != m_lastWeatherFingerprint) {
        m_lastWeatherFingerprint = fingerprint;
        m_weatherGlyph->setGlyph(glyphName);
        m_weatherTemp->setText(tempText);
        m_weatherMeta->setText(metaText);
        m_weatherMeta->setVisible(!metaText.empty());
        for (std::size_t i = 0; i < m_forecastColumns.size(); ++i) {
          auto& column = m_forecastColumns[i];
          const bool visible = i < forecast.size();
          if (column.column != nullptr) {
            column.column->setVisible(visible);
          }
          if (!visible) {
            continue;
          }
          column.day->setText(std::get<0>(forecast[i]));
          column.glyph->setGlyph(std::get<1>(forecast[i]));
          column.temps->setText(std::get<2>(forecast[i]));
          column.temps->setVisible(true);
        }
      }
    } else {
      m_lastWeatherFingerprint.clear();
    }
  }
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
  return i18n::tr("lockscreen.ready");
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
        const std::uint32_t blurWidth = renderTarget().bufferWidth();
        const std::uint32_t blurHeight = renderTarget().bufferHeight();
        m_blurredWallpaperTexture = m_wallpaperBlurCache.get(
            renderer->backend(), m_wallpaperTexture, blurWidth, blurHeight, blurRadius, kBlurRounds
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
  const std::uint32_t blurWidth = renderTarget().bufferWidth();
  const std::uint32_t blurHeight = renderTarget().bufferHeight();
  m_blurredDesktopTexture =
      m_blurCache.get(renderer->backend(), m_captureSourceTexture, blurWidth, blurHeight, blurRadius, kBlurRounds);
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

  if (!m_wallpaperPath.empty() && m_textureCache != nullptr) {
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

void LockSurface::prepareForGraphicsReset() noexcept {
  m_blurCache.abandon();
  m_wallpaperBlurCache.abandon();
  m_wallpaperTexture = {};
  m_blurredWallpaperTexture = {};
  m_captureSourceTexture = {};
  m_blurredDesktopTexture = {};
  m_captureDirty = true;
  m_wallpaperDirty = true;
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
