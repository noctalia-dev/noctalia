#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"
#include "core/input/key_chord.h"

#include <optional>
#include <string>
#include <string_view>

namespace noctalia::config::schema {

  namespace {
    // Plain string, but emitted only when non-empty (shell.lang).
    template <typename Struct> Field<Struct> stringIfNonEmptyField(std::string Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
            if (auto v = tbl[key].value<std::string>()) {
              out.*member = *v;
            } else {
              warnWrongType(tbl, key, parentPath, diag, "a string");
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if (!(in.*member).empty()) {
              tbl.insert_or_assign(key, in.*member);
            }
          }
      );
    }

    const Schema<ShellConfig::AnimationConfig>& shellAnimationSchema() {
      static const Schema<ShellConfig::AnimationConfig> s = {
          field(&ShellConfig::AnimationConfig::enabled, "enabled"),
          field(&ShellConfig::AnimationConfig::speed, "speed", kAnimationSpeedRange),
      };
      return s;
    }

    const Schema<ShadowStyle>& shellShadowSchema() {
      static const Schema<ShadowStyle> s = {
          enumField(&ShadowStyle::direction, "direction", kShadowDirections),
          field(&ShadowStyle::alpha, "alpha", kUnitRange),
      };
      return s;
    }

    // Optional strings stored trimmed-or-nullopt, always emitted (value_or("")).
    Field<DmenuEntryConfig>
    dmenuOptionalString(std::optional<std::string> DmenuEntryConfig::* member, std::string_view key) {
      return custom<DmenuEntryConfig>(
          key,
          [member, key](const toml::table& tbl, DmenuEntryConfig& out, std::string_view parentPath, Diagnostics& diag) {
            if (auto v = tbl[key].value<std::string>()) {
              const std::string trimmed = StringUtils::trim(*v);
              out.*member = trimmed.empty() ? std::optional<std::string>{} : std::optional<std::string>{trimmed};
            } else {
              warnWrongType(tbl, key, parentPath, diag, "a string");
            }
          },
          [member, key](toml::table& tbl, const DmenuEntryConfig& in) {
            tbl.insert_or_assign(key, (in.*member).value_or(""));
          }
      );
    }

    const Schema<DmenuEntryConfig>& dmenuEntrySchema() {
      static const Schema<DmenuEntryConfig> s = {
          field(&DmenuEntryConfig::command, "command"),
          dmenuOptionalString(&DmenuEntryConfig::exec, "exec"),
          dmenuOptionalString(&DmenuEntryConfig::prefix, "prefix"),
          dmenuOptionalString(&DmenuEntryConfig::label, "label"),
          dmenuOptionalString(&DmenuEntryConfig::glyph, "glyph"),
          field(&DmenuEntryConfig::global, "global"),
          field(&DmenuEntryConfig::freeform, "freeform"),
      };
      return s;
    }

    const Schema<ShellConfig::LauncherConfig::DmenuConfig>& shellLauncherDmenuSchema() {
      static const Schema<ShellConfig::LauncherConfig::DmenuConfig> s = {
          namedMap<ShellConfig::LauncherConfig::DmenuConfig, DmenuEntryConfig>(
              &ShellConfig::LauncherConfig::DmenuConfig::entries, "entry", dmenuEntrySchema(),
              [](DmenuEntryConfig& e, std::string_view name) { e.id = std::string(name); },
              [](const DmenuEntryConfig& e) { return e.id; }
          ),
      };
      return s;
    }

