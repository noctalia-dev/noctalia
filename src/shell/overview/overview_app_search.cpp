#include "shell/overview/overview_app_search.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "i18n/i18n.h"
#include "launcher/app_provider.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "shell/wayland/layer_surface.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon.h>

namespace {
  // Keep the blurred wallpaper visible through the field and results surface.
  constexpr float kSearchSurfaceOpacity = 0.58F;
  constexpr float kResultsSurfaceOpacity = 0.74F;

  void showResultIcon(Image& image, Glyph& fallback, bool ready) {
    image.setVisible(ready);
    image.setParticipatesInLayout(ready);
    fallback.setVisible(!ready);
    fallback.setParticipatesInLayout(!ready);
  }
} // namespace

OverviewAppSearch::OverviewAppSearch() = default;
OverviewAppSearch::~OverviewAppSearch() { destroy(); }

void OverviewAppSearch::initialize(
    WaylandConnection& wayland, RenderContext& renderContext, CompositorPlatform& platform, ConfigService& config,
    PanelManager& panels
) {
  m_wayland = &wayland;
  m_renderContext = &renderContext;
  m_platform = &platform;
  m_config = &config;
  m_panels = &panels;
  m_apps = std::make_unique<AppProvider>(&config, &platform);
}

void OverviewAppSearch::sync() {
  if (m_platform == nullptr
      || !m_config->config().shell.umbrielSpread.enabled
      || !m_config->config().shell.umbrielSpread.showSearch
      || !m_platform->isOverviewOpen()
      || !m_platform->isSpreadOverview()
      || m_panels->isOpen()
      || m_panels->isPanelTransitionActive()) {
    destroy();
    return;
  }
  if (m_surface != nullptr) {
    return;
  }
  wl_output* target = m_platform->focusedInteractiveOutput();
  if (target == nullptr) {
    target = m_platform->preferredInteractiveOutput();
  }
  for (const auto& output : m_wayland->outputs()) {
    if (output.output == target && output.done && output.hasUsableGeometry()) {
      create(target, output.effectiveLogicalWidth(), output.effectiveLogicalHeight());
      break;
    }
  }
}

void OverviewAppSearch::rebuild() {
  destroy();
  sync();
}

void OverviewAppSearch::destroy() {
  m_session.reset();
  m_dispatcher.setFocus(nullptr);
  m_dispatcher.setTextInputContext(nullptr, nullptr);
  m_dispatcher.setSceneRoot(nullptr);
  if (m_surface != nullptr) {
    m_surface->setSceneRoot(nullptr);
  }
  m_scene.reset();
  m_surface.reset();
  m_input = nullptr;
  m_resultsBox = nullptr;
  m_empty = nullptr;
  m_buttons.fill(nullptr);
  m_images.fill(nullptr);
  m_fallbackGlyphs.fill(nullptr);
  m_results.clear();
}

