#include "shell/settings/settings_editor_sheet_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"
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

  constexpr float kPopupWidth = 640.0f;    // minimum / fallback logical width
  constexpr float kPopupWidthMax = 820.0f; // absolute maximum logical width
  constexpr float kPopupWidthParentFraction = 0.75f;
  constexpr float kInitialPopupHeight = 480.0f;
  constexpr float kParentMargin = 48.0f;

  SettingsEditorSheetPopup::~SettingsEditorSheetPopup() { destroyPopup(); }

  void SettingsEditorSheetPopup::initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext
  ) {
    initializeBase(wayland, config, renderContext);
  }

  void SettingsEditorSheetPopup::open(SettingsEditorSheetPopupRequest request) {
    if (request.parent.xdgSurface == nullptr || request.parent.wlSurface == nullptr) {
      return;
    }

    if (isOpen()) {
      close();
    }

    m_scale = std::max(0.1f, request.scale);
    m_sheetTitle = std::move(request.sheetTitle);
    m_removeAction = std::move(request.removeAction);
    m_populateSheetBody = std::move(request.populateSheetBody);
    m_root = nullptr;
    m_parentWidth = request.parent.width;
    m_parentHeight = request.parent.height;

    const float popupWidth = kPopupWidth * m_scale;
    const float popupHeight = kInitialPopupHeight * m_scale;
    const auto cfg = centeredPopupConfig(
        request.parent.width, request.parent.height, static_cast<std::uint32_t>(std::max(1.0f, popupWidth)),
        static_cast<std::uint32_t>(std::max(1.0f, popupHeight)), request.parent.serial
    );

    if (!openPopupAsChild(cfg, request.parent)) {
      close();
      return;
    }
    m_parentOutput = request.parent.output;
  }

  void SettingsEditorSheetPopup::close() { destroyPopup(); }

  void SettingsEditorSheetPopup::setSheetTitle(std::string title) {
    m_sheetTitle = std::move(title);
    if (m_sheetTitleLabel != nullptr) {
      m_sheetTitleLabel->setText(m_sheetTitle);
    }
  }

  void SettingsEditorSheetPopup::rebuildBody() {
    if (!isOpen()) {
      return;
    }
    // Defer: control callbacks fire mid-dispatch; rebuilding the body destroys the nodes being
    // dispatched. Re-run populate on the next loop tick, then re-measure/resize.
    DeferredCall::callLater([this]() {
      if (!isOpen() || m_contentNode == nullptr) {
        return;
      }
      inputDispatcher().setFocus(nullptr);
      while (!m_contentNode->children().empty()) {
        m_contentNode->removeChild(m_contentNode->children().front().get());
      }
      m_root = nullptr;
      populateContent(m_contentNode, width(), height());
      requestLayout();
    });
  }

  bool SettingsEditorSheetPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  void SettingsEditorSheetPopup::dismissOpenSelectDropdown() {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->closeSelectDropdown();
    }
  }

  bool SettingsEditorSheetPopup::onPointerEvent(const PointerEvent& event) {
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

  void SettingsEditorSheetPopup::onKeyboardEvent(const KeyboardEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->onKeyboardEvent(event);
      return;
    }
    DialogPopupHost::onKeyboardEvent(event);
  }

  wl_surface* SettingsEditorSheetPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  bool SettingsEditorSheetPopup::ownsSelectDropdownSurface(wl_surface* surface) const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
  }

  bool SettingsEditorSheetPopup::isSelectDropdownOpen() const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen();
  }

  void
  SettingsEditorSheetPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const float popupPadding = Style::spaceSm * m_scale;
    const float popupGap = Style::spaceSm * m_scale;

    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = popupGap,
        .padding = popupPadding,
    });

    auto header = ui::row({
        .out = &m_header,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });

    header->addChild(
        ui::label({
            .out = &m_sheetTitleLabel,
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

    // Body scrolls when its content exceeds the sheet's clamped height.
    ScrollView* scrollPtr = nullptr;
    auto scroll = ui::scrollView({
        .out = &scrollPtr,
        .state = &m_scrollState,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0f,
        .viewportPaddingV = 0.0f,
        .flexGrow = 1.0f,
        .onScrollChanged = [this](float /*offset*/) { dismissOpenSelectDropdown(); },
        .configure =
            [](ScrollView& sv) {
              sv.clearFill();
              sv.clearBorder();
            },
    });
    m_scrollView = scrollPtr;

    Flex* body = scrollPtr->content();
    body->setDirection(FlexDirection::Vertical);
    body->setAlign(FlexAlign::Stretch);
    body->setGap(Style::spaceMd * m_scale);
    if (m_populateSheetBody) {
      m_populateSheetBody(*body);
    }

    root->addChild(std::move(scroll));
    contentParent->addChild(std::move(root));

    if (wayland() != nullptr && renderContext() != nullptr && xdgSurface() != nullptr) {
      if (m_selectPopup == nullptr) {
        m_selectPopup = std::make_unique<SelectDropdownPopup>(*wayland(), *renderContext());
      }
      if (config() != nullptr) {
        m_selectPopup->setShadowConfig(config()->config().shell.shadow);
      }
      m_selectPopup->setParent(xdgSurface(), wlSurface(), m_parentOutput);
      contentParent->setPopupContext(m_selectPopup.get());
    }
  }

  void SettingsEditorSheetPopup::layoutSheet(float contentWidth, float contentHeight) {
    if (m_root == nullptr
        || m_header == nullptr
        || m_scrollView == nullptr
        || m_scrollView->content() == nullptr
        || renderContext() == nullptr
        || m_surface == nullptr) {
      return;
    }

    Renderer& renderer = *renderContext();
    const float pad = computePadding(uiScale());
    const float popupPadding = Style::spaceSm * m_scale;
    const float popupGap = Style::spaceSm * m_scale;
    const ShellConfig::ShadowConfig shadow =
        config() != nullptr ? config()->config().shell.shadow : ShellConfig::ShadowConfig{};

    // Sheet width tracks the parent (settings) window: 75% of it, floored at kPopupWidth, clamped to
    // kPopupWidthMax, and capped so the surface (content + chrome) still fits within the parent minus
    // the edge margin.
    float panelW = kPopupWidth * m_scale;
    if (m_parentWidth > 0) {
      const auto probe = popup_chrome::computeGeometry(panelW, panelW, shadow);
      const float chromeW = static_cast<float>(probe.surfaceWidth) - panelW;
      const float fitPanelW = std::max(1.0f, static_cast<float>(m_parentWidth) - (kParentMargin * m_scale) - chromeW);
      const float maxPanelW = std::min(fitPanelW, kPopupWidthMax * m_scale);
      const float minPanelW = kPopupWidth * m_scale;
      const float preferredW = kPopupWidthParentFraction * static_cast<float>(m_parentWidth);
      panelW = std::min(std::max(preferredW, minPanelW), maxPanelW);
    }

    float cw = std::max(1.0f, contentWidth);
    float ch = std::max(1.0f, contentHeight);

    // Measure header + scroll content directly with width bounded, height unbounded. Measuring the
    // root would let its flexGrow scroll view inflate to fill the constraint, so the sheet would
    // never shrink to fit a short body.
    const auto naturalHeight = [&](float widthBudget) {
      const float innerCw = std::max(1.0f, widthBudget - 2.0f * popupPadding);
      LayoutConstraints c;
      // Exact width so wrapped setting labels measure their full line count. Max width
      // alone leaves cross-axis unconstrained during measure, which under-counts height
      // for short bodies (e.g. a two-setting plugin sheet) and clips the last row.
      c.setExactWidth(innerCw);
      const float headerH = m_header->measure(renderer, c).height;
      const float contentH = m_scrollView->content()->measure(renderer, c).height;
      return 2.0f * popupPadding + headerH + popupGap + contentH;
    };

    float rootH = naturalHeight(cw);
    const float panelH = std::ceil(rootH + pad * 2.0f);
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
      rootH = naturalHeight(cw);
    }

    const float sheetH = std::max(1.0f, std::min(rootH, ch));
    m_root->arrange(renderer, LayoutRect{.x = 0.0f, .y = 0.0f, .width = cw, .height = sheetH});
  }

  void SettingsEditorSheetPopup::cancelToFacade() {}

  InputArea* SettingsEditorSheetPopup::initialFocusArea() { return nullptr; }

  void SettingsEditorSheetPopup::onSheetClose() {
    if (m_selectPopup != nullptr) {
      m_selectPopup->closeSelectDropdown();
    }
    m_parentOutput = nullptr;
    m_sheetTitle.clear();
    m_sheetTitleLabel = nullptr;
    m_removeAction = nullptr;
    m_populateSheetBody = nullptr;
    m_root = nullptr;
    m_header = nullptr;
    m_scrollView = nullptr;
    m_parentWidth = 0;
    m_parentHeight = 0;
  }

} // namespace settings
