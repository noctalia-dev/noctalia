#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "system/directory_scanner.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addShellSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = env.shellAvatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true)
        },
        "image picture"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.circular-avatar.label"),
        tr("settings.schema.shell.circular-avatar.description"), {"shell", "circular_avatar"},
        ToggleSetting{cfg.shell.circularAvatar},
        "avatar profile picture launcher control center circle radius roundness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.settings-window-translucent.label"),
        tr("settings.schema.shell.settings-window-translucent.description"), {"shell", "settings_window_translucent"},
        ToggleSetting{cfg.shell.settingsWindowTranslucent},
        "settings window background alpha transparency translucent opacity"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.settings-expand-all-groups.label"),
        tr("settings.schema.shell.settings-expand-all-groups.description"), {"shell", "settings_expand_all_groups"},
        ToggleSetting{cfg.shell.settingsExpandAllGroups}, "settings window groups expand collapse expanded"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.time-format.label"),
        tr("settings.schema.shell.time-format.description"), {"shell", "time_format"},
        TextSetting{.value = cfg.shell.timeFormat, .placeholder = "{:%H:%M}", .browseFileExtensions = {}},
        "clock time format strftime chrono"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.date-format.label"),
        tr("settings.schema.shell.date-format.description"), {"shell", "date_format"},
        TextSetting{.value = cfg.shell.dateFormat, .placeholder = "%A, %x", .browseFileExtensions = {}},
        "calendar date format strftime chrono"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.readline-shortcuts.label"),
        tr("settings.schema.shell.readline-shortcuts.description"), {"shell", "readline_shortcuts"},
        ToggleSetting{cfg.shell.readlineShortcuts}, "readline emacs keyboard shortcuts text input line editing"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "keyboard-layout", tr("settings.schema.shell.keyboard-layout-custom-labels.label"),
        tr("settings.schema.shell.keyboard-layout-custom-labels.description"),
        {"shell", "keyboard_layout", "custom_labels"},
        StringMapSetting{
            .entries = cfg.shell.keyboardLayout.customLabels,
            .suggestedKeys = env.keyboardLayoutNames,
            .keyPlaceholder = tr("settings.widgets.map-placeholders.layout-name"),
            .valuePlaceholder = tr("settings.widgets.map-placeholders.value"),
        },
        "keyboard layout nickname alias label"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-as-systemd-services.label"),
        env.systemdUserManaged ? tr("settings.schema.shell.launch-apps-as-systemd-services.description")
                               : tr("settings.schema.shell.launch-apps-as-systemd-services.requires-systemd-session"),
        {"shell", "launch_apps_as_systemd_services"},
        ToggleSetting{
            .checked = cfg.shell.launchAppsAsSystemdServices,
            // Keep a leftover `true` switchable off even when the session cannot honor it.
            .enabled = env.systemdUserManaged || cfg.shell.launchAppsAsSystemdServices
        }
    ));
    {
      auto e = makeEntry(
          SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-custom-command.label"),
          tr("settings.schema.shell.launch-apps-custom-command.description"), {"shell", "launch_apps_custom_command"},
          TextSetting{
              .value = cfg.shell.launchAppsCustomCommand,
              .placeholder = tr("settings.schema.shell.launch-apps-custom-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {},
          },
          "app command custom launcher dock taskbar"
      );
      e.visibleWhen = [managed = env.systemdUserManaged](const Config& c) {
        return !(c.shell.launchAppsAsSystemdServices && managed);
      };
      entries.push_back(std::move(e));
    }
    const SettingVisibility clipboardOn = [](const Config& c) { return c.shell.clipboardEnabled; };
    entries.push_back(makeEntry(
        SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-enabled.label"),
        tr("settings.schema.shell.clipboard-enabled.description"), {"shell", "clipboard_enabled"},
        ToggleSetting{cfg.shell.clipboardEnabled}, "clipboard history paste copy"
    ));
    // Deliberately not gated on clipboardOn: this keeps the live selection
    // alive, which is part of the transport that stays active when history
    // retention is turned off.
    entries.push_back(makeEntry(
        SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-keep-from-closed-apps.label"),
        tr("settings.schema.shell.clipboard-keep-from-closed-apps.description"),
        {"shell", "clipboard_keep_from_closed_apps"}, ToggleSetting{cfg.shell.clipboardKeepFromClosedApps},
        "clipboard paste close quit exit persist"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-history-max-entries.label"),
          tr("settings.schema.shell.clipboard-history-max-entries.description"),
          {"shell", "clipboard_history_max_entries"},
          StepperSetting{
              .value = cfg.shell.clipboardHistoryMaxEntries,
              .minValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.min.value()),
              .maxValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.max.value()),
              .step = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.step.value())
          },
          "clipboard history limit entries"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-confirm-clear-history.label"),
          tr("settings.schema.shell.clipboard-confirm-clear-history.description"),
          {"shell", "clipboard_confirm_clear_history"}, ToggleSetting{cfg.shell.clipboardConfirmClearHistory},
          "clipboard history clear confirm pinned"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-auto-paste.label"),
          tr("settings.schema.shell.clipboard-auto-paste.description"), {"shell", "clipboard_auto_paste"},
          enumSelect(kClipboardAutoPasteModes, cfg.shell.clipboardAutoPaste), "clipboard paste"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-image-action.label"),
          tr("settings.schema.shell.clipboard-image-action.description"), {"shell", "clipboard_image_action_command"},
          TextSetting{
              .value = cfg.shell.clipboardImageActionCommand,
              .placeholder = tr("settings.schema.shell.clipboard-image-action.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "clipboard image action annotation editor external gimp satty gradia"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
  }

  void addScreenshotSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture", tr("settings.schema.shell.screenshot-freeze-screen.label"),
        tr("settings.schema.shell.screenshot-freeze-screen.description"), {"shell", "screenshot", "freeze_screen"},
        ToggleSetting{cfg.shell.screenshot.freezeScreen}, "screenshot capture freeze region selection"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture", tr("settings.schema.shell.screenshot-confirm-region.label"),
        tr("settings.schema.shell.screenshot-confirm-region.description"), {"shell", "screenshot", "confirm_region"},
        ToggleSetting{cfg.shell.screenshot.confirmRegion}, "screenshot capture confirm region selection"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture",
        tr("settings.schema.shell.screenshot-remember-last-region.label"),
        tr("settings.schema.shell.screenshot-remember-last-region.description"),
        {"shell", "screenshot", "remember_last_region"}, ToggleSetting{cfg.shell.screenshot.rememberLastRegion},
        "screenshot capture remember last region selection"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture", tr("settings.schema.shell.screenshot-region-modifier.label"),
        tr("settings.schema.shell.screenshot-region-modifier.description"), {"shell", "screenshot", "region_modifier"},
        enumSelect(kScreenshotRegionModifiers, cfg.shell.screenshot.regionModifier),
        "screenshot region modifier move resize drag selection captures"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture", tr("settings.schema.shell.screenshot-show-cursor.label"),
        tr("settings.schema.shell.screenshot-show-cursor.description"), {"shell", "screenshot", "show_cursor"},
        ToggleSetting{cfg.shell.screenshot.showCursor}, "screenshot capture show cursor pointer mouse"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture",
        tr("settings.schema.shell.screenshot-region-border-width.label"),
        tr("settings.schema.shell.screenshot-region-border-width.description"),
        {"shell", "screenshot", "region_border_width"},
        sliderFor(
            cfg.shell.screenshot.regionBorderWidth, noctalia::config::schema::kScreenshotRegionBorderWidthRange, false
        ),
        "screenshot region border outline width stroke"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture",
        tr("settings.schema.shell.screenshot-region-dim-opacity.label"),
        tr("settings.schema.shell.screenshot-region-dim-opacity.description"),
        {"shell", "screenshot", "region_dim_opacity"},
        sliderFor(cfg.shell.screenshot.regionDimOpacity, noctalia::config::schema::kUnitRange, false),
        "screenshot region dim opacity darkness overlay"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture",
        tr("settings.schema.shell.screenshot-show-region-hint.label"),
        tr("settings.schema.shell.screenshot-show-region-hint.description"),
        {"shell", "screenshot", "show_region_hint"}, ToggleSetting{cfg.shell.screenshot.showRegionHint},
        "screenshot region confirm hint keybind copy save cancel"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-capture",
        tr("settings.schema.shell.screenshot-show-region-dimensions.label"),
        tr("settings.schema.shell.screenshot-show-region-dimensions.description"),
        {"shell", "screenshot", "show_region_dimensions"}, ToggleSetting{cfg.shell.screenshot.showRegionDimensions},
        "screenshot region dimensions size tooltip badge"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-annotation", tr("settings.schema.shell.screenshot-annotate.label"),
        tr("settings.schema.shell.screenshot-annotate.description"), {"shell", "screenshot", "annotate"},
        ToggleSetting{cfg.shell.screenshot.annotate}, "screenshot annotate annotation draw edit markup"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-annotation",
        tr("settings.schema.shell.screenshot-skip-annotate-on-copy-save.label"),
        tr("settings.schema.shell.screenshot-skip-annotate-on-copy-save.description"),
        {"shell", "screenshot", "skip_annotate_on_copy_save"},
        ToggleSetting{cfg.shell.screenshot.skipAnnotateOnCopySave},
        "screenshot annotate annotation skip copy save region quick"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-annotation",
        tr("settings.schema.shell.screenshot-close-on-copy.label"),
        tr("settings.schema.shell.screenshot-close-on-copy.description"), {"shell", "screenshot", "close_on_copy"},
        ToggleSetting{cfg.shell.screenshot.closeOnCopy}, "screenshot annotation close copy clipboard exit"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-annotation",
        tr("settings.schema.shell.screenshot-close-on-save.label"),
        tr("settings.schema.shell.screenshot-close-on-save.description"), {"shell", "screenshot", "close_on_save"},
        ToggleSetting{cfg.shell.screenshot.closeOnSave}, "screenshot annotation close save exit"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-annotation",
        tr("settings.schema.shell.screenshot-copy-on-close.label"),
        tr("settings.schema.shell.screenshot-copy-on-close.description"), {"shell", "screenshot", "copy_on_close"},
        ToggleSetting{cfg.shell.screenshot.copyOnClose}, "screenshot annotation cancel copy clipboard close exit"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-output",
        tr("settings.schema.shell.screenshot-copy-to-clipboard.label"),
        tr("settings.schema.shell.screenshot-copy-to-clipboard.description"),
        {"shell", "screenshot", "copy_to_clipboard"}, ToggleSetting{cfg.shell.screenshot.copyToClipboard},
        "screenshot capture clipboard copy"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-save-to-file.label"),
        tr("settings.schema.shell.screenshot-save-to-file.description"), {"shell", "screenshot", "save_to_file"},
        ToggleSetting{cfg.shell.screenshot.saveToFile}, "screenshot capture save file"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-directory.label"),
          tr("settings.schema.shell.screenshot-directory.description"), {"shell", "screenshot", "directory"},
          TextSetting{
              .value = cfg.shell.screenshot.directory,
              .placeholder = tr("settings.schema.shell.screenshot-directory.placeholder"),
              .browseMode = TextSettingBrowseMode::SelectFolder,
              .browseFileExtensions = {}
          },
          "screenshot capture directory folder save location"
      );
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output",
          tr("settings.schema.shell.screenshot-filename-pattern.label"),
          tr("settings.schema.shell.screenshot-filename-pattern.description"),
          {"shell", "screenshot", "filename_pattern"},
          TextSetting{
              .value = cfg.shell.screenshot.filenamePattern,
              .placeholder = "screenshot_%Y%m%d_%H%M%S",
              .browseFileExtensions = {}
          },
          "screenshot capture filename pattern strftime", true
      );
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-output",
        tr("settings.schema.shell.screenshot-append-mode-suffix.label"),
        tr("settings.schema.shell.screenshot-append-mode-suffix.description"),
        {"shell", "screenshot", "append_mode_suffix"}, ToggleSetting{cfg.shell.screenshot.appendModeSuffix},
        "screenshot capture filename suffix region annotated window desktop mode"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-format.label"),
        tr("settings.schema.shell.screenshot-format.description"), {"shell", "screenshot", "format"},
        enumSelect(kScreenshotFormats, cfg.shell.screenshot.format),
        "screenshot capture save file format png jpeg jpg webp jxl jpeg-xl"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-quality.label"),
          tr("settings.schema.shell.screenshot-quality.description"), {"shell", "screenshot", "quality"},
          sliderFor(cfg.shell.screenshot.quality, noctalia::config::schema::kScreenshotQualityRange, true),
          "screenshot capture save file quality compression lossless jpeg webp jxl"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.format != ScreenshotFormat::Png; };
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-pipe-to-command.label"),
        tr("settings.schema.shell.screenshot-pipe-to-command.description"), {"shell", "screenshot", "pipe_to_command"},
        ToggleSetting{cfg.shell.screenshot.pipeToCommand}, "screenshot capture run pipe command stdin"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-pipe-command.label"),
          tr("settings.schema.shell.screenshot-pipe-command.description"), {"shell", "screenshot", "pipe_command"},
          TextSetting{
              .value = cfg.shell.screenshot.pipeCommand,
              .placeholder = tr("settings.schema.shell.screenshot-pipe-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "screenshot capture run pipe command stdin png"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.pipeToCommand; };
      entries.push_back(std::move(e));
    }
  }

  void addWindowSwitcherSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-style.label"),
        tr("settings.schema.shell.window-switcher-style.description"), {"shell", "window_switcher", "style"},
        enumSelect(ShellConfig::kWindowSwitcherStyles, cfg.shell.windowSwitcher.style),
        "window switcher alt tab style carousel compact grid icons"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-mru.label"),
        tr("settings.schema.shell.window-switcher-mru.description"), {"shell", "window_switcher", "mru"},
        ToggleSetting{cfg.shell.windowSwitcher.mru}, "window switcher alt tab mru most recently used"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-show-caption.label"),
        tr("settings.schema.shell.window-switcher-show-caption.description"),
        {"shell", "window_switcher", "show_caption"}, ToggleSetting{cfg.shell.windowSwitcher.showCaption},
        "window switcher alt tab caption title application name"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-show-count.label"),
        tr("settings.schema.shell.window-switcher-show-count.description"), {"shell", "window_switcher", "show_count"},
        ToggleSetting{cfg.shell.windowSwitcher.showCount}, "window switcher alt tab count position total"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-show-app-icon.label"),
        tr("settings.schema.shell.window-switcher-show-app-icon.description"),
        {"shell", "window_switcher", "show_app_icon"}, ToggleSetting{cfg.shell.windowSwitcher.showAppIcon},
        "window switcher alt tab application app icon"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shared.colorize-icons.label"),
        tr("settings.schema.shared.colorize-icons.description"), {"shell", "window_switcher", "colorize_icons"},
        ToggleSetting{cfg.shell.windowSwitcher.colorizeIcons}, "window switcher colorize tint application icons"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher", tr("settings.schema.shell.window-switcher-show-all-outputs.label"),
        tr("settings.schema.shell.window-switcher-show-all-outputs.description"),
        {"shell", "window_switcher", "show_all_outputs"}, ToggleSetting{cfg.shell.windowSwitcher.showAllOutputs},
        "window switcher alt tab monitor display output screen all current"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "window-switcher",
        tr("settings.schema.shell.window-switcher-current-workspace-only.label"),
        tr("settings.schema.shell.window-switcher-current-workspace-only.description"),
        {"shell", "window_switcher", "current_workspace_only"},
        ToggleSetting{cfg.shell.windowSwitcher.currentWorkspaceOnly},
        "window switcher alt tab workspace current only filter"
    ));
  }

} // namespace settings::registry
