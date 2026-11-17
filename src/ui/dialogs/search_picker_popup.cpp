#include "ui/dialogs/search_picker_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

  constexpr float kPopupWidth = 420.0F;
  constexpr float kPopupHeight = 380.0F;

  SearchPickerPopup* s_dialogPopup = nullptr;

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
        // One-shot pickers keep tray-like outside-click dismiss via xdg_popup grab.
        .grab = true,
    };
  }

} // namespace

SearchPickerPopup::~SearchPickerPopup() {
  closeNativeWindow(false);
  destroyPopup();
}

void SearchPickerPopup::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
  initializeBase(wayland, config, renderContext);
  setHazeProfile("search-picker");
}

void SearchPickerPopup::initialize(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts
) {
  initializeBase(wayland, config, renderContext, popupHosts);
  setHazeProfile("search-picker");
}

void SearchPickerPopup::setOnSelect(SelectCallback callback) { m_onSelect = std::move(callback); }

void SearchPickerPopup::setOnSelectWithOption(SelectWithOptionCallback callback) {
  m_onSelectWithOption = std::move(callback);
}

void SearchPickerPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

void SearchPickerPopup::open(SearchPickerPopupRequest request) {
  if ((request.options.empty() && !request.showAllApplicationsToggle)
      || (request.parent.has_value()
          && (request.parent->xdgSurface == nullptr || request.parent->wlSurface == nullptr))) {
    return;
  }

  if (isOpen()) {
    close();
  }

  m_scale = std::max(0.1F, request.scale);
  m_title = std::move(request.title);
  m_options = std::move(request.options);
  m_selectedValue = std::move(request.selectedValue);
  m_keepOpenOnSelect = request.keepOpenOnSelect;
  m_optionToggleLabel = std::move(request.optionToggleLabel);
  m_optionToggleChecked = request.optionToggleChecked;
  m_showCancelButton = request.showCancelButton;
  m_explicitConfirmation = request.explicitConfirmation;
  m_showAllApplicationsToggle = request.showAllApplicationsToggle;
  m_preferredWidth = request.preferredWidth;
  m_preferredHeight = request.preferredHeight;
  m_placeholder = std::move(request.placeholder);
  m_emptyText = std::move(request.emptyText);
  m_root = nullptr;
  m_searchPicker = nullptr;
  m_optionToggle = nullptr;
  m_showAllToggle = nullptr;
  m_openButton = nullptr;
  m_restoreParentOnClose = true;

  float widthLogical = m_preferredWidth > 0.0F ? m_preferredWidth : kPopupWidth;
  float heightLogical = m_preferredHeight > 0.0F ? m_preferredHeight : kPopupHeight;
  if (m_preferredWidth > 0.0F || m_preferredHeight > 0.0F) {
    std::uint32_t availableWidth = 0;
    std::uint32_t availableHeight = 0;
    if (request.parent.has_value()) {
      availableWidth = request.parent->width;
      availableHeight = request.parent->height;
    } else if (m_wayland != nullptr) {
      wl_output* output = m_wayland->outputForSurface(m_wayland->lastPointerSurface());
      if (output == nullptr)
        output = m_wayland->lastPointerOutput();
      if (const auto* geometry = m_wayland->findOutputByWl(output); geometry != nullptr) {
        availableWidth = static_cast<std::uint32_t>(std::max(0, geometry->effectiveLogicalWidth()));
        availableHeight = static_cast<std::uint32_t>(std::max(0, geometry->effectiveLogicalHeight()));
      }
    }
    constexpr float margin = 24.0F;
    if (availableWidth > 0)
      widthLogical = std::min(widthLogical, std::max(1.0F, static_cast<float>(availableWidth) / m_scale - margin));
    if (availableHeight > 0)
      heightLogical = std::min(heightLogical, std::max(1.0F, static_cast<float>(availableHeight) / m_scale - margin));
  }
  const auto width = static_cast<std::uint32_t>(std::max(1.0F, widthLogical * m_scale));
  const auto height = static_cast<std::uint32_t>(std::max(1.0F, heightLogical * m_scale));

  const bool opened = request.nativeWindow ? openNativeWindow(request, width, height)
      : request.parent.has_value()
      ? openPopupAsChild(
            centeredPopupConfig(request.parent->width, request.parent->height, width, height, request.parent->serial),
            *request.parent
        )
      : openPopup(width, height);
  if (!opened) {
    close();
  }
}

