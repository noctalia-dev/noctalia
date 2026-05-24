#include "shell/settings/settings_bar_management.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    std::unique_ptr<Label> makeLabel(
        std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal
    ) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .color = color,
          .fontWeight = fontWeight,
      });
    }

    std::unique_ptr<Button> makeManagementButton(
        std::string text, ButtonVariant variant, float scale, std::function<void()> onClick, std::string glyph = {},
        bool enabled = true
    ) {
      ui::ButtonProps props;
      props.text = std::move(text);
      if (!glyph.empty()) {
        props.glyph = std::move(glyph);
        props.glyphSize = Style::fontSizeCaption * scale;
      }
      props.fontSize = Style::fontSizeCaption * scale;
      props.enabled = enabled;
      props.variant = variant;
      props.minHeight = Style::controlHeightSm * scale;
      props.paddingV = Style::spaceXs * scale;
      props.paddingH = Style::spaceSm * scale;
      props.radius = Style::scaledRadiusSm(scale);
      props.onClick = std::move(onClick);
      return ui::button(std::move(props));
    }

    std::unique_ptr<Flex> makeConfirmPanel(float scale) {
      return ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceSm * scale,
          .configure = [scale](Flex& panel) {
            panel.setRadius(Style::scaledRadiusSm(scale));
            panel.setFill(colorSpecFromRole(ColorRole::Error, 0.10f));
            panel.setBorder(colorSpecFromRole(ColorRole::Error, 0.5f), Style::borderWidth);
          },
      });
    }

    std::string normalizedConfigId(std::string_view text) { return StringUtils::trim(text); }

    bool isValidConfigId(std::string_view text) {
      const auto trimmed = StringUtils::trim(text);
      if (trimmed.empty()) {
        return false;
      }
      return std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
      });
    }

    bool barNameExists(const Config& cfg, std::string_view name) {
      return std::any_of(cfg.bars.begin(), cfg.bars.end(), [name](const BarConfig& bar) { return bar.name == name; });
    }

    Flex* makeSection(Flex& content, std::string_view title, float scale, bool showBorder) {
      auto section = ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceSm * scale,
              .configure =
                  [scale, showBorder](Flex& container) {
                    container.setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
                    container.setCardStyle(scale, 1.0f, showBorder);
                    container.setFill(colorSpecFromRole(ColorRole::Surface));
                  },
          },
          makeLabel(title, Style::fontSizeTitle * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );

      auto* raw = section.get();
      content.addChild(std::move(section));
      return raw;
    }

    void addMonitorManagement(Flex& content, SettingsBarManagementContext& ctx) {
      if (ctx.searchQuery.empty() && ctx.selectedSection == "bar" && ctx.selectedBar != nullptr &&
          ctx.selectedMonitorOverride != nullptr && ctx.configService != nullptr &&
          ctx.configService->isOverrideOnlyMonitorOverride(ctx.selectedBar->name, ctx.selectedMonitorOverride->match)) {
        const std::string barName = ctx.selectedBar->name;
        const std::string match = ctx.selectedMonitorOverride->match;
        const bool pendingDelete =
            ctx.pendingDeleteMonitorOverrideBarName == barName && ctx.pendingDeleteMonitorOverrideMatch == match;
        const bool renaming =
            ctx.renamingMonitorOverrideBarName == barName && ctx.renamingMonitorOverrideMatch == match;
        auto* management = makeSection(
            content, i18n::tr("settings.entities.monitor-override.management"), ctx.scale,
            ctx.config.shell.panel.borders
        );

        if (renaming) {
          Input* inputPtr = nullptr;
          auto input = ui::input({
              .out = &inputPtr,
              .value = match,
              .placeholder = i18n::tr("settings.entities.monitor-override.match-placeholder"),
              .fontSize = Style::fontSizeBody * ctx.scale,
              .controlHeight = Style::controlHeight * ctx.scale,
              .horizontalPadding = Style::spaceSm * ctx.scale,
              .width = 190.0f * ctx.scale,
              .height = Style::controlHeight * ctx.scale,
              .flexGrow = 1.0f,
          });

          std::vector<std::string> existingMatches;
          existingMatches.reserve(ctx.selectedBar->monitorOverrides.size());
          for (const auto& monitorOverride : ctx.selectedBar->monitorOverrides) {
            if (monitorOverride.match != match) {
              existingMatches.push_back(monitorOverride.match);
            }
          }

          auto doRename = [&renamingMonitorOverrideBarName = ctx.renamingMonitorOverrideBarName,
                           &renamingMonitorOverrideMatch = ctx.renamingMonitorOverrideMatch, barName, match,
                           renameMonitorOverride = ctx.renameMonitorOverride, inputPtr,
                           existingMatches = std::move(existingMatches),
                           requestRebuild = ctx.requestRebuild](std::string rawMatch) {
            const std::string newMatch = normalizedConfigId(rawMatch);
            if (newMatch == match) {
              renamingMonitorOverrideBarName.clear();
              renamingMonitorOverrideMatch.clear();
              inputPtr->setInvalid(false);
              requestRebuild();
              return;
            }
            if (newMatch.empty() ||
                std::find(existingMatches.begin(), existingMatches.end(), newMatch) != existingMatches.end()) {
              inputPtr->setInvalid(true);
              return;
            }
            inputPtr->setInvalid(false);
            renameMonitorOverride(barName, match, newMatch);
          };

          inputPtr->setOnChange([inputPtr](const std::string& /*value*/) { inputPtr->setInvalid(false); });
          inputPtr->setOnSubmit([doRename](const std::string& text) mutable { doRename(text); });

          management->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceXs * ctx.scale,
                  },
                  std::move(input),
                  makeManagementButton(
                      i18n::tr("settings.entities.monitor-override.rename-save"), ButtonVariant::Default, ctx.scale,
                      [doRename, inputPtr]() mutable { doRename(inputPtr->value()); }
                  ),
                  makeManagementButton(
                      i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, ctx.scale,
                      [&renamingMonitorOverrideBarName = ctx.renamingMonitorOverrideBarName,
                       &renamingMonitorOverrideMatch = ctx.renamingMonitorOverrideMatch,
                       requestRebuild = ctx.requestRebuild]() {
                        renamingMonitorOverrideBarName.clear();
                        renamingMonitorOverrideMatch.clear();
                        requestRebuild();
                      }
                  )
              )
          );
        } else if (pendingDelete) {
          auto confirmPanel = makeConfirmPanel(ctx.scale);

          confirmPanel->addChild(makeLabel(
              i18n::tr("settings.entities.monitor-override.delete-confirm-title", "name", match),
              Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold
          ));
          confirmPanel->addChild(makeLabel(
              i18n::tr("settings.entities.monitor-override.delete-confirm-desc"), Style::fontSizeCaption * ctx.scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          ));

          confirmPanel->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceSm * ctx.scale,
                  },
                  ui::spacer(),
                  makeManagementButton(
                      i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, ctx.scale,
                      [&pendingDeleteMonitorOverrideBarName = ctx.pendingDeleteMonitorOverrideBarName,
                       &pendingDeleteMonitorOverrideMatch = ctx.pendingDeleteMonitorOverrideMatch,
                       requestRebuild = ctx.requestRebuild]() {
                        pendingDeleteMonitorOverrideBarName.clear();
                        pendingDeleteMonitorOverrideMatch.clear();
                        requestRebuild();
                      }
                  ),
                  makeManagementButton(
                      i18n::tr("settings.entities.monitor-override.delete"), ButtonVariant::Destructive, ctx.scale,
                      [deleteMonitorOverride = ctx.deleteMonitorOverride, barName, match]() {
                        deleteMonitorOverride(barName, match);
                      },
                      "trash"
                  )
              )
          );
          management->addChild(std::move(confirmPanel));
        } else {
          management->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceXs * ctx.scale,
                  },
                  ui::spacer(),
                  makeManagementButton(
                      i18n::tr("settings.entities.monitor-override.rename"), ButtonVariant::Ghost, ctx.scale,
                      [&renamingMonitorOverrideBarName = ctx.renamingMonitorOverrideBarName,
                       &renamingMonitorOverrideMatch = ctx.renamingMonitorOverrideMatch,
                       &pendingDeleteMonitorOverrideBarName = ctx.pendingDeleteMonitorOverrideBarName,
                       &pendingDeleteMonitorOverrideMatch = ctx.pendingDeleteMonitorOverrideMatch, barName, match,
                       requestRebuild = ctx.requestRebuild]() {
                        renamingMonitorOverrideBarName = barName;
                        renamingMonitorOverrideMatch = match;
                        pendingDeleteMonitorOverrideBarName.clear();
                        pendingDeleteMonitorOverrideMatch.clear();
                        requestRebuild();
                      }
                  ),
                  makeManagementButton(
                      i18n::tr("settings.entities.monitor-override.delete"), ButtonVariant::Ghost, ctx.scale,
                      [&pendingDeleteMonitorOverrideBarName = ctx.pendingDeleteMonitorOverrideBarName,
                       &pendingDeleteMonitorOverrideMatch = ctx.pendingDeleteMonitorOverrideMatch,
                       &renamingMonitorOverrideBarName = ctx.renamingMonitorOverrideBarName,
                       &renamingMonitorOverrideMatch = ctx.renamingMonitorOverrideMatch, barName, match,
                       requestRebuild = ctx.requestRebuild]() {
                        pendingDeleteMonitorOverrideBarName = barName;
                        pendingDeleteMonitorOverrideMatch = match;
                        renamingMonitorOverrideBarName.clear();
                        renamingMonitorOverrideMatch.clear();
                        requestRebuild();
                      },
                      "trash"
                  )
              )
          );
        }
      }
    }

    void addBarManagement(Flex& content, SettingsBarManagementContext& ctx) {
      if (ctx.searchQuery.empty() && ctx.selectedSection == "bar" && ctx.selectedBar != nullptr &&
          ctx.selectedMonitorOverride == nullptr && ctx.configService != nullptr) {
        const std::string barName = ctx.selectedBar->name;
        const bool overrideOnly = ctx.configService->isOverrideOnlyBar(barName);
        const bool canMoveUp = ctx.configService->canMoveBarOverride(barName, -1);
        const bool canMoveDown = ctx.configService->canMoveBarOverride(barName, 1);
        if (!overrideOnly && !canMoveUp && !canMoveDown) {
          return;
        }

        const bool pendingDelete = overrideOnly && ctx.pendingDeleteBarName == barName;
        const bool renaming = overrideOnly && ctx.renamingBarName == barName;
        auto* management = makeSection(
            content, i18n::tr("settings.entities.bar.management"), ctx.scale, ctx.config.shell.panel.borders
        );

        if (renaming) {
          Input* inputPtr = nullptr;
          auto input = ui::input({
              .out = &inputPtr,
              .value = barName,
              .placeholder = i18n::tr("settings.entities.bar.id-placeholder"),
              .fontSize = Style::fontSizeBody * ctx.scale,
              .controlHeight = Style::controlHeight * ctx.scale,
              .horizontalPadding = Style::spaceSm * ctx.scale,
              .width = 190.0f * ctx.scale,
              .height = Style::controlHeight * ctx.scale,
              .flexGrow = 1.0f,
          });

          auto doRename = [&renamingBarName = ctx.renamingBarName, config = ctx.config, barName,
                           renameBar = ctx.renameBar, inputPtr,
                           requestRebuild = ctx.requestRebuild](std::string rawName) {
            const std::string newName = normalizedConfigId(rawName);
            if (newName == barName) {
              renamingBarName.clear();
              inputPtr->setInvalid(false);
              requestRebuild();
              return;
            }
            if (!isValidConfigId(newName) || barNameExists(config, newName)) {
              inputPtr->setInvalid(true);
              return;
            }
            inputPtr->setInvalid(false);
            renameBar(barName, newName);
          };

          inputPtr->setOnChange([inputPtr](const std::string& /*value*/) { inputPtr->setInvalid(false); });
          inputPtr->setOnSubmit([doRename](const std::string& text) mutable { doRename(text); });

          management->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceXs * ctx.scale,
                  },
                  std::move(input),
                  makeManagementButton(
                      i18n::tr("settings.entities.bar.rename-save"), ButtonVariant::Default, ctx.scale,
                      [doRename, inputPtr]() mutable { doRename(inputPtr->value()); }
                  ),
                  makeManagementButton(
                      i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, ctx.scale,
                      [&renamingBarName = ctx.renamingBarName, requestRebuild = ctx.requestRebuild]() {
                        renamingBarName.clear();
                        requestRebuild();
                      }
                  )
              )
          );
        } else if (pendingDelete) {
          auto confirmPanel = makeConfirmPanel(ctx.scale);

          confirmPanel->addChild(makeLabel(
              i18n::tr("settings.entities.bar.delete-confirm-title", "name", barName), Style::fontSizeBody * ctx.scale,
              colorSpecFromRole(ColorRole::Error), FontWeight::Bold
          ));
          confirmPanel->addChild(makeLabel(
              i18n::tr("settings.entities.bar.delete-confirm-desc"), Style::fontSizeCaption * ctx.scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          ));

          confirmPanel->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceSm * ctx.scale,
                  },
                  ui::spacer(),
                  makeManagementButton(
                      i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, ctx.scale,
                      [&pendingDeleteBarName = ctx.pendingDeleteBarName, requestRebuild = ctx.requestRebuild]() {
                        pendingDeleteBarName.clear();
                        requestRebuild();
                      }
                  ),
                  makeManagementButton(
                      i18n::tr("settings.entities.bar.delete"), ButtonVariant::Destructive, ctx.scale,
                      [deleteBar = ctx.deleteBar, barName]() { deleteBar(barName); }, "trash"
                  )
              )
          );
          management->addChild(std::move(confirmPanel));
        } else {
          auto actionRow = ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
          });
          actionRow->addChild(ui::spacer());

          if (canMoveUp || canMoveDown) {
            actionRow->addChild(makeManagementButton(
                i18n::tr("settings.entities.bar.move-up"), ButtonVariant::Ghost, ctx.scale,
                [moveBar = ctx.moveBar, barName]() { moveBar(barName, -1); }, "chevron-up", canMoveUp
            ));
            actionRow->addChild(makeManagementButton(
                i18n::tr("settings.entities.bar.move-down"), ButtonVariant::Ghost, ctx.scale,
                [moveBar = ctx.moveBar, barName]() { moveBar(barName, 1); }, "chevron-down", canMoveDown
            ));
          }

          if (overrideOnly) {
            actionRow->addChild(makeManagementButton(
                i18n::tr("settings.entities.bar.rename"), ButtonVariant::Ghost, ctx.scale,
                [&renamingBarName = ctx.renamingBarName, &pendingDeleteBarName = ctx.pendingDeleteBarName, barName,
                 requestRebuild = ctx.requestRebuild]() {
                  renamingBarName = barName;
                  pendingDeleteBarName.clear();
                  requestRebuild();
                }
            ));
          }

          if (ctx.configService->canDeleteBarOverride(barName)) {
            actionRow->addChild(makeManagementButton(
                i18n::tr("settings.entities.bar.delete"), ButtonVariant::Ghost, ctx.scale,
                [&pendingDeleteBarName = ctx.pendingDeleteBarName, &renamingBarName = ctx.renamingBarName, barName,
                 requestRebuild = ctx.requestRebuild]() {
                  pendingDeleteBarName = barName;
                  renamingBarName.clear();
                  requestRebuild();
                },
                "trash"
            ));
          }

          management->addChild(std::move(actionRow));
        }
      }
    }

  } // namespace

  void addSettingsBarManagement(Flex& content, SettingsBarManagementContext ctx) {
    addMonitorManagement(content, ctx);
    addBarManagement(content, ctx);
  }

} // namespace settings