    const Schema<ShellConfig::PanelConfig>& shellPanelSchema() {
      static const Schema<ShellConfig::PanelConfig> s = {
          enumField(&ShellConfig::PanelConfig::transparencyMode, "transparency_mode", kPanelTransparencyModes),
          field(&ShellConfig::PanelConfig::borders, "borders"),
          colorField(&ShellConfig::PanelConfig::borderColor, "border_color"),
          field(&ShellConfig::PanelConfig::borderWidth, "border_width", kPanelBorderWidthRange),
          field(&ShellConfig::PanelConfig::shadow, "shadow"),
          field(&ShellConfig::PanelConfig::listItemBackground, "list_item_background"),
          field(&ShellConfig::PanelConfig::wallpaperShowNames, "wallpaper_show_names"),
          custom<ShellConfig::PanelConfig>(
              "floating_layer",
              [](const toml::table& tbl, ShellConfig::PanelConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (auto v = tbl["floating_layer"].value<std::string>()) {
                  if (*v == "top" || *v == "overlay") {
                    out.floatingLayer = *v;
                  } else {
                    diag.warn(joinPath(parentPath, "floating_layer"), "expected top or overlay, got \"" + *v + "\"");
                  }
                } else {
                  warnWrongType(tbl, "floating_layer", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const ShellConfig::PanelConfig& in) {
                tbl.insert_or_assign("floating_layer", in.floatingLayer);
              }
          ),
          enumField(&ShellConfig::PanelConfig::launcherPlacement, "launcher_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::clipboardPlacement, "clipboard_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::controlCenterPlacement, "control_center_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::wallpaperPlacement, "wallpaper_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::sessionPlacement, "session_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::polkitPlacement, "polkit_placement", kPanelPlacements),
          field(&ShellConfig::PanelConfig::launcherPosition, "launcher_position"),
          field(&ShellConfig::PanelConfig::clipboardPosition, "clipboard_position"),
          field(&ShellConfig::PanelConfig::controlCenterPosition, "control_center_position"),
          field(&ShellConfig::PanelConfig::wallpaperPosition, "wallpaper_position"),
          field(&ShellConfig::PanelConfig::sessionPosition, "session_position"),
          field(&ShellConfig::PanelConfig::polkitPosition, "polkit_position"),
          field(&ShellConfig::PanelConfig::floatingOffset, "floating_offset", Range<std::int64_t>{0, 100}),
          field(&ShellConfig::PanelConfig::openNearClickControlCenter, "open_near_click_control_center"),
          field(&ShellConfig::PanelConfig::openNearClickLauncher, "open_near_click_launcher"),
          field(&ShellConfig::PanelConfig::openNearClickClipboard, "open_near_click_clipboard"),
          field(&ShellConfig::PanelConfig::openNearClickWallpaper, "open_near_click_wallpaper"),
          field(&ShellConfig::PanelConfig::openNearClickSession, "open_near_click_session"),
      };
      return s;
    }

    const Schema<LauncherProviderConfig>& launcherProviderSchema() {
      static const Schema<LauncherProviderConfig> s = {
          field(&LauncherProviderConfig::prefix, "prefix"),
          optionalBoolField(&LauncherProviderConfig::global, "global"),
      };
      return s;
    }

    const Schema<ShellConfig::LauncherConfig::PanelsConfig>& shellLauncherPanelsSchema() {
      static const Schema<ShellConfig::LauncherConfig::PanelsConfig> s = {
          field(&ShellConfig::LauncherConfig::PanelsConfig::ignored, "ignored"),
      };
      return s;
    }

    const Schema<ShellConfig::LauncherConfig>& shellLauncherSchema() {
      static const Schema<ShellConfig::LauncherConfig> s = {
          enumField(&ShellConfig::LauncherConfig::layout, "layout", ShellConfig::kLauncherLayouts),
          field(&ShellConfig::LauncherConfig::width, "width", noctalia::config::schema::kLauncherWidthRange),
          enumField(
              &ShellConfig::LauncherConfig::searchPosition, "search_position", ShellConfig::kLauncherSearchPositions
          ),
          enumField(&ShellConfig::LauncherConfig::defaultView, "default_view", ShellConfig::kLauncherDefaultViews),
          field(&ShellConfig::LauncherConfig::categories, "categories"),
          field(&ShellConfig::LauncherConfig::categoryIconsOnly, "category_icons_only"),
          field(&ShellConfig::LauncherConfig::showNavigationLabels, "show_navigation_labels"),
          field(&ShellConfig::LauncherConfig::showSettingsButton, "show_settings_button"),
          field(&ShellConfig::LauncherConfig::showSessionButton, "show_session_button"),
          field(&ShellConfig::LauncherConfig::showIcons, "show_icons"),
          field(&ShellConfig::LauncherConfig::colorizeIcons, "colorize_icons"),
          field(&ShellConfig::LauncherConfig::showAppOriginIndicator, "show_app_origin_indicator"),
          field(&ShellConfig::LauncherConfig::compact, "compact"),
          field(&ShellConfig::LauncherConfig::appGrid, "app_grid"),
          field(&ShellConfig::LauncherConfig::showAppActions, "show_app_actions"),
          field(&ShellConfig::LauncherConfig::trackUsage, "track_usage"),
          field(&ShellConfig::LauncherConfig::sortByUsage, "sort_by_usage"),
          field(&ShellConfig::LauncherConfig::ignoreMouse, "ignore_mouse"),
          field(&ShellConfig::LauncherConfig::pinned, "pinned"),
          field(&ShellConfig::LauncherConfig::fetchExchangeRates, "fetch_exchange_rates"),
          field(&ShellConfig::LauncherConfig::providerPrefix, "provider_prefix"),
          enumField(&ShellConfig::LauncherConfig::autoPaste, "auto_paste", kClipboardAutoPasteModes),
          subTable(&ShellConfig::LauncherConfig::dmenu, "dmenu", shellLauncherDmenuSchema()),
          subTable(&ShellConfig::LauncherConfig::panels, "panels", shellLauncherPanelsSchema()),
          namedMap<ShellConfig::LauncherConfig, LauncherProviderConfig>(
              &ShellConfig::LauncherConfig::providers, "providers", launcherProviderSchema(),
              [](LauncherProviderConfig& elem, std::string_view name) { elem.name = StringUtils::toLower(name); },
              [](const LauncherProviderConfig& elem) { return elem.name; }
          ),
      };
      return s;
    }

    const Schema<ShellConfig::KeyboardLayoutConfig>& shellKeyboardLayoutSchema() {
      static const Schema<ShellConfig::KeyboardLayoutConfig> s = {
          field(&ShellConfig::KeyboardLayoutConfig::customLabels, "custom_labels"),
      };
      return s;
    }

    const Schema<ShellConfig::WindowSwitcherConfig>& shellWindowSwitcherSchema() {
      static const Schema<ShellConfig::WindowSwitcherConfig> s = {
          enumField(&ShellConfig::WindowSwitcherConfig::style, "style", ShellConfig::kWindowSwitcherStyles),
          field(&ShellConfig::WindowSwitcherConfig::mru, "mru"),
          field(&ShellConfig::WindowSwitcherConfig::showCaption, "show_caption"),
          field(&ShellConfig::WindowSwitcherConfig::showCount, "show_count"),
          field(&ShellConfig::WindowSwitcherConfig::showAppIcon, "show_app_icon"),
          field(&ShellConfig::WindowSwitcherConfig::colorizeIcons, "colorize_icons"),
          field(&ShellConfig::WindowSwitcherConfig::showAllOutputs, "show_all_outputs"),
          field(&ShellConfig::WindowSwitcherConfig::currentWorkspaceOnly, "current_workspace_only"),
      };
      return s;
    }

    const Schema<ShellConfig::ScreenCornersConfig>& shellScreenCornersSchema() {
      static const Schema<ShellConfig::ScreenCornersConfig> s = {
          field(&ShellConfig::ScreenCornersConfig::enabled, "enabled"),
          field(&ShellConfig::ScreenCornersConfig::size, "size", kScreenCornersSizeRange),
      };
      return s;
    }

    const Schema<ShellConfig::MprisConfig>& shellMprisSchema() {
      static const Schema<ShellConfig::MprisConfig> s = {
          field(&ShellConfig::MprisConfig::blacklist, "blacklist"),
      };
      return s;
    }

    // NOTE: the serializer previously never emitted [shell.screenshot] (read-only gap);
    // including it here fixes the export, mirroring the calendar gap-fix.
    const Schema<ShellConfig::ScreenshotConfig>& shellScreenshotSchema() {
      static const Schema<ShellConfig::ScreenshotConfig> s = {
          field(&ShellConfig::ScreenshotConfig::saveToFile, "save_to_file"),
          field(&ShellConfig::ScreenshotConfig::copyToClipboard, "copy_to_clipboard"),
          field(&ShellConfig::ScreenshotConfig::freezeScreen, "freeze_screen"),
          field(&ShellConfig::ScreenshotConfig::confirmRegion, "confirm_region"),
          field(&ShellConfig::ScreenshotConfig::rememberLastRegion, "remember_last_region"),
          enumField(&ShellConfig::ScreenshotConfig::regionModifier, "region_modifier", kScreenshotRegionModifiers),
          field(
              &ShellConfig::ScreenshotConfig::regionBorderWidth, "region_border_width",
              kScreenshotRegionBorderWidthRange
          ),
          field(&ShellConfig::ScreenshotConfig::regionDimOpacity, "region_dim_opacity", kUnitRange),
          field(&ShellConfig::ScreenshotConfig::showRegionHint, "show_region_hint"),
          field(&ShellConfig::ScreenshotConfig::showRegionDimensions, "show_region_dimensions"),
          field(&ShellConfig::ScreenshotConfig::showCursor, "show_cursor"),
          field(&ShellConfig::ScreenshotConfig::annotate, "annotate"),
          field(&ShellConfig::ScreenshotConfig::skipAnnotateOnCopySave, "skip_annotate_on_copy_save"),
          field(&ShellConfig::ScreenshotConfig::closeOnCopy, "close_on_copy"),
          field(&ShellConfig::ScreenshotConfig::closeOnSave, "close_on_save"),
          field(&ShellConfig::ScreenshotConfig::copyOnClose, "copy_on_close"),
          field(&ShellConfig::ScreenshotConfig::pipeToCommand, "pipe_to_command"),
          field(&ShellConfig::ScreenshotConfig::pipeCommand, "pipe_command"),
          field(&ShellConfig::ScreenshotConfig::directory, "directory"),
          field(&ShellConfig::ScreenshotConfig::filenamePattern, "filename_pattern"),
          field(&ShellConfig::ScreenshotConfig::appendModeSuffix, "append_mode_suffix"),
          enumField(&ShellConfig::ScreenshotConfig::format, "format", kScreenshotFormats),
          field(&ShellConfig::ScreenshotConfig::quality, "quality", kScreenshotQualityRange),
      };
      return s;
    }

    const Schema<ShellConfig::PrivacyConfig>& shellPrivacySchema() {
      static const Schema<ShellConfig::PrivacyConfig> s = {
          field(&ShellConfig::PrivacyConfig::micFilterRegex, "mic_filter_regex"),
          field(&ShellConfig::PrivacyConfig::camFilterRegex, "cam_filter_regex"),
          field(&ShellConfig::PrivacyConfig::screenFilterRegex, "screen_filter_regex"),
      };
      return s;
    }

    // command/label/glyph are stored trimmed-or-nullopt but always emitted (value_or("")).
    Field<SessionPanelActionConfig>
    sessionOptionalString(std::optional<std::string> SessionPanelActionConfig::* member, std::string_view key) {
      return custom<SessionPanelActionConfig>(
          key,
          [member,
           key](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view parentPath, Diagnostics& diag) {
            if (auto v = tbl[key].value<std::string>()) {
              const std::string trimmed = StringUtils::trim(*v);
              out.*member = trimmed.empty() ? std::optional<std::string>{} : std::optional<std::string>{trimmed};
            } else {
              warnWrongType(tbl, key, parentPath, diag, "a string");
            }
          },
          [member, key](toml::table& tbl, const SessionPanelActionConfig& in) {
            tbl.insert_or_assign(key, (in.*member).value_or(""));
          }
      );
    }

    const Schema<SessionPanelActionConfig>& sessionActionSchema() {
      static const Schema<SessionPanelActionConfig> s = {
          custom<SessionPanelActionConfig>(
              "action",
              [](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (auto v = tbl["action"].value<std::string>()) {
                  out.action = StringUtils::toLower(StringUtils::trim(*v));
                } else {
                  warnWrongType(tbl, "action", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const SessionPanelActionConfig& in) { tbl.insert_or_assign("action", in.action); }
          ),
          field(&SessionPanelActionConfig::enabled, "enabled"),
          sessionOptionalString(&SessionPanelActionConfig::command, "command"),
          sessionOptionalString(&SessionPanelActionConfig::label, "label"),
          sessionOptionalString(&SessionPanelActionConfig::glyph, "glyph"),
          enumField(&SessionPanelActionConfig::variant, "variant", kSessionActionButtonVariants),
          custom<SessionPanelActionConfig>(
              "shortcut",
              [](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (auto v = tbl["shortcut"].value<std::string>()) {
                  const std::string spec = StringUtils::trim(*v);
                  if (!spec.empty()) {
                    out.shortcut = parseKeyChordSpec(spec);
                  }
                } else {
                  warnWrongType(tbl, "shortcut", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const SessionPanelActionConfig& in) {
                tbl.insert_or_assign(
                    "shortcut", in.shortcut.has_value() ? keyChordToString(*in.shortcut) : std::string{}
                );
              }
          ),
          custom<SessionPanelActionConfig>(
              "lockscreen_shortcut",
              [](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (auto v = tbl["lockscreen_shortcut"].value<std::string>()) {
                  const std::string spec = StringUtils::trim(*v);
                  if (spec.empty()) {
                    return;
                  }
                  auto chord = parseKeyChordSpec(spec);
                  if (chord.has_value() && isTextKeyChord(*chord)) {
                    diag.warn(
                        joinPath(parentPath, "lockscreen_shortcut"),
                        "\""
                            + spec
                            + "\" types text into the password field; use a non-printable key or a Ctrl/Alt chord"
                    );
                    return;
                  }
                  out.lockscreenShortcut = chord;
                } else {
                  warnWrongType(tbl, "lockscreen_shortcut", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const SessionPanelActionConfig& in) {
                tbl.insert_or_assign(
                    "lockscreen_shortcut",
                    in.lockscreenShortcut.has_value() ? keyChordToString(*in.lockscreenShortcut) : std::string{}
                );
              }
          ),
          field(&SessionPanelActionConfig::countdownSeconds, "countdown_seconds"),
      };
      return s;
    }

    const Schema<ShellSessionConfig::ShellSessionPowerConfig>& shellSessionPowerSchema() {
      using Power = ShellSessionConfig::ShellSessionPowerConfig;
      static const Schema<Power> s = {
          optionalTrimmedStringField(&Power::suspend, "suspend"),
          optionalTrimmedStringField(&Power::reboot, "reboot"),
          optionalTrimmedStringField(&Power::shutdown, "shutdown"),
      };
      return s;
    }

    const Schema<ShellGreeterSyncConfig>& shellGreeterSyncSchema() {
      static const Schema<ShellGreeterSyncConfig> s = {
          field(&ShellGreeterSyncConfig::autoSync, "auto_sync"),
          custom<ShellGreeterSyncConfig>(
              "privilege_command",
              [](const toml::table& tbl, ShellGreeterSyncConfig& out, std::string_view parentPath, Diagnostics& diag) {
                if (auto v = tbl["privilege_command"].value<std::string>()) {
                  out.privilegeCommand = StringUtils::trim(*v);
                } else {
                  warnWrongType(tbl, "privilege_command", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const ShellGreeterSyncConfig& in) {
                if (!in.privilegeCommand.empty()) {
                  tbl.insert_or_assign("privilege_command", in.privilegeCommand);
                }
              }
          ),
      };
      return s;
    }

    const Schema<ShellSessionConfig>& shellSessionSchema() {
      static const Schema<ShellSessionConfig> s = {
          arrayOf<ShellSessionConfig, SessionPanelActionConfig>(
              &ShellSessionConfig::actions, "actions", sessionActionSchema(),
              [](const SessionPanelActionConfig& a) { return !a.action.empty(); }
          ),
          field(&ShellSessionConfig::grid, "grid"),
          field(&ShellSessionConfig::gridColumns, "grid_columns", kSessionGridColumnsRange),
          field(&ShellSessionConfig::showShortcuts, "show_shortcuts"),
          subTable(&ShellSessionConfig::power, "power", shellSessionPowerSchema()),
      };
      return s;
    }
  } // namespace

  const Schema<ShellConfig>& shellSchema() {
    static const Schema<ShellConfig> s = {
        field(&ShellConfig::cornerRadiusScale, "corner_radius_scale", kCornerRadiusScaleRange),
        field(&ShellConfig::buttonBorders, "button_borders"),
        field(&ShellConfig::inputBorders, "input_borders"),
        field(&ShellConfig::popupBorders, "popup_borders"),
        field(&ShellConfig::popupShadows, "popup_shadows"),
        field(&ShellConfig::cardBorders, "card_borders"),
        field(&ShellConfig::showCloseButtons, "show_close_buttons"),
        // font_family is trimmed; empty falls back to sans-serif.
        custom<ShellConfig>(
            "font_family",
            [](const toml::table& tbl, ShellConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (auto v = tbl["font_family"].value<std::string>()) {
                out.fontFamily = StringUtils::trim(*v);
                if (out.fontFamily.empty()) {
                  out.fontFamily = "sans-serif";
                }
              } else {
                warnWrongType(tbl, "font_family", parentPath, diag, "a string");
              }
            },
            [](toml::table& tbl, const ShellConfig& in) { tbl.insert_or_assign("font_family", in.fontFamily); }
        ),
        stringIfNonEmptyField(&ShellConfig::lang, "lang"),
        field(&ShellConfig::timeFormat, "time_format"),
        field(&ShellConfig::dateFormat, "date_format"),
        field(&ShellConfig::offlineMode, "offline_mode"),
        stringIfNonEmptyField(&ShellConfig::panelAnchorBar, "panel_anchor_bar"),
        field(&ShellConfig::externalIpEnabled, "external_ip_enabled"),
        field(&ShellConfig::telemetryEnabled, "telemetry_enabled"),
        field(&ShellConfig::setupWizardEnabled, "setup_wizard_enabled"),
        field(&ShellConfig::niriOverviewTypeToLaunchEnabled, "niri_overview_type_to_launch_enabled"),
        field(&ShellConfig::umbrielOverviewTypeToLaunchEnabled, "umbriel_overview_type_to_launch_enabled"),
        field(&ShellConfig::umbrielAudioProviderEnabled, "umbriel_audio_provider_enabled"),
        field(&ShellConfig::niriOverviewCloseOnLaunchEnabled, "niri_overview_close_on_launch_enabled"),
        field(&ShellConfig::umbrielOverviewCloseOnLaunchEnabled, "umbriel_overview_close_on_launch_enabled"),
        field(&ShellConfig::polkitAgent, "polkit_agent"),
        field(&ShellConfig::polkitColorizeIcons, "polkit_colorize_icons"),
        enumField(&ShellConfig::passwordMaskStyle, "password_style", kPasswordMaskStyles),
        field(&ShellConfig::readlineShortcuts, "readline_shortcuts"),
        field(&ShellConfig::settingsShowAdvanced, "settings_show_advanced"),
        field(&ShellConfig::settingsExpandAllGroups, "settings_expand_all_groups"),
        field(&ShellConfig::settingsWindowTranslucent, "settings_window_translucent"),
        field(&ShellConfig::showLocation, "show_location"),
        colorSpecField(&ShellConfig::appIconColor, "app_icon_color", /*alwaysEmit=*/false),
        field(&ShellConfig::launchAppsAsSystemdServices, "launch_apps_as_systemd_services"),
        field(&ShellConfig::launchAppsCustomCommand, "launch_apps_custom_command"),
        field(&ShellConfig::clipboardEnabled, "clipboard_enabled"),
        field(&ShellConfig::clipboardKeepFromClosedApps, "clipboard_keep_from_closed_apps"),
        field(
            &ShellConfig::clipboardHistoryMaxEntries, "clipboard_history_max_entries", kClipboardHistoryMaxEntriesRange
        ),
        field(&ShellConfig::clipboardConfirmClearHistory, "clipboard_confirm_clear_history"),
        field(&ShellConfig::screenTimeEnabled, "screen_time_enabled"),
        field(&ShellConfig::screenTimeColorizeIcons, "screen_time_colorize_icons"),
        field(&ShellConfig::sharedGlContext, "shared_gl_context"),
        field(&ShellConfig::disableMipmaps, "disable_mipmaps"),
        enumField(&ShellConfig::clipboardAutoPaste, "clipboard_auto_paste", kClipboardAutoPasteModes),
        field(&ShellConfig::clipboardImageActionCommand, "clipboard_image_action_command"),
        pathStringField(&ShellConfig::avatarPath, "avatar_path"),
        field(&ShellConfig::circularAvatar, "circular_avatar"),
        subTable(&ShellConfig::animation, "animation", shellAnimationSchema()),
        subTable(&ShellConfig::shadow, "shadow", shellShadowSchema()),
        subTable(&ShellConfig::panel, "panel", shellPanelSchema()),
        subTable(&ShellConfig::launcher, "launcher", shellLauncherSchema()),
        subTable(&ShellConfig::keyboardLayout, "keyboard_layout", shellKeyboardLayoutSchema()),
        subTable(&ShellConfig::windowSwitcher, "window_switcher", shellWindowSwitcherSchema()),
        subTable(&ShellConfig::screenCorners, "screen_corners", shellScreenCornersSchema()),
        subTable(&ShellConfig::mpris, "mpris", shellMprisSchema()),
        subTable(&ShellConfig::screenshot, "screenshot", shellScreenshotSchema()),
        subTable(&ShellConfig::privacy, "privacy", shellPrivacySchema()),
        subTable(&ShellConfig::session, "session", shellSessionSchema()),
        subTable(&ShellConfig::greeterSync, "greeter_sync", shellGreeterSyncSchema()),
    };
    return s;
  }

  const Schema<AccessibilityConfig>& accessibilitySchema() {
    static const Schema<AccessibilityConfig> s = {
        field(&AccessibilityConfig::uiScale, "ui_scale", kScaleRange),
        field(&AccessibilityConfig::highContrast, "high_contrast"),
    };
    return s;
  }

} // namespace noctalia::config::schema
