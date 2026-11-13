#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  namespace {

    // Launcher providers that expose a configurable prefix, keyed by the config name
    // (the lowercased provider id). Placeholders and global-search defaults mirror the
    // provider's own defaultPrefix()/defaultIncludeInGlobalSearch().
    struct LauncherProviderSettingSpec {
      std::string_view name;
      std::string_view prefixPlaceholder;
      bool globalByDefault = false;
    };

    constexpr auto kLauncherProviderSettings = std::to_array<LauncherProviderSettingSpec>({
        {.name = "calculator", .prefixPlaceholder = "calc", .globalByDefault = true},
        {.name = "emoji", .prefixPlaceholder = "emo"},
        {.name = "panels", .prefixPlaceholder = "pan"},
        {.name = "session", .prefixPlaceholder = "session"},
        {.name = "wallpaper", .prefixPlaceholder = "wall"},
        {.name = "windows", .prefixPlaceholder = "win"},
    });

  } // namespace

  void addLauncherSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "launcher", tr("settings.schema.panels.placement-launcher.label"),
        tr("settings.schema.panels.placement-launcher.description"), {"shell", "panel", "launcher_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.launcherPlacement)),
        "attached floating bar panel position"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::Launcher, "launcher", "launcher", "settings.schema.panels.position-launcher.label",
        "settings.schema.panels.position-launcher.description", cfg.shell.panel.launcherPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Launcher, "launcher", "launcher", "settings.schema.panels.open-near-click-launcher.label",
        "settings.schema.panels.open-near-click-launcher.description", cfg.shell.panel.openNearClickLauncher,
        &ShellConfig::PanelConfig::launcherPlacement, &ShellConfig::PanelConfig::launcherPosition
    ));
    const SettingVisibility launcherFull = [](const Config& c) {
      return c.shell.launcher.layout == ShellConfig::LauncherLayout::Full;
    };
    const SettingVisibility launcherSimple = [](const Config& c) {
      return c.shell.launcher.layout == ShellConfig::LauncherLayout::Simple;
    };
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-layout.label"),
        tr("settings.schema.panels.launcher-layout.description"), {"shell", "launcher", "layout"},
        asSegmented(enumSelect(ShellConfig::kLauncherLayouts, cfg.shell.launcher.layout)),
        "launcher layout simple full classic minimal compact toolbar tabs"
    ));
    {
      auto width = sliderFor(cfg.shell.launcher.width, noctalia::config::schema::kLauncherWidthRange, true);
      width.valueSuffix = "px";
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-width.label"),
          tr("settings.schema.panels.launcher-width.description"), {"shell", "launcher", "width"}, std::move(width),
          "launcher panel width size"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-search-position.label"),
          tr("settings.schema.panels.launcher-search-position.description"), {"shell", "launcher", "search_position"},
          asSegmented(enumSelect(ShellConfig::kLauncherSearchPositions, cfg.shell.launcher.searchPosition)),
          "launcher search toolbar position top bottom"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-default-view.label"),
          tr("settings.schema.panels.launcher-default-view.description"), {"shell", "launcher", "default_view"},
          enumSelect(ShellConfig::kLauncherDefaultViews, cfg.shell.launcher.defaultView),
          "launcher default view recent pinned all apps categories"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-categories.label"),
          tr("settings.schema.panels.launcher-categories.description"), {"shell", "launcher", "categories"},
          ToggleSetting{cfg.shell.launcher.categories}, "launcher categories chips filter recent all"
      );
      e.visibleWhen = launcherSimple;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-category-icons-only.label"),
        tr("settings.schema.panels.launcher-category-icons-only.description"),
        {"shell", "launcher", "category_icons_only"}, ToggleSetting{cfg.shell.launcher.categoryIconsOnly},
        "launcher app category icons labels text"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-navigation-labels.label"),
          tr("settings.schema.panels.launcher-show-navigation-labels.description"),
          {"shell", "launcher", "show_navigation_labels"}, ToggleSetting{cfg.shell.launcher.showNavigationLabels},
          "launcher navigation tabs labels text recent pinned all categories providers icons"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-settings-button.label"),
          tr("settings.schema.panels.launcher-show-settings-button.description"),
          {"shell", "launcher", "show_settings_button"}, ToggleSetting{cfg.shell.launcher.showSettingsButton},
          "launcher settings shortcut button"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-session-button.label"),
          tr("settings.schema.panels.launcher-show-session-button.description"),
          {"shell", "launcher", "show_session_button"}, ToggleSetting{cfg.shell.launcher.showSessionButton},
          "launcher session power shortcut button"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "display", tr("settings.schema.panels.launcher-show-icons.label"),
        tr("settings.schema.panels.launcher-show-icons.description"), {"shell", "launcher", "show_icons"},
        ToggleSetting{cfg.shell.launcher.showIcons}, "launcher app icons hide"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "display", tr("settings.schema.shared.colorize-icons.label"),
        tr("settings.schema.shared.colorize-icons.description"), {"shell", "launcher", "colorize_icons"},
        ToggleSetting{cfg.shell.launcher.colorizeIcons}, "colorize tint application icons"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "display", tr("settings.schema.panels.launcher-app-origin-indicator.label"),
        tr("settings.schema.panels.launcher-app-origin-indicator.description"),
        {"shell", "launcher", "show_app_origin_indicator"}, ToggleSetting{cfg.shell.launcher.showAppOriginIndicator},
        "launcher app origin indicator"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "display", tr("settings.schema.panels.launcher-grid.label"),
        tr("settings.schema.panels.launcher-grid.description"), {"shell", "launcher", "app_grid"},
        ToggleSetting{cfg.shell.launcher.appGrid}, "launcher grid icons view layout"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "display", tr("settings.schema.panels.launcher-compact.label"),
        tr("settings.schema.panels.launcher-compact.description"), {"shell", "launcher", "compact"},
        ToggleSetting{cfg.shell.launcher.compact}, "launcher compact rows dense"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "behavior", tr("settings.schema.panels.launcher-app-actions.label"),
        tr("settings.schema.panels.launcher-app-actions.description"), {"shell", "launcher", "show_app_actions"},
        ToggleSetting{cfg.shell.launcher.showAppActions}, "launcher app actions show"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "behavior", tr("settings.schema.panels.launcher-ignore-mouse.label"),
        tr("settings.schema.panels.launcher-ignore-mouse.description"), {"shell", "launcher", "ignore_mouse"},
        ToggleSetting{cfg.shell.launcher.ignoreMouse}, "launcher ignore disable mouse input pointer keyboard only"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "behavior", tr("settings.schema.panels.launcher-currency-exchange.label"),
        tr("settings.schema.panels.launcher-currency-exchange.description"),
        {"shell", "launcher", "fetch_exchange_rates"}, ToggleSetting{cfg.shell.launcher.fetchExchangeRates},
        "launcher currency exchange rates fetch online conversion"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "behavior", tr("settings.schema.panels.launcher-auto-paste.label"),
        tr("settings.schema.panels.launcher-auto-paste.description"), {"shell", "launcher", "auto_paste"},
        enumSelect(kClipboardAutoPasteModes, cfg.shell.launcher.autoPaste), "launcher auto paste"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "history", tr("settings.schema.panels.launcher-track-usage.label"),
        tr("settings.schema.panels.launcher-track-usage.description"), {"shell", "launcher", "track_usage"},
        ToggleSetting{cfg.shell.launcher.trackUsage}, "launcher track usage recent history frequency privacy"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "history", tr("settings.schema.panels.launcher-sort-by-usage.label"),
        tr("settings.schema.panels.launcher-sort-by-usage.description"), {"shell", "launcher", "sort_by_usage"},
        ToggleSetting{cfg.shell.launcher.sortByUsage, cfg.shell.launcher.trackUsage},
        "launcher sort usage recently used frequency"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "pinned-apps", tr("settings.schema.panels.launcher-pinned-apps.label"),
        tr("settings.schema.panels.launcher-pinned-apps.description"), {"shell", "launcher", "pinned"},
        ListSetting{.items = cfg.shell.launcher.pinned}, "launcher pinned apps"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "providers", tr("settings.schema.panels.launcher-prefix-character.label"),
        tr("settings.schema.panels.launcher-prefix-character.description"), {"shell", "launcher", "provider_prefix"},
        TextSetting{.value = cfg.shell.launcher.providerPrefix, .placeholder = "/"}, "launcher common prefix character"
    ));
    for (const auto& provider : kLauncherProviderSettings) {
      const auto it = std::ranges::find(cfg.shell.launcher.providers, provider.name, &LauncherProviderConfig::name);
      const LauncherProviderConfig* stored = it != cfg.shell.launcher.providers.end() ? &*it : nullptr;

      const std::string prefix = stored != nullptr ? stored->prefix : std::string();
      const bool global =
          stored != nullptr ? stored->global.value_or(provider.globalByDefault) : provider.globalByDefault;

      const std::string prefixKey = std::format("settings.schema.panels.launcher-prefix-{}", provider.name);
      const std::string globalKey = std::format("settings.schema.panels.launcher-global-{}", provider.name);

      entries.push_back(makeEntry(
          SettingsSection::Launcher, "providers", tr(prefixKey + ".label"), tr(prefixKey + ".description"),
          {"shell", "launcher", "providers", std::string(provider.name), "prefix"},
          TextSetting{.value = prefix, .placeholder = std::string(provider.prefixPlaceholder)},
          std::format("launcher {} prefix trigger", provider.name)
      ));
      entries.push_back(makeEntry(
          SettingsSection::Launcher, "providers", tr(globalKey + ".label"), tr(globalKey + ".description"),
          {"shell", "launcher", "providers", std::string(provider.name), "global"}, ToggleSetting{global},
          std::format("launcher {} global search unprefixed", provider.name)
      ));
      if (provider.name == "panels") {
        ListSetting ignoredPanels;
        ignoredPanels.items = cfg.shell.launcher.panels.ignored;
        const auto knownPanelIds = panel_catalog::allKnownIds();
        ignoredPanels.suggestedOptions.reserve(knownPanelIds.size());
        for (const auto& panelId : knownPanelIds) {
          ignoredPanels.suggestedOptions.push_back(SelectOption{panelId, panel_catalog::describe(panelId).title});
        }
        entries.push_back(makeEntry(
            SettingsSection::Launcher, "providers", tr("settings.schema.panels.launcher-ignored-panels.label"),
            tr("settings.schema.panels.launcher-ignored-panels.description"),
            {"shell", "launcher", "panels", "ignored"}, std::move(ignoredPanels),
            "launcher panels ignore hide exclude noise polkit setup wizard test"
        ));
      }
    }
  }

} // namespace settings::registry
