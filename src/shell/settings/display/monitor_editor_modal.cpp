#include "shell/settings/display/monitor_editor_modal.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/display/display_layout.h"
#include "shell/settings/display/display_service.h"
#include "shell/settings/display/monitor_layout_canvas.h"
#include "shell/settings/display/revert_dialog_modal.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace settings::display {

  namespace {

    using compositors::display::kTransforms;

    [[nodiscard]] std::string transformLabel(std::string_view transform) {
      if (transform == "90")
        return "90°";
      if (transform == "180")
        return "180°";
      if (transform == "270")
        return "270°";
      if (transform == "Flipped")
        return i18n::tr("settings.display.flipped");
      if (transform == "Flipped90")
        return i18n::tr("settings.display.flipped-angle", "angle", "90°");
      if (transform == "Flipped180")
        return i18n::tr("settings.display.flipped-angle", "angle", "180°");
      if (transform == "Flipped270")
        return i18n::tr("settings.display.flipped-angle", "angle", "270°");
      return i18n::tr("common.normal");
    }

    [[nodiscard]] std::vector<std::string>
    uniqueResolutions(const std::vector<compositors::display::DisplayMode>& modes) {
      std::vector<std::string> resolutions;
      for (const auto& mode : modes) {
        const auto key = std::to_string(mode.width) + "x" + std::to_string(mode.height);
        if (std::ranges::find(resolutions, key) == resolutions.end()) {
          resolutions.push_back(key);
        }
      }
      return resolutions;
    }

    [[nodiscard]] std::vector<int> sortedRatesForResolution(
        const std::vector<compositors::display::DisplayMode>& modes, const std::string& resolution
    ) {
      std::vector<int> rates;
      for (const auto& mode : modes) {
        const auto key = std::to_string(mode.width) + "x" + std::to_string(mode.height);
        if (key == resolution && std::ranges::find(rates, mode.refreshMhz) == rates.end()) {
          rates.push_back(mode.refreshMhz);
        }
      }
      std::ranges::sort(rates, std::greater<int>());
      return rates;
    }

    [[nodiscard]] bool modeIsPreferred(
        const std::vector<compositors::display::DisplayMode>& modes, const std::string& resolution, int refreshMhz
    ) {
      for (const auto& mode : modes) {
        const auto key = std::to_string(mode.width) + "x" + std::to_string(mode.height);
        if (key == resolution && mode.refreshMhz == refreshMhz && mode.preferred) {
          return true;
        }
      }
      return false;
    }

    [[nodiscard]] int
    firstRateForResolution(const std::vector<compositors::display::DisplayMode>& modes, const std::string& resolution) {
      for (const auto& mode : modes) {
        const auto key = std::to_string(mode.width) + "x" + std::to_string(mode.height);
        if (key == resolution) {
          return mode.refreshMhz;
        }
      }
      return 60000;
    }

    [[nodiscard]] std::unique_ptr<Flex>
    controlColumn(std::string label, float scale, std::unique_ptr<Node> control, float flexGrow = 0.0F) {
      auto column = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .flexGrow = flexGrow,
      });
      column->addChild(
          ui::label({
              .text = std::move(label),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      column->addChild(std::move(control));
      return column;
    }

  } // namespace

  void MonitorEditorModal::initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown) {
    m_dialogHost = &host;
    m_dismissSelectDropdown = std::move(dismissSelectDropdown);
    m_sheet.initialize(host, [this]() { m_dismissSelectDropdown(); });
    m_revertDialog = std::make_unique<RevertDialogModal>();
  }

  bool MonitorEditorModal::isOpen() const { return m_sheet.isOpen(); }

  void MonitorEditorModal::close() {
    if (isOpen()) {
      m_sheet.close();
    }
  }

  void MonitorEditorModal::markDirty() {
    if (isOpen()) {
      m_sheet.rebuildBody();
    }
    syncRevertDialog();
  }

  void MonitorEditorModal::syncRevertDialog() {
    if (m_display == nullptr || m_revertDialog == nullptr) {
      return;
    }
    if (m_display->awaitingConfirmation() && !m_revertDialog->isOpen()) {
      m_revertDialog->open(*m_dialogHost, *m_display, m_scale);
    } else if (!m_display->awaitingConfirmation() && m_revertDialog->isOpen()) {
      m_revertDialog->close();
    }
  }

  void MonitorEditorModal::open(MonitorEditorRequest request) {
    if (m_dialogHost == nullptr) {
      return;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }

    m_scale = std::max(0.1F, request.scale);
    m_display = request.display;
    m_onOpenEdid = std::move(request.onOpenEdid);
    m_onClosed = std::move(request.onClosed);
    m_selectedOutput.clear();

    m_sheet.open(
        settings::SettingsSheetRequest{
            .sheetTitle = i18n::tr("settings.display.title"),
            .populateSheetBody = [this](Flex& body) { populateBody(body); },
            .scale = m_scale,
            .maxWidth = 980.0F,
            .onClosed =
                [this]() {
                  m_display = nullptr;
                  m_selectedOutput.clear();
                  if (m_revertDialog != nullptr && m_revertDialog->isOpen()) {
                    m_revertDialog->close();
                  }
                  if (m_onClosed) {
                    m_onClosed();
                  }
                },
        }
    );
    if (!isOpen()) {
      m_display = nullptr;
    }
  }

  void MonitorEditorModal::setSelectedOutput(std::string name) {
    if (m_selectedOutput == name) {
      return;
    }
    m_selectedOutput = std::move(name);
    if (isOpen()) {
      m_sheet.rebuildBody();
    }
  }

  void MonitorEditorModal::populateBody(Flex& body) {
    if (m_display != nullptr && !m_display->writable()) {
      body.addChild(
          ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = Style::spaceXs * m_scale,
                  .padding = Style::spaceMd * m_scale,
                  .fillWidth = true,
                  .configure =
                      [this](Flex& box) {
                        box.setRadius(Style::scaledRadiusMd(m_scale));
                        box.setFill(colorSpecFromRole(ColorRole::Error, 0.15F));
                        box.setBorder(colorSpecFromRole(ColorRole::Error), Style::borderWidth);
                      },
              },
              ui::label({
                  .text = i18n::tr("settings.display.hw-control-unavailable"),
                  .fontSize = Style::fontSizeBody * m_scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::Error),
              }),
              ui::label({
                  .text = i18n::tr("settings.display.hw-control-unavailable-desc"),
                  .fontSize = Style::fontSizeCaption * m_scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 0,
              })
          )
      );
    }

    if (m_display != nullptr && !m_display->lastError().empty()) {
      body.addChild(
          ui::label({
              .text = m_display->lastError(),
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::Error),
          })
      );
    }

    if (m_display != nullptr) {
      body.addChild(
          ui::label({
              .text = i18n::tr("settings.display.monitor-drag-info"),
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      body.addChild(
          std::make_unique<MonitorLayoutCanvas>(
              *m_display, m_selectedOutput, [this](const std::string& name) { setSelectedOutput(name); }, m_scale
          )
      );
    }

    if (m_display != nullptr) {
      for (const auto& [name, output] : m_display->target()) {
        body.addChild(buildOutputCard(output));
      }
      if (m_display->target().empty() && !m_display->loading()) {
        body.addChild(
            ui::label({
                .text = i18n::tr("common.no-results"),
                .fontSize = Style::fontSizeBody * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        );
      }
    }
  }

  std::unique_ptr<Flex> MonitorEditorModal::buildOutputCard(const compositors::display::OutputState& output) {
    const bool selected = m_selectedOutput == output.name;
    const bool writable = m_display != nullptr && m_display->writable();

    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * m_scale,
        .padding = Style::spaceMd * m_scale,
        .fillWidth = true,
        .configure = [this, selected, writable](Flex& box) {
          box.setRadius(Style::scaledRadiusMd(m_scale));
          box.setFill(
              selected && writable ? colorSpecFromRole(ColorRole::Primary, 0.10F)
                                   : colorSpecFromRole(ColorRole::Surface)
          );
          box.setBorder(
              selected && writable ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::Outline),
              selected ? 2.0F : Style::borderWidth
          );
        },
    });

    card->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * m_scale,
            },
            ui::column(
                {
                    .align = FlexAlign::Stretch,
                    .gap = Style::spaceXs * m_scale,
                    .flexGrow = 1.0F,
                },
                ui::label({
                    .text = output.name,
                    .fontSize = Style::fontSizeBody * m_scale,
                    .fontWeight = FontWeight::Bold,
                    .color = selected ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::OnSurface),
                    .maxLines = 1,
                }),
                ui::label({
                    .text = output.make.empty() && output.model.empty()
                        ? i18n::tr("common.unknown")
                        : output.make + (output.make.empty() || output.model.empty() ? "" : " · ") + output.model,
                    .fontSize = Style::fontSizeCaption * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .maxLines = 1,
                })
            ),
            ui::button({
                .glyph = "info-circle",
                .glyphSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Default,
                .minWidth = Style::controlHeightSm * m_scale,
                .minHeight = Style::controlHeightSm * m_scale,
                .padding = Style::spaceXs * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this, name = output.name]() {
                  if (m_onOpenEdid) {
                    m_selectedOutput = name;
                    m_onOpenEdid(name);
                  }
                },
            })
        )
    );

    const auto sizes = predictedSizes(m_display->outputs(), m_display->target());
    const auto sizeIt = sizes.find(output.name);
    const OutputSize size = sizeIt != sizes.end() ? sizeIt->second : OutputSize{0, 0};

    card->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceLg * m_scale,
                .padding = Style::spaceSm * m_scale,
                .fillWidth = true,
                .configure =
                    [this](Flex& row) {
                      row.setRadius(Style::scaledRadiusSm(m_scale));
                      row.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.35F));
                    },
            },
            ui::column(
                {
                    .align = FlexAlign::Stretch,
                    .gap = Style::spaceXs * m_scale,
                },
                ui::label({
                    .text = i18n::tr("common.position"),
                    .fontSize = Style::fontSizeCaption * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                }),
                ui::label({
                    .text = "(" + std::to_string(output.x) + ", " + std::to_string(output.y) + ")",
                    .fontSize = Style::fontSizeBody * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                })
            ),
            ui::column(
                {
                    .align = FlexAlign::Stretch,
                    .gap = Style::spaceXs * m_scale,
                },
                ui::label({
                    .text = i18n::tr("settings.display.logical-size"),
                    .fontSize = Style::fontSizeCaption * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                }),
                ui::label({
                    .text = std::to_string(size.w) + "×" + std::to_string(size.h),
                    .fontSize = Style::fontSizeBody * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                })
            ),
            ui::column(
                {
                    .align = FlexAlign::Stretch,
                    .gap = Style::spaceXs * m_scale,
                },
                ui::label({
                    .text = i18n::tr("common.scale"),
                    .fontSize = Style::fontSizeCaption * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                }),
                ui::label({
                    .text = StringUtils::formatDotDecimal(output.scale) + "x",
                    .fontSize = Style::fontSizeBody * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                })
            ),
            ui::column(
                {
                    .align = FlexAlign::Stretch,
                    .gap = Style::spaceXs * m_scale,
                },
                ui::label({
                    .text = i18n::tr("settings.display.rotation"),
                    .fontSize = Style::fontSizeCaption * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                }),
                ui::label({
                    .text = transformLabel(output.transform),
                    .fontSize = Style::fontSizeBody * m_scale,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                })
            )
        )
    );

    if (!writable || !output.enabled) {
      return card;
    }

    const auto resolutions = uniqueResolutions(output.modes);
    const auto modeParts =
        output.modeStr.empty() ? std::string{"0x0"} : output.modeStr.substr(0, output.modeStr.find('@'));
    const auto resolutionIndex = std::ranges::find(resolutions, modeParts);
    const auto rates = sortedRatesForResolution(output.modes, modeParts);
    std::vector<std::string> rateLabels;
    rateLabels.reserve(rates.size());
    std::optional<std::size_t> selectedRate;
    for (std::size_t i = 0; i < rates.size(); ++i) {
      char buffer[32];
      std::snprintf(
          buffer, sizeof(buffer), "%.2f Hz%s", rates[i] / 1000.0,
          modeIsPreferred(output.modes, modeParts, rates[i]) ? " *" : ""
      );
      rateLabels.push_back(buffer);
      for (const auto& mode : output.modes) {
        if (mode.current && mode.refreshMhz == rates[i]) {
          selectedRate = i;
        }
      }
    }
    card->addChild(
        ui::row(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceMd * m_scale,
                .fillWidth = true,
            },
            controlColumn(
                i18n::tr("common.resolution"), m_scale,
                ui::select({
                    .options = resolutions,
                    .selectedIndex = resolutionIndex != resolutions.end()
                        ? std::optional<std::size_t>(static_cast<std::size_t>(resolutionIndex - resolutions.begin()))
                        : std::nullopt,
                    .clearSelection = resolutionIndex == resolutions.end(),
                    .fontSize = Style::fontSizeBody * m_scale,
                    .controlHeight = Style::controlHeight * m_scale,
                    .glyphSize = Style::fontSizeBody * m_scale,
                    .onSelectionChanged =
                        [this, name = output.name, resolutions](std::size_t index, std::string_view) {
                          if (m_display == nullptr || index >= resolutions.size()) {
                            return;
                          }
                          const auto& target = m_display->target();
                          const auto it = target.find(name);
                          if (it == target.end()) {
                            return;
                          }
                          const int rate = firstRateForResolution(it->second.modes, resolutions[index]);
                          m_display->setMode(name, resolutions[index] + "@" + compositors::display::formatRateHz(rate));
                        },
                    .configure = [](Select& select) { select.setFillWidth(true); },
                }),
                1.0F
            ),
            controlColumn(
                i18n::tr("settings.display.refresh-rate"), m_scale,
                ui::select({
                    .options = rateLabels,
                    .selectedIndex = selectedRate,
                    .clearSelection = !selectedRate.has_value(),
                    .fontSize = Style::fontSizeBody * m_scale,
                    .controlHeight = Style::controlHeight * m_scale,
                    .glyphSize = Style::fontSizeBody * m_scale,
                    .onSelectionChanged =
                        [this, name = output.name, modeParts, rates](std::size_t index, std::string_view) {
                          if (m_display == nullptr || index >= rates.size()) {
                            return;
                          }
                          m_display->setMode(name, modeParts + "@" + compositors::display::formatRateHz(rates[index]));
                        },
                    .configure = [](Select& select) { select.setFillWidth(true); },
                }),
                1.0F
            )
        )
    );

    std::vector<std::string> rotationLabels;
    rotationLabels.reserve(kTransforms.size());
    for (const auto& [transform, labelKey] : kTransforms) {
      rotationLabels.push_back(
          transform == "Flipped90" || transform == "Flipped180" || transform == "Flipped270"
              ? i18n::tr("settings.display.flipped-angle", "angle", transformLabel(transform))
              : transformLabel(transform)
      );
    }
    const auto transformIndex =
        std::ranges::find_if(kTransforms, [&output](const auto& option) { return option.first == output.transform; });

    {
      auto row = ui::row({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * m_scale,
          .fillWidth = true,
      });
      row->addChild(controlColumn(
          i18n::tr("common.scale"), m_scale,
          ui::stepper({
              .minValue = 25,
              .maxValue = 400,
              .step = 5,
              .value = static_cast<int>(std::lround(output.scale * 100.0)),
              .scale = m_scale,
              .valueSuffix = "%",
              .onValueCommitted =
                  [this, name = output.name](int value) {
                    if (m_display == nullptr) {
                      return;
                    }
                    m_scaleDebounceOutput = name;
                    m_scaleDebounceTimer.start(std::chrono::milliseconds(600), [this, value]() {
                      if (m_display != nullptr) {
                        m_display->setScale(m_scaleDebounceOutput, value / 100.0);
                      }
                    });
                  },
              .configure = [](Stepper& stepper) { stepper.setFillWidth(true); },
          }),
          1.0F
      ));
      row->addChild(controlColumn(
          i18n::tr("settings.display.rotation"), m_scale,
          ui::select({
              .options = rotationLabels,
              .selectedIndex = transformIndex != kTransforms.end()
                  ? std::optional<std::size_t>(static_cast<std::size_t>(transformIndex - kTransforms.begin()))
                  : std::nullopt,
              .clearSelection = transformIndex == kTransforms.end(),
              .fontSize = Style::fontSizeBody * m_scale,
              .controlHeight = Style::controlHeight * m_scale,
              .glyphSize = Style::fontSizeBody * m_scale,
              .onSelectionChanged =
                  [this, name = output.name](std::size_t index, std::string_view) {
                    if (m_display == nullptr || index >= kTransforms.size()) {
                      return;
                    }
                    m_display->setTransform(name, std::string(kTransforms[index].first));
                  },
              .configure = [](Select& select) { select.setFillWidth(true); },
          }),
          1.0F
      ));
      card->addChild(std::move(row));
    }

    if (m_display != nullptr && m_display->supportsHdr() && output.hdrSupported) {
      card->addChild(
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceSm * m_scale,
                  .fillWidth = true,
              },
              ui::label({
                  .text = i18n::tr("settings.display.hdr"),
                  .fontSize = Style::fontSizeCaption * m_scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 1.0F,
              }),
              ui::toggle({
                  .checked = output.hdr,
                  .scale = m_scale,
                  .onChange = [this, name = output.name](bool checked) {
                    if (m_display != nullptr) {
                      m_display->setHdr(name, checked);
                    }
                  },
              })
          )
      );
    }

    card->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * m_scale,
                .fillWidth = true,
            },
            ui::label({
                .text = i18n::tr("settings.display.vrr"),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            }),
            ui::toggle({
                .checked = output.vrr,
                .scale = m_scale,
                .onChange = [this, name = output.name](bool checked) {
                  if (m_display != nullptr) {
                    m_display->setVrr(name, checked);
                  }
                },
            })
        )
    );

    int enabledCount = 0;
    for (const auto& [otherName, other] : m_display->target()) {
      if (other.enabled) {
        enabledCount++;
      }
    }
    const bool isLastEnabled = output.enabled && enabledCount <= 1;
    card->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * m_scale,
                .fillWidth = true,
            },
            ui::label({
                .text = i18n::tr("common.enable-display"),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            }),
            ui::toggle({
                .checked = output.enabled,
                .enabled = !isLastEnabled,
                .scale = m_scale,
                .onChange = [this, name = output.name](bool checked) {
                  if (m_display != nullptr) {
                    m_display->toggleOutput(name, checked);
                  }
                },
            })
        )
    );

    return card;
  }

} // namespace settings::display
