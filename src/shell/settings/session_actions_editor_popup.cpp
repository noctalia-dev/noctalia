#include "shell/settings/session_actions_editor_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>

namespace settings {

  namespace {

    PopupSurfaceConfig centeredPopupConfig(
        std::uint32_t parentWidth, std::uint32_t parentHeight, std::uint32_t width, std::uint32_t height,
        std::uint32_t serial
    ) {
      return PopupSurfaceConfig{
          .anchorX = static_cast<std::int32_t>(parentWidth / 2),
          .anchorY = static_cast<std::int32_t>(parentHeight / 2),
          .anchorWidth = 1,
          .anchorHeight = 1,
          .width = std::max<std::uint32_t>(1, width),
          .height = std::max<std::uint32_t>(1, height),
          .anchor = XDG_POSITIONER_ANCHOR_NONE,
          .gravity = XDG_POSITIONER_GRAVITY_NONE,
          .constraintAdjustment =
              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
          .offsetX = 0,
          .offsetY = 0,
          .serial = serial,
          .grab = true,
      };
    }

  } // namespace

  constexpr float kPopupWidth = 640.0f;
  constexpr float kInitialPopupHeight = 360.0f;
  constexpr float kParentMargin = 48.0f;

  SessionActionsEditorPopup::~SessionActionsEditorPopup() { destroyPopup(); }

