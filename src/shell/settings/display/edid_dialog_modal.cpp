#include "shell/settings/display/edid_dialog_modal.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "shell/settings/display/display_service.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <utility>

namespace settings::display {

  namespace {

    [[nodiscard]] std::string formatHexDump(const std::string& hex) {
      std::string dump;
      dump.reserve(hex.size() + hex.size() / 2 + hex.size() / 32);
      for (std::size_t i = 0; i < hex.size(); i += 2) {
        if (i > 0 && i % 32 == 0) {
          dump.push_back('\n');
        }
        if (!dump.empty() && dump.back() != '\n') {
          dump.push_back(' ');
        }
        dump.push_back(hex[i]);
        if (i + 1 < hex.size()) {
          dump.push_back(hex[i + 1]);
        }
      }
      return dump;
    }

  } // namespace

  void EdidDialogModal::initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown) {
    m_sheet.initialize(host, std::move(dismissSelectDropdown));
  }

  bool EdidDialogModal::isOpen() const { return m_sheet.isOpen(); }

  void EdidDialogModal::close() {
    if (isOpen()) {
      m_sheet.close();
    }
  }

  void EdidDialogModal::markDirty() {
    if (isOpen()) {
      m_sheet.rebuildBody();
    }
  }

  void EdidDialogModal::open(EdidDialogRequest request) {
    if (isOpen()) {
      close();
    }
    m_display = request.display;
    m_clipboard = request.clipboard;
    m_outputName = std::move(request.outputName);
    m_scale = std::max(0.1F, request.scale);
    m_showRaw = false;

    m_sheet.open(
        settings::SettingsSheetRequest{
            .sheetTitle = i18n::tr("settings.display.edid-title", "output", m_outputName),
            .createHeaderAction =
                [this]() {
                  return ui::button({
                      .text = i18n::tr("settings.display.edid-view-original"),
                      .fontSize = Style::fontSizeCaption * m_scale,
                      .variant = m_showRaw ? ButtonVariant::Primary : ButtonVariant::Ghost,
                      .minHeight = Style::controlHeightSm * m_scale,
                      .paddingV = Style::spaceXs * m_scale,
                      .paddingH = Style::spaceMd * m_scale,
                      .radius = Style::scaledRadiusMd(m_scale),
                      .onClick = [this]() {
                        m_showRaw = !m_showRaw;
                        if (isOpen()) {
                          m_sheet.rebuildBody();
                        }
                      },
                  });
                },
            .populateSheetBody = [this](Flex& body) { populateBody(body); },
            .scale = m_scale,
            .onClosed =
                [this]() {
                  m_display = nullptr;
                  m_clipboard = nullptr;
                  m_outputName.clear();
                },
        }
    );
    if (!isOpen()) {
      m_display = nullptr;
      m_clipboard = nullptr;
      m_outputName.clear();
    }
  }

  void EdidDialogModal::populateBody(Flex& body) {
    body.addChild(
        ui::label({
            .text = i18n::tr("settings.display.edid-description"),
            .fontSize = Style::fontSizeCaption * m_scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    body.addChild(
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceSm * m_scale,
                .padding = Style::spaceSm * m_scale,
                .configure =
                    [this](Flex& box) {
                      box.setRadius(Style::scaledRadiusMd(m_scale));
                      box.setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
                      box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                    },
            },
            ui::label({
                .text = contentText(),
                .fontSize = Style::fontSizeCaption * m_scale,
                .fontFamily = "monospace",
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 0,
            })
        )
    );

    body.addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .justify = FlexJustify::End,
                .gap = Style::spaceSm * m_scale,
            },
            ui::button({
                .text = i18n::tr("common.refresh"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick =
                    [this]() {
                      if (m_display != nullptr) {
                        m_showRaw = false;
                        m_display->readEdid(m_outputName);
                      }
                    },
            }),
            ui::button({
                .text = i18n::tr("settings.display.copy-edid"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick =
                    [this]() {
                      if (m_clipboard != nullptr) {
                        m_clipboard->setClipboardText(copyPayload());
                      }
                    },
            }),
            ui::button({
                .text = i18n::tr("common.close"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Default,
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this]() { close(); },
            })
        )
    );
  }

  std::string EdidDialogModal::contentText() const {
    if (m_display == nullptr) {
      return {};
    }
    const auto& summary = m_display->edidSummary();
    if (m_showRaw) {
      return formatHexDump(m_display->edidHex());
    }
    if (summary.status == EdidStatus::ReadError
        || summary.status == EdidStatus::DecodeError
        || summary.status == EdidStatus::DecodedEmpty) {
      return i18n::tr("settings.display.edid-empty");
    }
    std::string text;
    const auto addLine = [&text](std::string line) {
      if (!line.empty()) {
        if (!text.empty()) {
          text.push_back('\n');
        }
        text += line;
      }
    };
    if (!summary.monitorName.empty()) {
      addLine(i18n::tr("settings.display.edid-field-monitor-name", "value", summary.monitorName));
    }
    if (!summary.manufacturerId.empty()) {
      addLine(i18n::tr("settings.display.edid-field-manufacturer-id", "value", summary.manufacturerId));
    }
    if (!summary.productCode.empty()) {
      addLine(i18n::tr("settings.display.edid-field-product-code", "value", summary.productCode));
    }
    const std::string serial = !summary.serialText.empty() ? summary.serialText : summary.serialNumber;
    if (!serial.empty()) {
      addLine(i18n::tr("settings.display.edid-field-serial-number", "value", serial));
    }
    if (!summary.version.empty()) {
      addLine(i18n::tr("settings.display.edid-field-version", "value", summary.version));
    }
    if (summary.week > 0 && summary.year > 0) {
      addLine(i18n::tr("settings.display.edid-field-manufactured", "week", summary.week, "year", summary.year));
    }
    if (!summary.inputType.empty()) {
      addLine(i18n::tr("settings.display.edid-field-input-type", "value", summary.inputType));
    }
    if (summary.sizeWidthCm > 0 && summary.sizeHeightCm > 0) {
      addLine(
          i18n::tr(
              "settings.display.edid-field-physical-size", "value",
              i18n::tr("settings.display.edid-size-cm", "width", summary.sizeWidthCm, "height", summary.sizeHeightCm)
          )
      );
    }
    if (!summary.preferredMode.empty()) {
      addLine(i18n::tr("settings.display.edid-field-preferred-mode", "value", summary.preferredMode));
    }
    return text.empty() ? i18n::tr("settings.display.edid-empty") : text;
  }

  std::string EdidDialogModal::copyPayload() const {
    if (m_display == nullptr) {
      return {};
    }
    std::string payload = i18n::tr("settings.display.edid-title", "output", m_outputName);
    payload += "\n\n";
    payload += contentText();
    return payload;
  }

} // namespace settings::display
