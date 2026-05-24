#include "shell/settings/config_export_dialog_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace settings {
  namespace {

    constexpr float kPopupWidth = 560.0f;
    constexpr float kInitialPopupHeight = 320.0f;
    constexpr float kParentMargin = 48.0f;

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

  ConfigExportDialogPopup::~ConfigExportDialogPopup() { destroyPopup(); }

  void
  ConfigExportDialogPopup::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
    initializeBase(wayland, config, renderContext);
  }

  void ConfigExportDialogPopup::open(
      xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
      std::uint32_t parentWidth, std::uint32_t parentHeight, float scale, ExportCallback callback
  ) {
    if (parentXdgSurface == nullptr || parentWlSurface == nullptr) {
      return;
    }

    if (isOpen()) {
      close();
    }

    m_scale = std::max(0.1f, scale);
    m_mode = ConfigExportMode::MergedUser;
    m_callback = std::move(callback);
    m_root = nullptr;
    m_mergedRadio = nullptr;
    m_fullRadio = nullptr;
    m_parentHeight = parentHeight;

    const float popupWidth = kPopupWidth * m_scale;
    const float popupHeight = kInitialPopupHeight * m_scale;
    const auto cfg = centeredPopupConfig(
        parentWidth, parentHeight, static_cast<std::uint32_t>(std::max(1.0f, popupWidth)),
        static_cast<std::uint32_t>(std::max(1.0f, popupHeight)), serial
    );

    if (!openPopupAsChild(cfg, parentXdgSurface, parentWlSurface, output)) {
      close();
    }
  }

  void ConfigExportDialogPopup::close() { destroyPopup(); }

  bool ConfigExportDialogPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  wl_surface* ConfigExportDialogPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  void ConfigExportDialogPopup::setMode(ConfigExportMode mode) {
    m_mode = mode;
    if (m_mergedRadio != nullptr) {
      m_mergedRadio->setChecked(mode == ConfigExportMode::MergedUser);
    }
    if (m_fullRadio != nullptr) {
      m_fullRadio->setChecked(mode == ConfigExportMode::FullEffective);
    }
    requestRedraw();
  }

  void ConfigExportDialogPopup::accept() {
    const ConfigExportMode mode = m_mode;
    ExportCallback callback = std::move(m_callback);
    closeAfterAccept();
    if (callback) {
      callback(mode);
    }
  }

  std::unique_ptr<Flex>
  ConfigExportDialogPopup::makeOption(ConfigExportMode mode, const std::string& title, const std::string& description) {
    RadioButton* radioPtr = nullptr;
    auto option = ui::row(
        {
            .align = FlexAlign::Start,
            .gap = Style::spaceSm * m_scale,
            .padding = Style::spaceSm * m_scale,
            .fillWidth = true,
            .configure =
                [this](Flex& row) {
                  row.setRadius(Style::scaledRadiusMd(m_scale));
                  row.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f));
                  row.setBorder(colorSpecFromRole(ColorRole::Outline, 0.55f), Style::borderWidth);
                },
        },
        ui::radioButton({
            .out = &radioPtr,
            .checked = m_mode == mode,
            .scale = m_scale,
            .onChange =
                [this, mode](bool checked) {
                  if (checked) {
                    setMode(mode);
                  }
                },
        }),
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceXs * m_scale,
                .flexGrow = 1.0f,
            },
            ui::label({
                .text = title,
                .fontSize = Style::fontSizeBody * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
                .fontWeight = FontWeight::Bold,
            }),
            ui::label({
                .text = description,
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxWidth = (kPopupWidth - 92.0f) * m_scale,
                .maxLines = 3,
            })
        )
    );
    if (mode == ConfigExportMode::MergedUser) {
      m_mergedRadio = radioPtr;
    } else {
      m_fullRadio = radioPtr;
    }
    return option;
  }

  void
  ConfigExportDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const float popupPadding = Style::spaceMd * m_scale;
    const float popupGap = Style::spaceMd * m_scale;

    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = popupGap,
        .padding = popupPadding,
    });

    root->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * m_scale,
            },
            ui::label({
                .text = i18n::tr("settings.export-config.title"),
                .fontSize = Style::fontSizeTitle * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .fontWeight = FontWeight::Bold,
                .flexGrow = 1.0f,
            }),
            ui::button({
                .glyph = "close",
                .glyphSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Default,
                // Dialog header icon style.
                .minWidth = Style::controlHeightSm * m_scale,
                .minHeight = Style::controlHeightSm * m_scale,
                .padding = Style::spaceXs * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this]() { DeferredCall::callLater([this]() { close(); }); },
            })
        )
    );

    root->addChild(
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceSm * m_scale,
            },
            makeOption(
                ConfigExportMode::MergedUser, i18n::tr("settings.export-config.merged-user-title"),
                i18n::tr("settings.export-config.merged-user-description")
            ),
            makeOption(
                ConfigExportMode::FullEffective, i18n::tr("settings.export-config.full-effective-title"),
                i18n::tr("settings.export-config.full-effective-description")
            )
        )
    );

    root->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .justify = FlexJustify::End,
                .gap = Style::spaceSm * m_scale,
            },
            ui::button({
                .text = i18n::tr("common.actions.cancel"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Ghost,
                // Dialog footer action style.
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this]() { DeferredCall::callLater([this]() { close(); }); },
            }),
            ui::button({
                .text = i18n::tr("settings.export-config.export"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Primary,
                // Dialog footer action style.
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this]() { DeferredCall::callLater([this]() { accept(); }); },
            })
        )
    );

    contentParent->addChild(std::move(root));
  }

  void ConfigExportDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
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

  void ConfigExportDialogPopup::cancelToFacade() {}

  InputArea* ConfigExportDialogPopup::initialFocusArea() { return nullptr; }

} // namespace settings