void SearchPickerPopup::close() {
  if (m_nativeWindow != nullptr) {
    closeNativeWindow(m_restoreParentOnClose);
    return;
  }
  destroyPopup();
}

wl_surface* SearchPickerPopup::wlSurface() const noexcept {
  return m_nativeWindow != nullptr ? m_nativeWindow->wlSurface() : DialogPopupHost::wlSurface();
}

bool SearchPickerPopup::ownsSurface(wl_surface* surface) const noexcept {
  return surface != nullptr && surface == wlSurface();
}

bool SearchPickerPopup::openNativeWindow(
    const SearchPickerPopupRequest& request, std::uint32_t width, std::uint32_t height
) {
  if (m_wayland == nullptr || m_renderContext == nullptr || request.nativeParentSurface == nullptr)
    return false;

  const auto shadow = m_config != nullptr ? m_config->config().shell.shadow : ShellConfig::ShadowConfig{};
  const auto desiredGeometry = popup_chrome::computeGeometry(
      static_cast<float>(width), static_cast<float>(height), shadow, Style::popupShadowsEnabled()
  );
  const auto minimumGeometry =
      popup_chrome::computeGeometry(400.0F * m_scale, 360.0F * m_scale, shadow, Style::popupShadowsEnabled());
  const auto* output = m_wayland->findOutputByWl(m_wayland->outputForSurface(request.nativeParentSurface));
  if (output == nullptr && m_wayland->lastPointerOutput() != nullptr)
    output = m_wayland->findOutputByWl(m_wayland->lastPointerOutput());
  if (output == nullptr && !m_wayland->outputs().empty())
    output = &m_wayland->outputs().front();
  const std::uint32_t outputWidth = output != nullptr
      ? static_cast<std::uint32_t>(std::max(1, output->effectiveLogicalWidth()))
      : desiredGeometry.surfaceWidth;
  const std::uint32_t outputHeight = output != nullptr
      ? static_cast<std::uint32_t>(std::max(1, output->effectiveLogicalHeight()))
      : desiredGeometry.surfaceHeight;
  const std::uint32_t minWidth = std::min(minimumGeometry.surfaceWidth, outputWidth);
  const std::uint32_t minHeight = std::min(minimumGeometry.surfaceHeight, outputHeight);
  const std::uint32_t windowWidth = std::clamp(desiredGeometry.surfaceWidth, minWidth, outputWidth);
  const std::uint32_t windowHeight = std::clamp(desiredGeometry.surfaceHeight, minHeight, outputHeight);

  m_nativeParentSurface = request.nativeParentSurface;
  m_nativeWindow = std::make_unique<ToplevelSurface>(*m_wayland);
  m_nativeWindow->setRenderContext(m_renderContext);
  m_nativeWindow->setAnimationManager(&m_animations);
  m_nativeWindow->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_nativeWindow != nullptr)
      m_nativeWindow->requestLayout();
  });
  m_nativeWindow->setPrepareFrameCallback([this](bool update, bool layout) { prepareNativeFrame(update, layout); });
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  m_nativeWindow->setClosedCallback([this, aliveGuard] {
    DeferredCall::callLater([this, aliveGuard] {
      if (!aliveGuard.expired())
        close();
    });
  });
  const ToplevelSurfaceConfig config{
      .width = windowWidth,
      .height = windowHeight,
      .minWidth = minWidth,
      .minHeight = minHeight,
      .maxWidth = outputWidth,
      .maxHeight = outputHeight,
      .parent = request.nativeParent,
      .title = request.title,
      .appId = "dev.nythera.FileManager.OpenWith"
  };
  if (!m_nativeWindow->initialize(output != nullptr ? output->output : nullptr, config)) {
    m_nativeWindow.reset();
    m_nativeParentSurface = nullptr;
    return false;
  }
  m_wayland->activateSurface(m_nativeWindow->wlSurface());
  return true;
}

