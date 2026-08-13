#include "shell/settings/settings_sheet_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace settings {

  namespace {

    constexpr float kInitialPopupHeight = 480.0F;
    constexpr float kParentMargin = 48.0F;

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
          .grab = false,
          .reactive = true,
      };
    }

  } // namespace

  SettingsSheetPopup::~SettingsSheetPopup() { destroyPopup(); }

  void SettingsSheetPopup::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
    initializeBase(wayland, config, renderContext);
    inputDispatcher().setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (xdgSurface() == nullptr) {
        return;
      }
      wl_output* output = m_parentOutput;
      if (output == nullptr && this->wayland() != nullptr) {
        output = this->wayland()->outputForSurface(wlSurface());
      }
      TooltipManager::instance().onHoverChange(next, xdgSurface(), output);
    });
  }

  void SettingsSheetPopup::open(SettingsSheetRequest request) {
    if (request.parent.xdgSurface == nullptr || request.parent.wlSurface == nullptr) {
      return;
    }
    if (isOpen()) {
      close();
    }

    m_parentWidth = request.parent.width;
    m_parentHeight = request.parent.height;
    const XdgPopupParent parent = request.parent;
    m_sheet.configure(std::move(request));

    const float popupWidth = m_sheet.minWidth() * m_sheet.scale();
    const float popupHeight = kInitialPopupHeight * m_sheet.scale();
    const auto cfg = centeredPopupConfig(
        parent.width, parent.height, static_cast<std::uint32_t>(std::max(1.0F, popupWidth)),
        static_cast<std::uint32_t>(std::max(1.0F, popupHeight)), parent.serial
    );
    if (!openPopupAsChild(cfg, parent)) {
      close();
      return;
    }
    m_parentOutput = parent.output;
  }

  void SettingsSheetPopup::close() { destroyPopup(); }

  void SettingsSheetPopup::requestClose() { m_sheet.requestClose(); }

  void SettingsSheetPopup::setSheetTitle(std::string title) { m_sheet.setTitle(std::move(title)); }

  void SettingsSheetPopup::setStatusMessage(std::string message, bool error) {
    m_sheet.setStatusMessage(std::move(message), error);
    requestLayout();
  }

  void SettingsSheetPopup::clearStatusMessage() { setStatusMessage({}, false); }

  void SettingsSheetPopup::rebuildBody() {
    if (!isOpen()) {
      return;
    }
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    DeferredCall::callLater([this, aliveGuard]() {
      if (aliveGuard.expired() || !isOpen() || m_contentNode == nullptr) {
        return;
      }
      inputDispatcher().stashTabFocus();
      inputDispatcher().setFocus(nullptr);
      while (!m_contentNode->children().empty()) {
        m_contentNode->removeChild(m_contentNode->children().front().get());
      }
      populateContent(m_contentNode, width(), height());
      requestLayout();
      inputDispatcher().restoreStashedTabFocus();
    });
  }

  bool SettingsSheetPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  void SettingsSheetPopup::dismissOpenSelectDropdown() {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->closeSelectDropdown();
    }
  }

  bool SettingsSheetPopup::onPointerEvent(const PointerEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      if (m_selectPopup->onPointerEvent(event)) {
        return true;
      }
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_selectPopup->closeSelectDropdown();
        return true;
      }
    }
    return DialogPopupHost::onPointerEvent(event);
  }

  void SettingsSheetPopup::onKeyboardEvent(const KeyboardEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->onKeyboardEvent(event);
      return;
    }
    if (event.pressed && !event.preedit && KeySymbol::isEscape(event.sym)) {
      m_sheet.requestClose();
      return;
    }
    DialogPopupHost::onKeyboardEvent(event);
  }

  bool SettingsSheetPopup::preDispatchKeyboard(const KeyboardEvent& event) {
    return m_sheet.preDispatchKeyboard(event);
  }

  wl_surface* SettingsSheetPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  bool SettingsSheetPopup::ownsSelectDropdownSurface(wl_surface* surface) const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
  }

  bool SettingsSheetPopup::isSelectDropdownOpen() const noexcept {
    return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen();
  }

  InputArea* SettingsSheetPopup::focusedArea() noexcept { return inputDispatcher().focusedArea(); }

  void SettingsSheetPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    contentParent->addChild(m_sheet.build(
        [this, aliveGuard]() {
          if (!aliveGuard.expired()) {
            close();
          }
        },
        [this, aliveGuard]() {
          if (!aliveGuard.expired()) {
            dismissOpenSelectDropdown();
          }
        }
    ));

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

  void SettingsSheetPopup::layoutSheet(float contentWidth, float contentHeight) {
    if (m_sheet.root() == nullptr || renderContext() == nullptr || m_surface == nullptr) {
      return;
    }

    Renderer& renderer = m_surface->renderTarget().renderer();
    const float pad = computePadding(uiScale());
    const ShellConfig::ShadowConfig shadow =
        config() != nullptr ? config()->config().shell.shadow : ShellConfig::ShadowConfig{};

    float panelWidth = m_sheet.minWidth() * m_sheet.scale();
    if (m_parentWidth > 0) {
      const auto probe = popup_chrome::computeGeometry(panelWidth, panelWidth, shadow, Style::popupShadowsEnabled());
      const float chromeWidth = static_cast<float>(probe.surfaceWidth) - panelWidth;
      const float fitWidth =
          std::max(1.0F, static_cast<float>(m_parentWidth) - (kParentMargin * m_sheet.scale()) - chromeWidth);
      const float maxWidth = std::min(fitWidth, m_sheet.maxWidth() * m_sheet.scale());
      const float preferredWidth = m_sheet.parentFraction() * static_cast<float>(m_parentWidth);
      panelWidth = std::min(std::max(preferredWidth, m_sheet.minWidth() * m_sheet.scale()), maxWidth);
    }

    float contentW = std::max(1.0F, contentWidth);
    float contentH = std::max(1.0F, contentHeight);
    float rootHeight = m_sheet.naturalHeight(renderer, contentW);
    if (m_sheet.fillParentHeight() && m_parentHeight > 0) {
      const float fillHeight = static_cast<float>(m_parentHeight) - (kParentMargin * m_sheet.scale()) - pad * 2.0F;
      rootHeight = std::max(rootHeight, fillHeight);
    }

    const float panelHeight = std::ceil(rootHeight + pad * 2.0F);
    const auto geometry = popup_chrome::computeGeometry(panelWidth, panelHeight, shadow, Style::popupShadowsEnabled());
    const float maxOuterHeight = m_parentHeight > 0
        ? std::max(1.0F, static_cast<float>(m_parentHeight) - (kParentMargin * m_sheet.scale()))
        : 1.0e6F;
    const auto nextHeight = static_cast<std::uint32_t>(
        std::max(1.0F, std::min(static_cast<float>(geometry.surfaceHeight), maxOuterHeight))
    );
    const std::uint32_t nextWidth = geometry.surfaceWidth;

    if (m_surface->height() != nextHeight || m_surface->width() != nextWidth) {
      m_surface->resize(nextWidth, nextHeight);
      syncSceneGeometryFromSurface();
      contentW = std::max(1.0F, m_chrome.contentWidth - pad * 2.0F);
      contentH = std::max(1.0F, m_chrome.contentHeight - pad * 2.0F);
      rootHeight = m_sheet.naturalHeight(renderer, contentW);
    }

    m_sheet.arrange(renderer, contentW, std::max(1.0F, std::min(rootHeight, contentH)));
  }

  void SettingsSheetPopup::cancelToFacade() {}

  InputArea* SettingsSheetPopup::initialFocusArea() { return nullptr; }

  void SettingsSheetPopup::onSheetClose() {
    if (m_selectPopup != nullptr) {
      m_selectPopup->closeSelectDropdown();
    }
    m_sheet.clear();
    m_parentOutput = nullptr;
    m_parentWidth = 0;
    m_parentHeight = 0;
  }

} // namespace settings