  void SessionActionsEditorPopup::initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext
  ) {
    initializeBase(wayland, config, renderContext);
  }

  void SessionActionsEditorPopup::open(
      xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
      std::uint32_t parentWidth, std::uint32_t parentHeight, float scale, std::string sheetTitle,
      std::function<void()> removeAction, std::function<void(Flex& sheetBody)> populateSheetBody
  ) {
    if (parentXdgSurface == nullptr || parentWlSurface == nullptr) {
      return;
    }

    if (isOpen()) {
      close();
    }

    m_scale = std::max(0.1f, scale);
    m_sheetTitle = std::move(sheetTitle);
    m_removeAction = std::move(removeAction);
    m_populateSheetBody = std::move(populateSheetBody);
    m_root = nullptr;
    m_parentWidth = parentWidth;
    m_parentHeight = parentHeight;

    const float popupWidth = kPopupWidth * m_scale;
    const float popupHeight = kInitialPopupHeight * m_scale;
    const auto cfg = centeredPopupConfig(
        parentWidth, parentHeight, static_cast<std::uint32_t>(std::max(1.0f, popupWidth)),
        static_cast<std::uint32_t>(std::max(1.0f, popupHeight)), serial
    );

    if (!openPopupAsChild(cfg, parentXdgSurface, parentWlSurface, output)) {
      close();
      return;
    }
    m_parentOutput = output;
  }

  void SessionActionsEditorPopup::close() { destroyPopup(); }

  bool SessionActionsEditorPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  bool SessionActionsEditorPopup::onPointerEvent(const PointerEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      if (m_selectPopup->onPointerEvent(event)) {
        return true;
      }
      if (event.type == PointerEvent::Type::Button && event.state == 1) {
        m_selectPopup->closeSelectDropdown();
        return true;
      }
    }
    return DialogPopupHost::onPointerEvent(event);
  }

  void SessionActionsEditorPopup::onKeyboardEvent(const KeyboardEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->onKeyboardEvent(event);
      return;
    }
    DialogPopupHost::onKeyboardEvent(event);
  }

  wl_surface* SessionActionsEditorPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  bool SessionActionsEditorPopup::ownsSelectDropdownSurface(wl_surface* surface) const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
  }

  bool SessionActionsEditorPopup::isSelectDropdownOpen() const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen();
  }

  void
  SessionActionsEditorPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const float popupPadding = Style::spaceSm * m_scale;
    const float popupGap = Style::spaceSm * m_scale;

    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = popupGap,
        .padding = popupPadding,
    });

    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });

    header->addChild(
        ui::label({
            .text = m_sheetTitle,
            .fontSize = Style::fontSizeBody * m_scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
        })
    );
    header->addChild(ui::spacer());

    if (m_removeAction) {
      header->addChild(
          ui::button({
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * m_scale,
              .variant = ButtonVariant::Destructive,
              // Sheet header icon style.
              .minWidth = Style::controlHeightSm * m_scale,
              .minHeight = Style::controlHeightSm * m_scale,
              .padding = Style::spaceXs * m_scale,
              .radius = Style::scaledRadiusMd(m_scale),
              .onClick = [removeAction = m_removeAction]() {
                if (removeAction) {
                  DeferredCall::callLater(removeAction);
                }
              },
          })
      );
    }

    header->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * m_scale,
            .variant = ButtonVariant::Default,
            // Sheet header icon style.
            .minWidth = Style::controlHeightSm * m_scale,
            .minHeight = Style::controlHeightSm * m_scale,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this]() { DeferredCall::callLater([this]() { close(); }); },
        })
    );
    root->addChild(std::move(header));

    auto body = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * m_scale,
    });

    if (m_populateSheetBody) {
      m_populateSheetBody(*body);
    }

    root->addChild(std::move(body));
    contentParent->addChild(std::move(root));

    if (wayland() != nullptr && renderContext() != nullptr && xdgSurface() != nullptr) {
      if (m_selectPopup == nullptr) {
        m_selectPopup = std::make_unique<SelectDropdownPopup>(*wayland(), *renderContext());
      }
      if (config() != nullptr) {
        m_selectPopup->setShadowConfig(config()->config().shell.shadow);
      }
      m_selectPopup->setParent(xdgSurface(), m_parentOutput);
      contentParent->setPopupContext(m_selectPopup.get());
    }
  }

  void SessionActionsEditorPopup::layoutSheet(float contentWidth, float contentHeight) {
    if (m_root == nullptr || renderContext() == nullptr || m_surface == nullptr) {
      return;
    }

    Renderer& renderer = *renderContext();
    const float pad = computePadding(uiScale());
    const float panelW = kPopupWidth * m_scale;

    float cw = std::max(1.0f, contentWidth);
    float ch = std::max(1.0f, contentHeight);

    LayoutSize pref = m_root->measure(renderer, LayoutConstraints::available(cw, 1.0e6f));
    const float panelH = std::ceil(pref.height + pad * 2.0f);
    const ShellConfig::ShadowConfig shadow =
        config() != nullptr ? config()->config().shell.shadow : ShellConfig::ShadowConfig{};
    const auto geo = popup_chrome::computeGeometry(panelW, panelH, shadow);
    const float maxOuterHeight =
        m_parentHeight > 0 ? std::max(1.0f, static_cast<float>(m_parentHeight) - (kParentMargin * m_scale)) : 1.0e6f;
    const std::uint32_t nextHeight =
        static_cast<std::uint32_t>(std::max(1.0f, std::min(static_cast<float>(geo.surfaceHeight), maxOuterHeight)));
    const std::uint32_t nextWidth = geo.surfaceWidth;

    if (m_surface->height() != nextHeight || m_surface->width() != nextWidth) {
      m_surface->resize(nextWidth, nextHeight);
      syncSceneGeometryFromSurface();
      cw = std::max(1.0f, m_chrome.contentWidth - pad * 2.0f);
      ch = std::max(1.0f, m_chrome.contentHeight - pad * 2.0f);
      pref = m_root->measure(renderer, LayoutConstraints::available(cw, 1.0e6f));
    }

    const float sheetH = std::max(1.0f, std::min(pref.height, ch));
    m_root->arrange(renderer, LayoutRect{.x = 0.0f, .y = 0.0f, .width = cw, .height = sheetH});
  }

  void SessionActionsEditorPopup::cancelToFacade() {}

  InputArea* SessionActionsEditorPopup::initialFocusArea() { return nullptr; }

  void SessionActionsEditorPopup::onSheetClose() {
    if (m_selectPopup != nullptr) {
      m_selectPopup->closeSelectDropdown();
    }
    m_parentOutput = nullptr;
    m_sheetTitle.clear();
    m_removeAction = nullptr;
    m_populateSheetBody = nullptr;
    m_root = nullptr;
    m_parentWidth = 0;
    m_parentHeight = 0;
  }

} // namespace settings