void OverviewAppSearch::create(wl_output* output, int outputWidth, int outputHeight) {
  // Stay inside the existing top margin; do not resize windows or their gaps.
  m_scale = std::min(
      std::max(0.1F, m_config->config().accessibility.uiScale),
      std::clamp(static_cast<float>(outputHeight) / 720.0F, 0.75F, 1.5F)
  );
  const auto width =
      static_cast<std::uint32_t>(std::max(1.0F, std::min(380.0F * m_scale, static_cast<float>(outputWidth) * 0.8F)));
  m_requestedHeight = static_cast<std::uint32_t>(std::ceil(40.0F * m_scale));
  // Typing can own focus when the compositor accepts forwarded navigation keys.
  m_typeToSearch = m_platform->supportsOverviewKeyForwarding();
  m_surface = std::make_unique<LayerSurface>(
      *m_wayland,
      LayerSurfaceConfig{
          .nameSpace = "noctalia-overview-search",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top,
          .width = width,
          .height = m_requestedHeight,
          .exclusiveZone = 0,
          .marginTop = static_cast<std::int32_t>(std::round(16.0F * m_scale)),
          .keyboard = m_typeToSearch ? LayerShellKeyboard::Exclusive : LayerShellKeyboard::None,
          .defaultWidth = width,
          .defaultHeight = m_requestedHeight,
          .prewarmBlur = true,
      }
  );
  m_surface->setRenderContext(m_renderContext);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool, bool) { prepareFrame(); });
  if (!m_surface->initialize(output)) {
    destroy();
    return;
  }

  m_session = std::make_shared<int>(0);
  m_scene = ui::node({});
  m_scene->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh, kSearchSurfaceOpacity),
          .border = colorSpecFromRole(ColorRole::OutlineVariant),
          .borderWidth = 1.0F,
          .radius = 20.0F * m_scale,
          .width = static_cast<float>(width),
          .height = 40.0F * m_scale,
      })
  );
  auto glyph = ui::glyph({.glyph = "search", .glyphSize = 20.0F * m_scale});
  glyph->setPosition(14.0F * m_scale, 10.0F * m_scale);
  glyph->setSize(20.0F * m_scale, 20.0F * m_scale);
  m_scene->addChild(std::move(glyph));
  auto input = ui::input({
      .out = &m_input,
      .placeholder = i18n::tr("launcher.search-placeholder"),
      .fontSize = Style::fontSizeBody * m_scale,
      .controlHeight = 40.0F * m_scale,
      .horizontalPadding = 4.0F * m_scale,
      .clearButtonEnabled = true,
      .frameVisible = false,
      .onChange = [this](const std::string&) { refreshResults(); },
      .onSubmit = [this](const std::string&) { activate(m_selected); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKey(sym, modifiers); },
  });
  input->setPosition(42.0F * m_scale, 0);
  input->setSize(static_cast<float>(width) - 50.0F * m_scale, 40.0F * m_scale);
  m_scene->addChild(std::move(input));
  auto resultsBox = ui::box({
      .fill = colorSpecFromRole(ColorRole::Surface, kResultsSurfaceOpacity),
      .border = colorSpecFromRole(ColorRole::OutlineVariant),
      .borderWidth = 1.0F,
      .radius = 16.0F * m_scale,
      .visible = false,
  });
  m_resultsBox = resultsBox.get();
  resultsBox->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("launcher.empty.no-results"),
          .fontSize = Style::fontSizeBody * m_scale,
      })
  );
  auto resultPalette = Button::defaultPalette(ButtonVariant::Ghost);
  resultPalette.selected = Button::ButtonStateColors{
      colorSpecFromRole(ColorRole::PrimaryContainer), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimaryContainer)
  };
  for (std::size_t i = 0; i < m_buttons.size(); ++i) {
    resultsBox->addChild(
        ui::button({
            .out = &m_buttons[i],
            .fontSize = Style::fontSizeBody * m_scale,
            .controlHeight = 40.0F * m_scale,
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .customPalette = resultPalette,
            .gap = 10.0F * m_scale,
            .radius = 10.0F * m_scale,
            .onClick = [this, i] { activate(i); },
        })
    );
    m_buttons[i]->insertChildAt(
        0,
        ui::image({
            .out = &m_images[i],
            .width = 20.0F * m_scale,
            .height = 20.0F * m_scale,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    m_buttons[i]->insertChildAt(
        1,
        ui::glyph({
            .out = &m_fallbackGlyphs[i],
            .glyph = "app-window",
            .glyphSize = 20.0F * m_scale,
            .width = 20.0F * m_scale,
            .height = 20.0F * m_scale,
        })
    );
    m_images[i]->setAsyncReadyCallback([this, i] {
      if (m_images[i]->hasImage()) {
        showResultIcon(*m_images[i], *m_fallbackGlyphs[i], true);
      }
    });
  }
  m_scene->addChild(std::move(resultsBox));
  m_dispatcher.setSceneRoot(m_scene.get());
  m_dispatcher.setTextInputContext(m_surface->wlSurface(), m_wayland->textInputService(), true);
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  if (m_typeToSearch) {
    m_dispatcher.setFocus(m_input->inputArea());
  }
  m_surface->setSceneRoot(m_scene.get());
  m_surface->requestUpdate();
}

void OverviewAppSearch::refreshResults() {
  m_results = m_input->value().empty() ? std::vector<LauncherResult>{} : m_apps->query(m_input->value());
  m_results.resize(std::min(m_results.size(), m_buttons.size()));
  m_selected = 0;
  m_surface->requestUpdate();
}

void OverviewAppSearch::prepareFrame() {
  if (m_scene == nullptr) {
    return;
  }
  if (!m_renderContext->makeCurrent(m_surface->renderTarget())) {
    return;
  }
  auto& renderer = m_surface->renderTarget().renderer();
  const auto width = static_cast<float>(m_surface->width());
  const bool searching = !m_input->value().empty();
  const float resultsHeight =
      (16.0F + 44.0F * static_cast<float>(std::max(std::size_t{1}, m_results.size()))) * m_scale;
  const auto height =
      static_cast<std::uint32_t>(std::ceil(searching ? 48.0F * m_scale + resultsHeight : 40.0F * m_scale));
  if (height != m_requestedHeight) {
    m_requestedHeight = height;
    m_surface->requestSize(m_surface->width(), height);
  }
  m_scene->setSize(width, static_cast<float>(height));
  m_resultsBox->setVisible(searching);
  m_resultsBox->setPosition(0, 48.0F * m_scale);
  m_resultsBox->setSize(width, resultsHeight);
  m_empty->setVisible(m_results.empty());
  m_empty->setPosition(16.0F * m_scale, 20.0F * m_scale);
  const int iconSize = static_cast<int>(std::ceil(20.0F * m_scale));
  const int physicalIconSize = static_cast<int>(std::ceil(static_cast<float>(iconSize) * renderer.renderScale()));
  for (std::size_t i = 0; i < m_buttons.size(); ++i) {
    auto* button = m_buttons[i];
    button->setVisible(i < m_results.size());
    if (i < m_results.size()) {
      auto& result = m_results[i];
      const std::string path =
          result.iconPath.empty() ? m_iconResolver.resolve(result.iconName, physicalIconSize) : result.iconPath;
      bool iconReady = false;
      if (!path.empty()) {
        iconReady = m_images[i]->setSourceFile(renderer, path, {.targetSize = iconSize, .mipmap = true});
      } else {
        m_images[i]->clear(renderer);
      }
      showResultIcon(*m_images[i], *m_fallbackGlyphs[i], iconReady);
      button->setText(m_results[i].title);
      button->setSelected(i == m_selected);
      button->setPosition(8.0F * m_scale, (8.0F + 44.0F * static_cast<float>(i)) * m_scale);
      button->setSize(width - 16.0F * m_scale, 40.0F * m_scale);
    }
  }
  m_scene->layout(renderer);
  auto regions = Surface::tessellateRoundedRect(
      0, 0, static_cast<int>(width), static_cast<int>(std::ceil(40.0F * m_scale)), 20.0F * m_scale
  );
  if (searching) {
    const auto resultsRegion = Surface::tessellateRoundedRect(
        0, static_cast<int>(std::round(48.0F * m_scale)), static_cast<int>(width),
        static_cast<int>(std::ceil(resultsHeight)), 16.0F * m_scale
    );
    regions.insert(regions.end(), resultsRegion.begin(), resultsRegion.end());
  }
  m_surface->setBlurRegion(regions);
  m_surface->setInputRegion(regions);
}

void OverviewAppSearch::activate(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  DeferredCall::callLater([this, session = std::weak_ptr<void>(m_session), result = m_results[index]] {
    if (session.expired()
        || !m_apps->activateFromSurface(
            result, m_surface->wlSurface(), m_wayland->outputForSurface(m_surface->wlSurface())
        )) {
      return;
    }
    if (session.expired()) {
      return;
    }
    m_input->setValue("");
    refreshResults();
    m_dispatcher.setFocus(m_input->inputArea());
    if (m_config->config().shell.umbrielOverviewCloseOnLaunchEnabled) {
      m_platform->closeOverview();
    }
  });
}

bool OverviewAppSearch::handleKey(std::uint32_t sym, std::uint32_t modifiers) {
  const bool navigation = KeySymbol::isLeft(sym)
      || KeySymbol::isRight(sym)
      || KeySymbol::isUp(sym)
      || KeySymbol::isDown(sym)
      || KeySymbol::isTab(sym)
      || KeySymbol::isEnter(sym)
      || KeySymbol::isEscape(sym)
      || KeySymbol::isDelete(sym);
  if (m_typeToSearch
      && m_input->value().empty()
      && !m_input->composing()
      && navigation
      && (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0) {
    if (KeySymbol::isTab(sym) && (modifiers & KeyMod::Shift) != 0) {
      sym = XKB_KEY_ISO_Left_Tab;
    }
    std::array<char, 64> name{};
    if (xkb_keysym_get_name(sym, name.data(), name.size()) > 0) {
      // Defer: Enter/Escape may close this surface through the IPC event stream.
      DeferredCall::callLater([this, session = std::weak_ptr<void>(m_session), key = std::string(name.data())] {
        if (!session.expired()) {
          m_platform->forwardOverviewKey(key);
        }
      });
    }
    return true;
  }
  if (KeySymbol::isEscape(sym)) {
    if (!m_input->value().empty()) {
      m_input->setValue("");
      refreshResults();
    } else {
      DeferredCall::callLater([this, session = std::weak_ptr<void>(m_session)] {
        if (!session.expired()) {
          m_platform->closeOverview();
        }
      });
    }
    return true;
  }
  if (KeySymbol::isUp(sym) || KeySymbol::isDown(sym) || KeySymbol::isTab(sym)) {
    if (!m_results.empty()) {
      const bool reverse = KeySymbol::isUp(sym) || sym == XKB_KEY_ISO_Left_Tab || (modifiers & KeyMod::Shift) != 0;
      m_selected = (m_selected + (reverse ? m_results.size() - 1 : 1)) % m_results.size();
      m_surface->requestUpdate();
    }
    return true;
  }
  return false;
}

bool OverviewAppSearch::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_surface == nullptr || m_wayland->lastKeyboardSurface() != m_surface->wlSurface()) {
    return false;
  }
  m_dispatcher.setFocus(m_input->inputArea());
  m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  return true;
}

bool OverviewAppSearch::onPointerEvent(const PointerEvent& event) {
  if (m_surface == nullptr || event.surface != m_surface->wlSurface()) {
    return false;
  }
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    if (event.pressed) {
      m_dispatcher.setFocus(m_input->inputArea());
      m_surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
    }
    m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  return true;
}
