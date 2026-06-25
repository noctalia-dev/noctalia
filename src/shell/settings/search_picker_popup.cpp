#include "shell/settings/search_picker_popup.h"

#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

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

  SearchPickerPopup::~SearchPickerPopup() { destroyPopup(); }

  void SearchPickerPopup::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
    initializeBase(wayland, config, renderContext);
  }

  void SearchPickerPopup::setOnSelect(SelectCallback callback) { m_onSelect = std::move(callback); }

  void SearchPickerPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

  void SearchPickerPopup::open(SearchPickerPopupRequest request) {
    if (request.parent.xdgSurface == nullptr || request.parent.wlSurface == nullptr || request.options.empty()) {
      return;
    }

    if (isOpen()) {
      close();
    }

    m_scale = std::max(0.1f, request.scale);
    m_title = std::move(request.title);
    m_options = std::move(request.options);
    m_selectedValue = std::move(request.selectedValue);
    m_placeholder = std::move(request.placeholder);
    m_emptyText = std::move(request.emptyText);
    m_root = nullptr;
    m_searchPicker = nullptr;

    const float panelWidth = 420.0f * m_scale;
    const float panelHeight = 380.0f * m_scale;
    const auto cfg = centeredPopupConfig(
        request.parent.width, request.parent.height, static_cast<std::uint32_t>(std::max(1.0f, panelWidth)),
        static_cast<std::uint32_t>(std::max(1.0f, panelHeight)), request.parent.serial
    );

    if (!openPopupAsChild(cfg, request.parent)) {
      close();
    }
  }

  void SearchPickerPopup::close() { destroyPopup(); }

  bool SearchPickerPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  bool SearchPickerPopup::onPointerEvent(const PointerEvent& event) { return DialogPopupHost::onPointerEvent(event); }

  void SearchPickerPopup::onKeyboardEvent(const KeyboardEvent& event) { DialogPopupHost::onKeyboardEvent(event); }

  wl_surface* SearchPickerPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  void SearchPickerPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const float panelPadding = Style::spaceSm * m_scale;
    const float panelGap = Style::spaceSm * m_scale;
    const std::optional<std::string> placeholder =
        m_placeholder.empty() ? std::nullopt : std::optional<std::string>(m_placeholder);
    const std::optional<std::string> emptyText =
        m_emptyText.empty() ? std::nullopt : std::optional<std::string>(m_emptyText);

    contentParent->addChild(
        ui::column(
            {
                .out = &m_root,
                .align = FlexAlign::Stretch,
                .gap = panelGap,
                .padding = panelPadding,
            },
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .gap = Style::spaceSm * m_scale,
                },
                ui::label({
                    .text = m_title,
                    .fontSize = Style::fontSizeBody * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                    .fontWeight = FontWeight::Bold,
                }),
                ui::spacer(),
                ui::button({
                    .glyph = "close",
                    .glyphSize = Style::fontSizeBody * m_scale,
                    .variant = ButtonVariant::Default,
                    .minWidth = Style::controlHeightSm * m_scale,
                    .minHeight = Style::controlHeightSm * m_scale,
                    .padding = Style::spaceXs * m_scale,
                    .radius = Style::scaledRadiusMd(m_scale),
                    .onClick = [this]() { DeferredCall::callLater([this]() { close(); }); },
                })
            ),
            ui::searchPicker({
                .out = &m_searchPicker,
                .placeholder = placeholder,
                .emptyText = emptyText,
                .selectedValue = m_selectedValue,
                .options = m_options,
                .flexGrow = 1.0f,
                .onActivated =
                    [this](const SearchPickerOption& option) {
                      if (m_onSelect) {
                        m_onSelect(option.value);
                      }
                      DeferredCall::callLater([this]() { close(); });
                    },
                .onCancel = [this]() { DeferredCall::callLater([this]() { close(); }); },
                .configure =
                    [](SearchPicker& picker) {
                      picker.clearFill();
                      picker.clearBorder();
                      picker.setRadius(0.0f);
                      picker.setPadding(0.0f);
                    },
            })
        )
    );
  }

  void SearchPickerPopup::layoutSheet(float contentWidth, float contentHeight) {
    if (m_root == nullptr || renderContext() == nullptr) {
      return;
    }
    m_root->setSize(contentWidth, contentHeight);
    m_root->layout(*renderContext());
  }

  void SearchPickerPopup::cancelToFacade() {}

  InputArea* SearchPickerPopup::initialFocusArea() {
    return m_searchPicker != nullptr ? m_searchPicker->filterInputArea() : nullptr;
  }

  void SearchPickerPopup::onSheetClose() {
    m_options.clear();
    m_root = nullptr;
    m_searchPicker = nullptr;
    if (m_onDismissed) {
      DeferredCall::callLater([callback = m_onDismissed]() { callback(); });
    }
  }

} // namespace settings