void SearchPickerPopup::closeNativeWindow(bool restoreParent) {
  if (m_nativeWindow == nullptr)
    return;
  wl_surface* const parentSurface = m_nativeParentSurface;
  const bool shouldRestoreParent = restoreParent && m_restoreParentOnClose;
  m_inputDispatcher.setTextInputContext(nullptr, nullptr);
  m_inputDispatcher.setSceneRoot(nullptr);
  m_nativeWindow->setSceneRoot(nullptr);
  m_sceneRoot.reset();
  m_bgNode = nullptr;
  m_panelShadow = nullptr;
  m_contentNode = nullptr;
  m_root = nullptr;
  m_searchPicker = nullptr;
  m_optionToggle = nullptr;
  m_showAllToggle = nullptr;
  m_openButton = nullptr;
  m_resizeEdges = {};
  m_nativeWindow.reset();
  m_nativeParentSurface = nullptr;
  onSheetClose();
  if (shouldRestoreParent && parentSurface != nullptr && m_wayland != nullptr)
    m_wayland->activateSurface(parentSurface);
}

void SearchPickerPopup::prepareNativeFrame(bool update, bool layout) {
  if (m_nativeWindow == nullptr || m_renderContext == nullptr)
    return;
  const std::uint32_t width = m_nativeWindow->width();
  const std::uint32_t height = m_nativeWindow->height();
  if (width == 0 || height == 0)
    return;
  m_renderContext->makeCurrent(m_nativeWindow->renderTarget());
  if (m_sceneRoot == nullptr)
    buildNativeScene(width, height);
  if (update) {
    UiPhaseScope phase(UiPhase::Update);
    runUpdatePhase();
  }
  if (layout) {
    UiPhaseScope phase(UiPhase::Layout);
    layoutNativeScene(width, height);
  }
  if (update && m_sceneRoot != nullptr && m_sceneRoot->layoutDirty())
    m_nativeWindow->requestLayout();
}

void SearchPickerPopup::buildNativeScene(std::uint32_t width, std::uint32_t height) {
  if (m_nativeWindow == nullptr || m_wayland == nullptr)
    return;
  const auto shadow = m_config != nullptr ? m_config->config().shell.shadow : ShellConfig::ShadowConfig{};
  const auto bleed = popup_chrome::computeGeometry(1.0F, 1.0F, shadow, Style::popupShadowsEnabled()).bleed;
  const float panelWidth = std::max(1.0F, static_cast<float>(width) - static_cast<float>(bleed.left + bleed.right));
  const float panelHeight = std::max(1.0F, static_cast<float>(height) - static_cast<float>(bleed.up + bleed.down));
  m_chrome = popup_chrome::computeGeometry(panelWidth, panelHeight, shadow, Style::popupShadowsEnabled());
  m_sceneRoot = ui::node({});
  m_sceneRoot->setAnimationManager(&m_animations);
  if (Style::popupShadowsEnabled())
    m_panelShadow = popup_chrome::addShadow(*m_sceneRoot, m_chrome, shadow, Style::scaledRadiusXl());
  auto background = ui::box({.configure = [](Box& box) { box.setDialogStyle(); }});
  m_bgNode = static_cast<Box*>(m_sceneRoot->addChild(std::move(background)));
  auto content = ui::node({});
  m_contentNode = content.get();
  m_sceneRoot->addChild(std::move(content));

  constexpr std::array<std::uint32_t, 8> edges{
      XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,      XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
      XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
      XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
  };
  constexpr std::array<std::uint32_t, 8> cursors{
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
  };
  for (std::size_t i = 0; i < edges.size(); ++i) {
    auto edge = std::make_unique<InputArea>();
    edge->setParticipatesInLayout(false);
    edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    edge->setCursorShape(cursors[i]);
    edge->setOnPress([this, side = edges[i]](const InputArea::PointerData& data) {
      if (data.pressed && m_nativeWindow != nullptr && m_wayland != nullptr)
        xdg_toplevel_resize(m_nativeWindow->xdgToplevel(), m_wayland->seat(), data.serial, side);
    });
    m_resizeEdges[i] = static_cast<InputArea*>(m_sceneRoot->addChild(std::move(edge)));
  }
  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_inputDispatcher.setTextInputContext(m_nativeWindow->wlSurface(), m_wayland->textInputService());
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    if (m_wayland != nullptr)
      m_wayland->setCursorShape(serial, shape);
  });
  m_nativeWindow->setSceneRoot(m_sceneRoot.get());
  populateContent(
      m_contentNode, static_cast<std::uint32_t>(std::round(m_chrome.contentWidth)),
      static_cast<std::uint32_t>(std::round(m_chrome.contentHeight))
  );
  if (auto* focusArea = initialFocusArea(); focusArea != nullptr)
    m_inputDispatcher.setFocus(focusArea);
  layoutNativeScene(width, height);
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard] {
    if (!aliveGuard.expired() && m_nativeWindow != nullptr) {
      if (auto* focusArea = initialFocusArea(); focusArea != nullptr)
        m_inputDispatcher.setFocus(focusArea);
    }
  });
}

