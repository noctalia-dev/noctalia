#include "shell/settings/settings_sheet.h"

#include "core/deferred_call.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

namespace settings {

  void SettingsSheet::configure(SettingsSheetRequest request) {
    m_scale = std::max(0.1F, request.scale);
    m_minWidth = request.minWidth;
    m_maxWidth = request.maxWidth;
    m_parentFraction = request.parentFraction;
    m_fillParentHeight = request.fillParentHeight;
    m_scrollableBody = request.scrollableBody;
    m_onCloseRequested = std::move(request.onCloseRequested);
    m_preDispatchKeyboard = std::move(request.preDispatchKeyboard);
    m_title = std::move(request.sheetTitle);
    m_removeAction = std::move(request.removeAction);
    m_createHeaderAction = std::move(request.createHeaderAction);
    m_populateBody = std::move(request.populateSheetBody);
    clearNodePointers();
  }

  std::unique_ptr<Node>
  SettingsSheet::build(std::function<void()> closeAction, std::function<void()> dismissSelectDropdown) {
    clearNodePointers();
    m_closeAction = std::move(closeAction);
    m_dismissSelectDropdown = std::move(dismissSelectDropdown);

    const float padding = Style::spaceSm * m_scale;
    const float gap = Style::spaceSm * m_scale;
    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = gap,
        .padding = padding,
    });

    auto header = ui::row({
        .out = &m_header,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });
    header->addChild(
        ui::label({
            .out = &m_titleLabel,
            .text = m_title,
            .fontSize = Style::fontSizeBody * m_scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    header->addChild(ui::spacer());

    if (m_createHeaderAction) {
      if (auto action = m_createHeaderAction()) {
        header->addChild(std::move(action));
      }
    }
    if (m_removeAction) {
      header->addChild(
          ui::button({
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * m_scale,
              .variant = ButtonVariant::Destructive,
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
            .minWidth = Style::controlHeightSm * m_scale,
            .minHeight = Style::controlHeightSm * m_scale,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this]() {
              if (m_closeAction) {
                DeferredCall::callLater(m_closeAction);
              }
            },
        })
    );
    root->addChild(std::move(header));
    root->addChild(makeSettingsStatusBanner({
        .message = m_statusMessage,
        .error = m_statusIsError,
        .scale = m_scale,
        .onDismiss = [this]() { clearStatusMessage(); },
        .out = &m_statusBanner,
        .messageOut = &m_statusLabel,
    }));

    if (m_scrollableBody) {
      auto scroll = ui::scrollView({
          .out = &m_scrollView,
          .state = &m_scrollState,
          .scrollbarVisible = true,
          .viewportPaddingH = 0.0F,
          .viewportPaddingV = 0.0F,
          .flexGrow = 1.0F,
          .onScrollChanged =
              [this](float /*offset*/) {
                if (m_dismissSelectDropdown) {
                  m_dismissSelectDropdown();
                }
              },
          .configure =
              [](ScrollView& scrollView) {
                scrollView.clearFill();
                scrollView.clearBorder();
              },
      });
      m_body = m_scrollView->content();
      m_body->setDirection(FlexDirection::Vertical);
      m_body->setAlign(FlexAlign::Stretch);
      m_body->setGap(Style::spaceMd * m_scale);
      if (m_populateBody) {
        m_populateBody(*m_body);
      }
      root->addChild(std::move(scroll));
    } else {
      auto body = ui::column({
          .out = &m_body,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * m_scale,
          .flexGrow = 1.0F,
      });
      if (m_populateBody) {
        m_populateBody(*m_body);
      }
      root->addChild(std::move(body));
    }
    return root;
  }

  void SettingsSheet::clear() {
    clearNodePointers();
    m_title.clear();
    m_statusMessage.clear();
    m_statusIsError = false;
    m_removeAction = nullptr;
    m_createHeaderAction = nullptr;
    m_populateBody = nullptr;
    m_onCloseRequested = nullptr;
    m_preDispatchKeyboard = nullptr;
    m_closeAction = nullptr;
    m_dismissSelectDropdown = nullptr;
  }

  void SettingsSheet::requestClose() {
    const auto onCloseRequested = m_onCloseRequested;
    if (onCloseRequested && onCloseRequested()) {
      return;
    }
    const auto closeAction = m_closeAction;
    if (closeAction) {
      closeAction();
    }
  }

  void SettingsSheet::setTitle(std::string title) {
    m_title = std::move(title);
    if (m_titleLabel != nullptr) {
      m_titleLabel->setText(m_title);
    }
  }

  void SettingsSheet::setStatusMessage(std::string message, bool error) {
    m_statusMessage = std::move(message);
    m_statusIsError = error;
    if (m_statusBanner != nullptr && m_statusLabel != nullptr) {
      updateSettingsStatusBanner(*m_statusBanner, *m_statusLabel, m_statusMessage, error);
    }
  }

  void SettingsSheet::clearStatusMessage() { setStatusMessage({}, false); }

  bool SettingsSheet::preDispatchKeyboard(const KeyboardEvent& event) const {
    return m_preDispatchKeyboard && m_preDispatchKeyboard(event);
  }

  float SettingsSheet::naturalHeight(Renderer& renderer, float width) const {
    if (m_header == nullptr || m_body == nullptr) {
      return 1.0F;
    }
    const float padding = Style::spaceSm * m_scale;
    const float gap = Style::spaceSm * m_scale;
    const float innerWidth = std::max(1.0F, width - 2.0F * padding);
    LayoutConstraints constraints;
    constraints.setExactWidth(innerWidth);
    const float headerHeight = m_header->measure(renderer, constraints).height;
    float statusHeight = 0.0F;
    if (m_statusBanner != nullptr && m_statusBanner->visible()) {
      statusHeight = m_statusBanner->measure(renderer, constraints).height + gap;
    }
    const float contentHeight = m_body->measure(renderer, constraints).height;
    return 2.0F * padding + headerHeight + gap + statusHeight + contentHeight;
  }

  void SettingsSheet::arrange(Renderer& renderer, float width, float height) {
    if (m_root != nullptr) {
      m_root->arrange(
          renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = std::max(1.0F, width), .height = std::max(1.0F, height)}
      );
    }
  }

  void SettingsSheet::clearNodePointers() {
    m_root = nullptr;
    m_header = nullptr;
    m_body = nullptr;
    m_scrollView = nullptr;
    m_titleLabel = nullptr;
    m_statusBanner = nullptr;
    m_statusLabel = nullptr;
  }

} // namespace settings