void SearchPickerPopup::layoutNativeScene(std::uint32_t width, std::uint32_t height) {
  if (m_nativeWindow == nullptr || m_sceneRoot == nullptr)
    return;
  const auto shadow = m_config != nullptr ? m_config->config().shell.shadow : ShellConfig::ShadowConfig{};
  const auto bleed = popup_chrome::computeGeometry(1.0F, 1.0F, shadow, Style::popupShadowsEnabled()).bleed;
  const float panelWidth = std::max(1.0F, static_cast<float>(width) - static_cast<float>(bleed.left + bleed.right));
  const float panelHeight = std::max(1.0F, static_cast<float>(height) - static_cast<float>(bleed.up + bleed.down));
  if (width != m_chrome.surfaceWidth || height != m_chrome.surfaceHeight)
    m_chrome = popup_chrome::computeGeometry(panelWidth, panelHeight, shadow, Style::popupShadowsEnabled());
  m_sceneRoot->setSize(static_cast<float>(width), static_cast<float>(height));
  const float panelX = m_chrome.contentX();
  const float panelY = m_chrome.contentY();
  if (m_panelShadow != nullptr && Style::popupShadowsEnabled()) {
    const auto offset = shadowDirectionOffset(shadow.direction);
    m_panelShadow->setPosition(panelX + static_cast<float>(offset.x), panelY + static_cast<float>(offset.y));
    m_panelShadow->setFrameSize(m_chrome.contentWidth, m_chrome.contentHeight);
  }
  if (m_bgNode != nullptr) {
    m_bgNode->setPosition(panelX, panelY);
    m_bgNode->setSize(m_chrome.contentWidth, m_chrome.contentHeight);
  }
  const float padding = computePadding(uiScale());
  const float contentWidth = m_chrome.contentWidth - padding * 2.0F;
  const float contentHeight = m_chrome.contentHeight - padding * 2.0F;
  if (m_contentNode != nullptr) {
    m_contentNode->setPosition(panelX + padding, panelY + padding);
    m_contentNode->setSize(contentWidth, contentHeight);
  }
  layoutSheet(contentWidth, contentHeight);

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const std::array<LayoutRect, 8> bounds{
      {{10, 0, w - 20, 5},
       {10, h - 5, w - 20, 5},
       {0, 10, 5, h - 20},
       {w - 5, 10, 5, h - 20},
       {0, 0, 10, 10},
       {w - 10, 0, 10, 10},
       {0, h - 10, 10, 10},
       {w - 10, h - 10, 10, 10}}
  };
  Renderer& renderer = m_nativeWindow->renderTarget().renderer();
  for (std::size_t i = 0; i < bounds.size(); ++i) {
    m_resizeEdges[i]->setPosition(bounds[i].x, bounds[i].y);
    m_resizeEdges[i]->setSize(bounds[i].width, bounds[i].height);
    m_resizeEdges[i]->layout(renderer);
  }
}

bool SearchPickerPopup::onPointerEvent(const PointerEvent& event) {
  if (m_nativeWindow == nullptr)
    return DialogPopupHost::onPointerEvent(event);
  if (event.surface != m_nativeWindow->wlSurface())
    return false;
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
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
        event.time, event.touch
    );
    break;
  case PointerEvent::Type::Axis:
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  return true;
}

void SearchPickerPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_nativeWindow == nullptr) {
    DialogPopupHost::onKeyboardEvent(event);
    return;
  }
  if (m_wayland == nullptr || m_wayland->lastKeyboardSurface() != m_nativeWindow->wlSurface())
    return;
  if (event.pressed && !event.preedit && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    close();
    return;
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

void SearchPickerPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  const float panelPadding = Style::spaceSm * m_scale;
  const float panelGap = Style::spaceSm * m_scale;
  const std::optional<std::string> placeholder =
      m_placeholder.empty() ? std::nullopt : std::optional<std::string>(m_placeholder);
  const std::optional<std::string> emptyText =
      m_emptyText.empty() ? std::nullopt : std::optional<std::string>(m_emptyText);

  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * m_scale,
  });
  header->addChild(
      ui::label({
          .text = m_title,
          .fontSize = Style::fontSizeBody * m_scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  header->addChild(ui::spacer());
  if (Style::closeButtonsEnabled()) {
    header->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * m_scale,
            .variant = ButtonVariant::Default,
            .minWidth = Style::controlHeightSm * m_scale,
            .minHeight = Style::controlHeightSm * m_scale,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this] {
              const std::weak_ptr<void> aliveGuard = m_aliveGuard;
              DeferredCall::callLater([this, aliveGuard] {
                if (aliveGuard.expired()) {
                  return;
                }
                close();
              });
            },
        })
    );
  }

  auto picker = ui::searchPicker(
      {.out = &m_searchPicker,
       .placeholder = placeholder,
       .emptyText = emptyText,
       .selectedValue = m_selectedValue,
       .options = m_options,
       .flexGrow = 1.0F,
       .onActivated =
           [this](const SearchPickerOption& option) {
             if (m_explicitConfirmation) {
               m_selectedValue = option.value;
               if (m_searchPicker != nullptr)
                 m_searchPicker->setSelectedValue(option.value);
               if (m_openButton != nullptr)
                 m_openButton->setEnabled(true);
               return;
             }
             if (m_onSelectWithOption)
               m_onSelectWithOption(option.value, m_optionToggleChecked);
             else if (m_onSelect)
               m_onSelect(option.value);
             if (m_keepOpenOnSelect) {
               m_selectedValue = option.value;
               if (m_searchPicker != nullptr)
                 m_searchPicker->setSelectedValue(option.value);
               return;
             }
             const std::weak_ptr<void> aliveGuard = m_aliveGuard;
             DeferredCall::callLater([this, aliveGuard] {
               if (!aliveGuard.expired())
                 close();
             });
           },
       .onCancel =
           [this] {
             const std::weak_ptr<void> aliveGuard = m_aliveGuard;
             DeferredCall::callLater([this, aliveGuard] {
               if (!aliveGuard.expired())
                 close();
             });
           },
       .configure =
           [this](SearchPicker& searchPicker) {
             searchPicker.clearFill();
             searchPicker.clearBorder();
             searchPicker.setRadius(0.0F);
             searchPicker.setPadding(0.0F);
             if (m_showAllApplicationsToggle)
               searchPicker.setCompatibleEmptyText("No compatible applications");
             if (m_explicitConfirmation) {
               searchPicker.setExplicitSelectionMode(true);
               searchPicker.setOnSelectionChanged([this](const std::string& value) {
                 m_selectedValue = value;
                 if (m_openButton != nullptr)
                   m_openButton->setEnabled(!value.empty());
               });
               searchPicker.setOnValidate([this](const SearchPickerOption& option) {
                 if (m_searchPicker == nullptr)
                   return;
                 m_searchPicker->setSelectedValue(option.value);
                 m_selectedValue = option.value;
                 if (m_openButton != nullptr)
                   m_openButton->setEnabled(true);
                 m_restoreParentOnClose = false;
                 if (m_onSelectWithOption)
                   m_onSelectWithOption(option.value, m_optionToggleChecked);
                 else if (m_onSelect)
                   m_onSelect(option.value);
                 const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                 DeferredCall::callLater([this, aliveGuard] {
                   if (!aliveGuard.expired())
                     close();
                 });
               });
             }
           }}
  );

  auto content = ui::column(
      {
          .out = &m_root,
          .align = FlexAlign::Stretch,
          .gap = panelGap,
          .padding = panelPadding,
      },
      std::move(header), std::move(picker)
  );
  if (!m_optionToggleLabel.empty()) {
    auto optionRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale});
    optionRow->addChild(
        ui::checkbox(
            {.out = &m_optionToggle,
             .checked = m_optionToggleChecked,
             .scale = m_scale,
             .onChange = [this](bool checked) { m_optionToggleChecked = checked; }}
        )
    );
    optionRow->addChild(
        ui::label(
            {.text = m_optionToggleLabel,
             .fontSize = Style::fontSizeCaption * m_scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    content->addChild(std::move(optionRow));
  }
  if (m_showAllApplicationsToggle) {
    auto showAllRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale});
    showAllRow->addChild(
        ui::checkbox({.out = &m_showAllToggle, .checked = false, .scale = m_scale, .onChange = [this](bool checked) {
                        if (m_searchPicker != nullptr)
                          m_searchPicker->setShowAllApplications(checked);
                      }})
    );
    showAllRow->addChild(
        ui::label(
            {.text = "Show All Applications",
             .fontSize = Style::fontSizeCaption * m_scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    content->addChild(std::move(showAllRow));
  }
  if (m_showCancelButton) {
    auto footer = ui::row({.align = FlexAlign::Center});
    footer->addChild(ui::spacer());
    if (m_explicitConfirmation) {
      footer->addChild(
          ui::button(
              {.out = &m_openButton,
               .text = "Open",
               .enabled = !m_selectedValue.empty(),
               .variant = ButtonVariant::Primary,
               .minWidth = 88.0F * m_scale,
               .minHeight = Style::controlHeight * m_scale,
               .radius = Style::scaledRadiusMd(m_scale),
               .onClick = [this] {
                 if (m_selectedValue.empty())
                   return;
                 m_restoreParentOnClose = false;
                 if (m_onSelectWithOption)
                   m_onSelectWithOption(m_selectedValue, m_optionToggleChecked);
                 else if (m_onSelect)
                   m_onSelect(m_selectedValue);
                 const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                 DeferredCall::callLater([this, aliveGuard] {
                   if (!aliveGuard.expired())
                     close();
                 });
               }}
          )
      );
    }
    footer->addChild(
        ui::button(
            {.text = "Cancel",
             .variant = ButtonVariant::Outline,
             .minWidth = 88.0F * m_scale,
             .minHeight = Style::controlHeight * m_scale,
             .radius = Style::scaledRadiusMd(m_scale),
             .onClick = [this] {
               const std::weak_ptr<void> aliveGuard = m_aliveGuard;
               DeferredCall::callLater([this, aliveGuard] {
                 if (!aliveGuard.expired())
                   close();
               });
             }}
        )
    );
    content->addChild(std::move(footer));
  }
  contentParent->addChild(std::move(content));
}

void SearchPickerPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_root == nullptr || renderContext() == nullptr) {
    return;
  }
  m_root->setSize(contentWidth, contentHeight);
  m_root->layout(m_nativeWindow != nullptr ? m_nativeWindow->renderTarget().renderer() : renderer());
}

void SearchPickerPopup::cancelToFacade() {}

InputArea* SearchPickerPopup::initialFocusArea() {
  return m_searchPicker != nullptr ? m_searchPicker->filterInputArea() : nullptr;
}

void SearchPickerPopup::onSheetClose() {
  m_options.clear();
  m_root = nullptr;
  m_searchPicker = nullptr;
  m_optionToggle = nullptr;
  m_showAllToggle = nullptr;
  m_openButton = nullptr;
  if (m_onDismissed) {
    DeferredCall::callLater([callback = m_onDismissed] { callback(); });
  }
}

void SearchPickerDialog::setPopup(SearchPickerPopup* popup) noexcept { s_dialogPopup = popup; }

bool SearchPickerDialog::open(SearchPickerPopupRequest request, SearchPickerPopup::SelectCallback onSelect) {
  if (s_dialogPopup == nullptr || request.parent.has_value()) {
    return false;
  }
  s_dialogPopup->setOnSelect(std::move(onSelect));
  s_dialogPopup->open(std::move(request));
  return s_dialogPopup->isOpen();
}
